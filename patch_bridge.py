import os
import re

filepath = r"C:\Users\Vicente\Desktop\Proyectos Mios\Forks and Codes\Github Repositories\G-Diffuser\port\n64_gfx_bridge.cpp"

with open(filepath, 'r') as f:
    content = f.read()

bswap_utils = """
static inline uint32_t Byteswap32(uint32_t x) {
    return ((x & 0xFF000000) >> 24) |
           ((x & 0x00FF0000) >> 8)  |
           ((x & 0x0000FF00) << 8)  |
           ((x & 0x000000FF) << 24);
}

static inline uint16_t Byteswap16(uint16_t x) {
    return ((x & 0xFF00) >> 8) | ((x & 0x00FF) << 8);
}

static inline bool IsLikelyBigEndianDisplayList(const N64Gfx* source, size_t readableLimit) {
    if (readableLimit == 0) return false;
    uint32_t w0 = source[0].w0;
    uint8_t opL = w0 >> 24;
    uint8_t opB = w0 & 0xFF;
    if (opL == 0 && opB != 0) return true;
    if ((opB >= 0xB0 || opB == 0x01 || opB == 0x04) && opL < 0x20) return true;
    return false;
}
"""

content = content.replace("namespace {", "namespace {\n" + bswap_utils)

vtx_copy = """
    uintptr_t MakePersistentVtxCopy(uintptr_t source, size_t count) {
        if (source == 0 || count == 0) {
            return 0;
        }
        size_t requiredBytes = count * 16;
        auto alloc = std::make_unique<uint8_t[]>(requiredBytes);
        uint8_t* out = alloc.get();
        mPersistentAllocations.push_back(std::move(alloc));
        
        const uint8_t* in = reinterpret_cast<const uint8_t*>(source);
        for (size_t i = 0; i < count; i++) {
            uint16_t* out_s = reinterpret_cast<uint16_t*>(out + i * 16);
            const uint16_t* in_s = reinterpret_cast<const uint16_t*>(in + i * 16);
            out_s[0] = Byteswap16(in_s[0]);
            out_s[1] = Byteswap16(in_s[1]);
            out_s[2] = Byteswap16(in_s[2]);
            out_s[3] = Byteswap16(in_s[3]);
            out_s[4] = Byteswap16(in_s[4]);
            out_s[5] = Byteswap16(in_s[5]);
            out[i * 16 + 12] = in[i * 16 + 12];
            out[i * 16 + 13] = in[i * 16 + 13];
            out[i * 16 + 14] = in[i * 16 + 14];
            out[i * 16 + 15] = in[i * 16 + 15];
        }
        return reinterpret_cast<uintptr_t>(out);
    }
"""

content = content.replace("uintptr_t MakePersistentRawTextureCopy", vtx_copy + "\n    uintptr_t MakePersistentRawTextureCopy")

content = content.replace(
    "uintptr_t TranslateTexturePointer(uint32_t raw, const N64Gfx* source, size_t index, size_t limit) {",
    "uintptr_t TranslateTexturePointer(uint32_t raw, const N64Gfx* source, size_t index, size_t limit, bool isBig) {"
)

content = content.replace(
    "            const N64Gfx& in = source[i];\n            const uint8_t op = Opcode(in.w0);",
    "            N64Gfx in = source[i];\n            if (isBig) {\n                in.w0 = Byteswap32(in.w0);\n                in.w1 = Byteswap32(in.w1);\n            }\n            const uint8_t op = Opcode(in.w0);"
)

texture_copy_mod = """
        uintptr_t outPtr = MakePersistentRawTextureCopy(translated, required);
        if (isBig && outPtr != 0) {
            if (fmt == kImageFmtRgba || fmt == kImageFmtIa || fmt == kImageFmtI || fmt == kImageFmtColorIndex) {
                if (size == kImageSize16b) {
                    uint16_t* ptr16 = reinterpret_cast<uint16_t*>(outPtr);
                    for (size_t i = 0; i < required / 2; i++) ptr16[i] = Byteswap16(ptr16[i]);
                } else if (size == kImageSize32b) {
                    uint32_t* ptr32 = reinterpret_cast<uint32_t*>(outPtr);
                    for (size_t i = 0; i < required / 4; i++) ptr32[i] = Byteswap32(ptr32[i]);
                }
            }
        }
        return outPtr;
"""
content = content.replace("return MakePersistentRawTextureCopy(translated, required);", texture_copy_mod.strip("\n"))

known_limit_mod = """
    size_t KnownCommandLimit(const N64Gfx* source) const {
        if (source == nullptr) return 0;
        const size_t readableLimit = ReadableCommandLimit(source);
        if (readableLimit == 0) return 0;
        bool isBig = IsLikelyBigEndianDisplayList(source, readableLimit);
        size_t limit = 0;
        for (size_t i = 0; i < readableLimit; i++) {
            limit++;
            uint32_t w0 = source[i].w0;
            if (isBig) w0 = Byteswap32(w0);
            if (Opcode(w0) == kOpEndDl) return limit;
        }
        return 0;
    }
"""
content = re.sub(r'    size_t KnownCommandLimit\(const N64Gfx\* source\) const \{.*?return 0;\n    \}', known_limit_mod.strip('\n'), content, flags=re.DOTALL)

convert_list_header = """
    Fast::F3DGfx* ConvertList(const N64Gfx* source, size_t explicitLimit = 0) {
        if (source == nullptr) {
            return nullptr;
        }
        auto cached = mLists.find(source);
        if (cached != mLists.end()) {
            return cached->second->commands.data();
        }
        auto list = std::make_unique<ConvertedList>();
        ConvertedList* listPtr = list.get();
        mLists.emplace(source, std::move(list));
        if (mStats != nullptr) {
            mStats->convertedLists++;
        }
        const size_t limit = EffectiveLimit(source, explicitLimit);
        listPtr->commands.reserve(std::min<size_t>(limit, 4096));
        bool isBig = IsLikelyBigEndianDisplayList(source, limit);
        for (size_t i = 0; i < limit; i++) {
            N64Gfx in = source[i];
            if (isBig) {
                in.w0 = Byteswap32(in.w0);
                in.w1 = Byteswap32(in.w1);
            }
"""
content = re.sub(r'    Fast::F3DGfx\* ConvertList\(const N64Gfx\* source, size_t explicitLimit = 0\) \{.*?        for \(size_t i = 0; i < limit; i\+\+\) \{\n            const N64Gfx& in = source\[i\];', convert_list_header.strip('\n'), content, flags=re.DOTALL)

content = content.replace("TranslateTexturePointer(in.w1, source, i, limit)", "TranslateTexturePointer(in.w1, source, i, limit, isBig)")

vtx_handler_mod = """
                case kOpVtx:
                case kOpMtx:
                case kOpMovemem:
                    outW1 = TranslateDataPointer(in.w1);
                    if (outW1 != 0 && isBig && op == kOpVtx) {
                        outW1 = MakePersistentVtxCopy(outW1, (WordParam(outW0) >> 12) & 0xFF);
                    }
                    break;
"""
content = re.sub(r'                case kOpVtx:\n                case kOpMtx:\n                case kOpMovemem:\n                    outW1 = TranslateDataPointer\(in.w1\);\n                    break;', vtx_handler_mod.strip('\n'), content, flags=re.DOTALL)

with open(filepath, 'w') as f:
    f.write(content)
print("Updated n64_gfx_bridge.cpp")
