// G-Diffuser — runtime O2R asset extraction launcher. See gdx_extract_launch.h for the contract map.

#include "gdx_extract_launch.h"
#include "port_log.h"

// C3 golden-archive expectations, emitted by the Wave 1-C harness generator. The single code-level
// contract between first-boot (1-B, this file) and the harness (1-C) is these two macro names:
//   GDX_O2R_EXPECTED_SHA256       — hex string, the deterministic archive's SHA-256.
//   GDX_O2R_EXPECTED_ENTRY_COUNT  — integer, the archive's zip entry count (4240).
// If the generated header is not present yet (generator not wired), fall back to inert placeholders
// so this TU still compiles: with a placeholder SHA-256 that no real archive can match, every
// extraction "fails validation" and boot degrades to the raw-ROM fallback (C6) — never a hard break.
#if defined(__has_include)
#if __has_include("gen/gdx_o2r_expected.h")
#include "gen/gdx_o2r_expected.h"
#endif
#endif
#ifndef GDX_O2R_EXPECTED_SHA256
#ifdef _MSC_VER
#pragma message("gdx_extract_launch.cpp: gen/gdx_o2r_expected.h not found - using placeholder O2R expectations (extraction always falls back to raw). Run the 1-C harness to generate it.")
#else
#warning "gen/gdx_o2r_expected.h not found - using placeholder O2R expectations (extraction always falls back to raw). Run the 1-C harness to generate it."
#endif
#define GDX_O2R_EXPECTED_SHA256 "0000000000000000000000000000000000000000000000000000000000000000"
#endif
#ifndef GDX_O2R_EXPECTED_ENTRY_COUNT
#define GDX_O2R_EXPECTED_ENTRY_COUNT 0
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h> // PROGRESS_CLASS / PBM_SETMARQUEE (modeless progress dialog)
#include <thread>
#include <mutex>
#include <atomic>
#else
#include <cerrno>
#include <csignal>   // kill() for the extraction hang guard
#include <ctime>     // time() deadline for the hang guard
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace gdx {
namespace {

// ── Constants ────────────────────────────────────────────────────────────────────────────────────

// The installed archive is named after the game (fzerox.o2r, matching fzerox.sav and the
// SoH/Starship convention of game-named archives). The extractor child itself always writes
// Torch's fixed output name into the temp dir; the atomic install renames it.
constexpr const char* kArchiveName = "fzerox.o2r";
constexpr const char* kExtractorOutputName = "generic.o2r";
constexpr const char* kSidecarName = "gdx_extract_state.cfg";
constexpr const char* kRecipesDirName = "decomp-recipes";
constexpr const char* kConfigYmlName = "config.yml";
constexpr const char* kTorchHashName = "torch.hash.yml"; // stray artifact the extractor may leave (C2)
constexpr const char* kTempSubdir = ".gdx_extract.tmp";  // same-filesystem staging dir under dataDir

#ifdef _WIN32
constexpr const char* kExtractBinaryName = "gdx-extract.exe";
#else
constexpr const char* kExtractBinaryName = "gdx-extract";
#endif

// C1 fallback ROM SHA-1 (US rev0, the key decomp/config.yml uses). Preferred source is config.yml at
// runtime (recipes = single source of truth); this constant is only used if config.yml cannot be read.
constexpr const char* kExpectedRomSha1Fallback = "5f658e88ffa9de23cba6986a8fd3d3a90d7b4340";

// C4 version-entry contract: Torch stamps generic.o2r's game version = the US-rev0 ROM CRC.
constexpr std::uint32_t kExpectedRomCrc = 0x78D90EB3u;

// Disk-space guard (C8): require >= 3x an estimated archive size before spawning. The exact archive
// size is not a compile-time constant here, so use a conservative upper estimate.
constexpr std::uintmax_t kEstimatedArchiveBytes = 128u * 1024u * 1024u; // ~128 MiB estimate.
constexpr std::uintmax_t kRequiredFreeBytes = 3u * kEstimatedArchiveBytes;

// ── Small helpers ────────────────────────────────────────────────────────────────────────────────

std::string toLowerHex(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

FILE* openBinary(const fs::path& p, const char* mode) {
    FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, p.string().c_str(), mode) != 0) {
        return nullptr;
    }
#else
    f = std::fopen(p.string().c_str(), mode);
#endif
    return f;
}

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

// ── Async setup-GUI progress sink ────────────────────────────────────────────────────────────────
// When extraction is driven from the in-window setup flow (GdxExtractStartAsync), the background
// worker publishes its latest stage line + last actionable error here and the ImGui screen polls it.
// The blocking pre-window path (DevLayout/SetupComplete) leaves the poll untouched; publishing into
// this sink from the shared stdout readers is harmless there (nobody polls). Function-local static
// so there is exactly one instance regardless of translation-unit init order.
struct AsyncExtractState {
    std::mutex mtx;
    std::string stage;              // latest stage line (guarded by mtx)
    std::string lastError;          // last actionable error line (guarded by mtx)
    std::atomic<int> phase{0};      // 0 = Idle, 1 = Running, 2 = Done
    std::atomic<int> outcome{0};    // ExtractOutcome as int; valid only when phase == 2
    std::atomic<bool> suppressDialog{false}; // suppress the Windows marquee dialog when GUI-driven
    std::thread worker;
};

AsyncExtractState& asyncState() {
    static AsyncExtractState s;
    return s;
}

void gdxAsyncPublishStage(const std::string& line) {
    AsyncExtractState& s = asyncState();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.stage = line;
}

void gdxAsyncPublishError(const std::string& msg) {
    AsyncExtractState& s = asyncState();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.lastError = msg;
}

// ── Vendored SHA-1 (public domain; Steve Reid's reference, condensed) ─────────────────────────────
// Used only for ROM identity validation (C1). Not security-sensitive — SHA-1 is the identifier the
// decomp recipes key on, so we match Torch/config.yml exactly.

struct Sha1Ctx {
    std::uint32_t state[5];
    std::uint64_t count; // bits
    std::uint8_t buffer[64];
};

inline std::uint32_t rol32(std::uint32_t v, int b) {
    return (v << b) | (v >> (32 - b));
}

void sha1Transform(std::uint32_t state[5], const std::uint8_t buffer[64]) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(buffer[i * 4]) << 24) |
               (static_cast<std::uint32_t>(buffer[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(buffer[i * 4 + 2]) << 8) |
               (static_cast<std::uint32_t>(buffer[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        std::uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1Init(Sha1Ctx& ctx) {
    ctx.state[0] = 0x67452301u;
    ctx.state[1] = 0xEFCDAB89u;
    ctx.state[2] = 0x98BADCFEu;
    ctx.state[3] = 0x10325476u;
    ctx.state[4] = 0xC3D2E1F0u;
    ctx.count = 0;
}

void sha1Update(Sha1Ctx& ctx, const std::uint8_t* data, size_t len) {
    size_t idx = static_cast<size_t>((ctx.count >> 3) & 63u);
    ctx.count += static_cast<std::uint64_t>(len) << 3;
    size_t part = 64 - idx;
    size_t i = 0;
    if (len >= part) {
        std::memcpy(&ctx.buffer[idx], data, part);
        sha1Transform(ctx.state, ctx.buffer);
        for (i = part; i + 63 < len; i += 64) {
            sha1Transform(ctx.state, &data[i]);
        }
        idx = 0;
    }
    std::memcpy(&ctx.buffer[idx], &data[i], len - i);
}

void sha1Final(Sha1Ctx& ctx, std::uint8_t out[20]) {
    std::uint8_t finalCount[8];
    for (int i = 0; i < 8; ++i) {
        finalCount[i] = static_cast<std::uint8_t>((ctx.count >> ((7 - i) * 8)) & 0xFF);
    }
    std::uint8_t c = 0x80;
    sha1Update(ctx, &c, 1);
    c = 0x00;
    while ((ctx.count & 504u) != 448u) { // pad to 56 bytes mod 64 (in bits: 448 mod 512)
        sha1Update(ctx, &c, 1);
    }
    sha1Update(ctx, finalCount, 8);
    for (int i = 0; i < 20; ++i) {
        out[i] = static_cast<std::uint8_t>((ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF);
    }
}

// ── Vendored SHA-256 (public domain reference) ───────────────────────────────────────────────────
// Used for the strongest install gate (C5): archive SHA-256 == GDX_O2R_EXPECTED_SHA256.

struct Sha256Ctx {
    std::uint32_t state[8];
    std::uint64_t count; // bytes
    std::uint8_t buffer[64];
};

const std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline std::uint32_t ror32(std::uint32_t v, int b) {
    return (v >> b) | (v << (32 - b));
}

void sha256Transform(std::uint32_t state[8], const std::uint8_t block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<std::uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        std::uint32_t ch = (e & f) ^ ((~e) & g);
        std::uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
        std::uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256Init(Sha256Ctx& ctx) {
    ctx.state[0] = 0x6a09e667u;
    ctx.state[1] = 0xbb67ae85u;
    ctx.state[2] = 0x3c6ef372u;
    ctx.state[3] = 0xa54ff53au;
    ctx.state[4] = 0x510e527fu;
    ctx.state[5] = 0x9b05688cu;
    ctx.state[6] = 0x1f83d9abu;
    ctx.state[7] = 0x5be0cd19u;
    ctx.count = 0;
}

void sha256Update(Sha256Ctx& ctx, const std::uint8_t* data, size_t len) {
    size_t idx = static_cast<size_t>(ctx.count & 63u);
    ctx.count += len;
    size_t i = 0;
    if (idx > 0) {
        size_t part = 64 - idx;
        if (len < part) {
            std::memcpy(&ctx.buffer[idx], data, len);
            return;
        }
        std::memcpy(&ctx.buffer[idx], data, part);
        sha256Transform(ctx.state, ctx.buffer);
        i = part;
    }
    for (; i + 63 < len; i += 64) {
        sha256Transform(ctx.state, &data[i]);
    }
    std::memcpy(ctx.buffer, &data[i], len - i);
}

void sha256Final(Sha256Ctx& ctx, std::uint8_t out[32]) {
    std::uint64_t bits = ctx.count << 3;
    std::uint8_t c = 0x80;
    sha256Update(ctx, &c, 1);
    c = 0x00;
    while ((ctx.count & 63u) != 56u) {
        sha256Update(ctx, &c, 1);
    }
    std::uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) {
        lenBytes[i] = static_cast<std::uint8_t>((bits >> ((7 - i) * 8)) & 0xFF);
    }
    sha256Update(ctx, lenBytes, 8);
    for (int i = 0; i < 32; ++i) {
        out[i] = static_cast<std::uint8_t>((ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF);
    }
}

std::string toHex(const std::uint8_t* bytes, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(d[bytes[i] >> 4]);
        out.push_back(d[bytes[i] & 0xF]);
    }
    return out;
}

// Streamed file hashing (avoids loading multi-MiB files into RAM). Returns empty on read error.
std::string sha1File(const fs::path& p) {
    FILE* f = openBinary(p, "rb");
    if (f == nullptr) {
        return {};
    }
    Sha1Ctx ctx;
    sha1Init(ctx);
    std::vector<std::uint8_t> buf(1u << 16);
    for (;;) {
        size_t got = std::fread(buf.data(), 1, buf.size(), f);
        if (got > 0) {
            sha1Update(ctx, buf.data(), got);
        }
        if (got < buf.size()) {
            break;
        }
    }
    bool err = (std::ferror(f) != 0);
    std::fclose(f);
    if (err) {
        return {};
    }
    std::uint8_t out[20];
    sha1Final(ctx, out);
    return toHex(out, 20);
}

std::string sha256File(const fs::path& p) {
    FILE* f = openBinary(p, "rb");
    if (f == nullptr) {
        return {};
    }
    Sha256Ctx ctx;
    sha256Init(ctx);
    std::vector<std::uint8_t> buf(1u << 16);
    for (;;) {
        size_t got = std::fread(buf.data(), 1, buf.size(), f);
        if (got > 0) {
            sha256Update(ctx, buf.data(), got);
        }
        if (got < buf.size()) {
            break;
        }
    }
    bool err = (std::ferror(f) != 0);
    std::fclose(f);
    if (err) {
        return {};
    }
    std::uint8_t out[32];
    sha256Final(ctx, out);
    return toHex(out, 32);
}

// ── config.yml expected-ROM-SHA-1 parse (C1: recipes are the single source of truth) ─────────────
// The recipe config keys each recipe tree on the ROM SHA-1. We want the key whose block declares
// `path: assets/yaml/us/rev0`. Returns lowercase hex, or empty if not found / unreadable.
std::string expectedRomSha1FromConfig(const fs::path& configYml) {
    FILE* f = openBinary(configYml, "rb");
    if (f == nullptr) {
        return {};
    }
    std::string currentKey;
    std::string result;
    char line[4096];
    auto trim = [](std::string s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    };
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string raw(line);
        // A top-level key starts at column 0 and ends in ':'. In this file that key is the SHA-1.
        if (!raw.empty() && raw[0] != ' ' && raw[0] != '\t' && raw[0] != '#') {
            std::string t = trim(raw);
            size_t colon = t.find(':');
            if (colon != std::string::npos) {
                currentKey = toLowerHex(trim(t.substr(0, colon)));
            }
            continue;
        }
        // Inside a block: look for `path: assets/yaml/us/rev0`.
        std::string t = trim(raw);
        if (t.rfind("path:", 0) == 0) {
            std::string val = trim(t.substr(5));
            if (val == "assets/yaml/us/rev0" && currentKey.size() == 40) {
                result = currentKey;
                break;
            }
        }
    }
    std::fclose(f);
    return result;
}

// ── Zip End-Of-Central-Directory entry count (C5 step 2) ─────────────────────────────────────────
// Scan backwards for the EOCD signature (0x06054b50) and read "total number of central directory
// records" (u16 @ off 12). This is the RAW record count — NOT a unique-name set. The archive keys
// duplicate names on purpose (e.g. course_track_gfx/*: 664 duplicate records over 3,576 unique names,
// 4,240 records total), so counting a name set would spuriously report 3,576 and fail a correct
// archive. We deliberately count every central-directory record, including duplicates. 4,240 is well
// below the u16 limit, so a ZIP64 EOCD is not expected; if the classic field reads 0xFFFF we report
// -1 (unknown) and let the SHA-256 gate decide.
long zipEntryCount(const fs::path& p) {
    FILE* f = openBinary(p, "rb");
    if (f == nullptr) {
        return -1;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    if (size < 22) { // minimum EOCD size
        std::fclose(f);
        return -1;
    }
    // The EOCD comment can be up to 65535 bytes; scan the last 64 KiB + 22.
    long scan = size < (65535 + 22) ? size : (65535 + 22);
    std::fseek(f, size - scan, SEEK_SET);
    std::vector<std::uint8_t> buf(static_cast<size_t>(scan));
    size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) {
        return -1;
    }
    for (long i = static_cast<long>(got) - 22; i >= 0; --i) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
            // EOCD offset 10 (u16): total number of central directory records across all disks
            // (offset 8 is the per-disk count, offset 12 is the CD *size* — not the count). Every
            // record is counted, duplicates included — the count GDX_O2R_EXPECTED_ENTRY_COUNT expects.
            std::uint16_t total = static_cast<std::uint16_t>(buf[i + 10] | (buf[i + 11] << 8));
            if (total == 0xFFFF) {
                return -1; // ZIP64 sentinel — not expected for this archive.
            }
            return static_cast<long>(total);
        }
    }
    return -1;
}

// ── Completion sidecar (C7): gdx_extract_state.cfg — key=value, same pattern as gdx_firstboot.cfg ─

struct ExtractState {
    std::string extractorVersion;
    std::string recipeFingerprint;
    std::string romSha1;
    std::string archiveSha256;
    std::uintmax_t romSize = 0;
    std::int64_t romMtime = 0;
    bool valid = false; // true only if the file was read and parsed
};

ExtractState loadSidecar(const fs::path& dataDir) {
    ExtractState st;
    FILE* f = openBinary(dataDir / kSidecarName, "rb");
    if (f == nullptr) {
        return st;
    }
    char line[4096];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        if (s.empty() || s[0] == '#') {
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string k = s.substr(0, eq);
        std::string v = s.substr(eq + 1);
        if (k == "extractor_version") {
            st.extractorVersion = v;
        } else if (k == "recipe_fingerprint") {
            st.recipeFingerprint = v;
        } else if (k == "rom_sha1") {
            st.romSha1 = toLowerHex(v);
        } else if (k == "archive_sha256") {
            st.archiveSha256 = toLowerHex(v);
        } else if (k == "rom_size") {
            st.romSize = std::strtoull(v.c_str(), nullptr, 10);
        } else if (k == "rom_mtime") {
            st.romMtime = static_cast<std::int64_t>(std::strtoll(v.c_str(), nullptr, 10));
        }
    }
    std::fclose(f);
    st.valid = true;
    return st;
}

bool saveSidecar(const fs::path& dataDir, const ExtractState& st) {
    FILE* f = openBinary(dataDir / kSidecarName, "wb");
    if (f == nullptr) {
        gdx_port_logf("[extract] WARNING: could not write %s; warm-boot cache will be rebuilt next run\n",
                      (dataDir / kSidecarName).string().c_str());
        return false;
    }
    std::fprintf(f, "# G-Diffuser O2R extraction state. Auto-generated; safe to delete to force re-extract.\n");
    std::fprintf(f, "extractor_version=%s\n", st.extractorVersion.c_str());
    std::fprintf(f, "recipe_fingerprint=%s\n", st.recipeFingerprint.c_str());
    std::fprintf(f, "rom_sha1=%s\n", st.romSha1.c_str());
    std::fprintf(f, "archive_sha256=%s\n", st.archiveSha256.c_str());
    std::fprintf(f, "rom_size=%llu\n", static_cast<unsigned long long>(st.romSize));
    std::fprintf(f, "rom_mtime=%lld\n", static_cast<long long>(st.romMtime));
    std::fclose(f);
    return true;
}

std::int64_t fileMtime(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) {
        return 0;
    }
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

// ── Atomic install: rename temp -> final, with a Windows sharing-violation retry loop (C5) ────────
bool atomicReplace(const fs::path& tmp, const fs::path& final) {
#ifdef _WIN32
    std::wstring wt = tmp.wstring();
    std::wstring wf = final.wstring();
    const int kMaxRetries = 12;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        if (MoveFileExW(wt.c_str(), wf.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED || err == ERROR_LOCK_VIOLATION) {
            // Classic on-access AV lock on a freshly written archive in %APPDATA%. Back off and retry.
            gdx_port_logf("[extract] install retry %d/%d (Windows error %lu; antivirus may be scanning "
                          "the new archive)\n",
                          attempt + 1, kMaxRetries, static_cast<unsigned long>(err));
            Sleep(250 * (attempt + 1));
            continue;
        }
        gdx_port_logf("[extract] ERROR: could not install archive (Windows error %lu)\n",
                      static_cast<unsigned long>(err));
        return false;
    }
    gdx_port_logf("[extract] ERROR: install kept failing with a sharing violation. Your antivirus may "
                  "be blocking G-Diffuser from writing %s.\n",
                  final.string().c_str());
    return false;
#else
    std::error_code ec;
    fs::rename(tmp, final, ec); // atomic within the same filesystem
    if (!ec) {
        return true;
    }
    gdx_port_logf("[extract] ERROR: could not install archive: %s\n", ec.message().c_str());
    return false;
#endif
}

// ── Best-effort parse of extractor stdout for diagnostic sidecar fields (C7) ─────────────────────
// The extractor prints a recipe fingerprint and version on stdout. Exact wording is owned by the
// extractor (Wave 1-A); we scan case-insensitively for "fingerprint" / "extractor" tokens and grab
// the trailing value. These fields are diagnostic only — the golden SHA-256 is the gating authority —
// so an unparsed line never affects correctness.
void scanStdoutLine(const std::string& line, ExtractState& st) {
    auto lower = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return s;
    };
    std::string lc = lower(line);
    auto tailToken = [&](const std::string& s) {
        size_t e = s.find_last_not_of(" \t\r\n");
        if (e == std::string::npos) {
            return std::string();
        }
        size_t b = s.find_last_of(" \t=:", e);
        return s.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b));
    };
    if (st.recipeFingerprint.empty() && lc.find("fingerprint") != std::string::npos) {
        st.recipeFingerprint = tailToken(line);
    } else if (st.extractorVersion.empty() && lc.find("gdx-extract") != std::string::npos &&
               lc.find("version") != std::string::npos) {
        st.extractorVersion = tailToken(line);
    }
}

// ── Child-process launch ─────────────────────────────────────────────────────────────────────────
// Runs gdx-extract, streaming its stdout: to the log on every platform, and (Windows) to a modeless
// marquee progress dialog. Returns true if the process spawned and exited 0; fills exitCode + state.

#ifdef _WIN32

// Latest stage line, shared reader-thread -> UI-thread. A WM_TIMER refresh reads it (no PostMessage
// marshalling needed for a status string).
struct ProgressShared {
    std::mutex mtx;
    std::string stage;
};

LRESULT CALLBACK progressWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool runExtractorWindows(const fs::path& exe, const std::wstring& cmdLine, const fs::path& workDir,
                         int& exitCode, ExtractState& state) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        gdx_port_logf("[extract] ERROR: CreatePipe failed (%lu)\n",
                      static_cast<unsigned long>(GetLastError()));
        return false;
    }
    // The read end must NOT be inherited by the child.
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdLine; // CreateProcessW may modify the buffer
    std::wstring wWorkDir = workDir.wstring();

    BOOL ok = CreateProcessW(exe.wstring().c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, wWorkDir.c_str(), &si, &pi);
    CloseHandle(writePipe); // parent keeps only the read end
    if (!ok) {
        gdx_port_logf("[extract] ERROR: could not launch %s (Windows error %lu)\n",
                      exe.string().c_str(), static_cast<unsigned long>(GetLastError()));
        CloseHandle(readPipe);
        return false;
    }

    // ── Modeless marquee progress dialog ─────────────────────────────────────────────────────────
    // Suppressed when the extraction is driven from the in-window setup screen: that ImGui screen
    // shows the stage line + an animated indicator itself, so a second native dialog would be
    // redundant (and would steal focus from the game window). We still pump the message loop below.
    const bool showDialog = !asyncState().suppressDialog.load();
    HWND wnd = nullptr;
    HWND text = nullptr;
    HWND bar = nullptr;
    if (showDialog) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    static const wchar_t* kClass = L"GdxExtractProgress";
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = progressWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW; explicit W form (build is not UNICODE)
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc); // ignore "already registered" on a second run

    const int w = 460;
    const int h = 140;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass,
                          L"G-Diffuser — preparing game assets",
                          WS_POPUPWINDOW | WS_CAPTION, sx, sy, w, h, nullptr, nullptr, hInst, nullptr);
    if (wnd != nullptr) {
        text = CreateWindowExW(0, L"STATIC",
                               L"Extracting game assets from your ROM.\nThis happens once and may take "
                               L"a minute…",
                               WS_CHILD | WS_VISIBLE, 16, 12, w - 40, 48, wnd, nullptr, hInst, nullptr);
        bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                              WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 16, 68, w - 40, 22, wnd, nullptr,
                              hInst, nullptr);
        if (bar != nullptr) {
            SendMessageW(bar, PBM_SETMARQUEE, TRUE, 60);
        }
        ShowWindow(wnd, SW_SHOWNORMAL);
        UpdateWindow(wnd);
    }
    } // if (showDialog)

    ProgressShared shared;
    std::atomic<bool> readerDone{false};
    std::thread reader([&]() {
        std::string acc;
        char buf[4096];
        DWORD n = 0;
        for (;;) {
            if (!ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) || n == 0) {
                break;
            }
            acc.append(buf, n);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r')) {
                    line.pop_back();
                }
                if (!line.empty()) {
                    gdx_port_logf("[extract] %s\n", line.c_str());
                    scanStdoutLine(line, state);
                    gdxAsyncPublishStage(line); // feed the in-window setup screen (no-op when unused)
                    std::lock_guard<std::mutex> lk(shared.mtx);
                    shared.stage = line;
                }
            }
        }
        if (!acc.empty()) {
            gdx_port_logf("[extract] %s\n", acc.c_str());
            scanStdoutLine(acc, state);
        }
        readerDone.store(true);
    });

    // Pump the message loop, refreshing the status line, until the child exits.
    // C6 requires that boot NEVER blocks: a hung (not failed) extractor is killed
    // after a hard deadline and treated as a failed extraction (raw fallback).
    // Normal extraction takes ~2 seconds; 120s is generously beyond any slow disk.
    const ULONGLONG deadline = GetTickCount64() + 120u * 1000u;
    bool timedOut = false;
    for (;;) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 50);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (text != nullptr) {
            std::string stage;
            {
                std::lock_guard<std::mutex> lk(shared.mtx);
                stage = shared.stage;
            }
            if (!stage.empty()) {
                std::wstring ws(stage.begin(), stage.end());
                SetWindowTextW(text, ws.c_str());
            }
        }
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (GetTickCount64() >= deadline) {
            gdx_port_logf("[extract] ERROR: extractor exceeded the %us deadline; terminating child\n", 120u);
            TerminateProcess(pi.hProcess, 124u);
            WaitForSingleObject(pi.hProcess, 5000);
            timedOut = true;
            break;
        }
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    exitCode = timedOut ? 124 : static_cast<int>(code);

    if (reader.joinable()) {
        reader.join();
    }
    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (wnd != nullptr) {
        DestroyWindow(wnd);
    }
    return exitCode == 0;
}

#else // POSIX

bool runExtractorPosix(const fs::path& exe, const std::vector<std::string>& args, const fs::path& workDir,
                       int& exitCode, ExtractState& state) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        gdx_port_logf("[extract] ERROR: pipe() failed: %s\n", std::strerror(errno));
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        gdx_port_logf("[extract] ERROR: fork() failed: %s\n", std::strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        // Child: redirect stdout+stderr to the pipe, chdir to the work dir, exec.
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (!workDir.empty()) {
            if (chdir(workDir.string().c_str()) != 0) {
                _exit(127);
            }
        }
        std::vector<char*> argv;
        std::string exeStr = exe.string();
        argv.push_back(const_cast<char*>(exeStr.c_str()));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execv(exeStr.c_str(), argv.data());
        _exit(127); // exec failed
    }
    // Parent: stream the child's stdout to the log (Linux progress UX is log-only, per C8).
    // C6 hang guard: bound the whole read with a hard deadline via select(); a child that
    // stops producing output and never exits is killed and treated as a failed extraction.
    close(pipefd[1]);
    std::string acc;
    char buf[4096];
    const time_t deadline = time(nullptr) + 120;
    bool timedOut = false;
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        struct timeval tv = { 1, 0 };
        int sel = select(pipefd[0] + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0 && errno == EINTR) {
            continue;
        }
        if (sel > 0) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0) {
                break; // EOF (child closed stdout) or read error
            }
            acc.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (!line.empty()) {
                    gdx_port_logf("[extract] %s\n", line.c_str());
                    scanStdoutLine(line, state);
                    gdxAsyncPublishStage(line); // feed the in-window setup screen (no-op when unused)
                }
            }
        }
        if (time(nullptr) >= deadline) {
            gdx_port_logf("[extract] ERROR: extractor exceeded the 120s deadline; killing child\n");
            kill(pid, SIGKILL);
            timedOut = true;
            break;
        }
    }
    if (!acc.empty()) {
        gdx_port_logf("[extract] %s\n", acc.c_str());
        scanStdoutLine(acc, state);
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timedOut) {
        exitCode = 124;
    } else if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    } else {
        exitCode = 1;
    }
    return exitCode == 0;
}

#endif

// ── Cleanup helpers ──────────────────────────────────────────────────────────────────────────────
void removeIfExists(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

// ── Extraction orchestration ─────────────────────────────────────────────────────────────────────
// Runs the extractor into a same-filesystem temp dir, validates the output (C5), and atomically
// installs it. Returns Extracted on full success, FailedRawFallback otherwise. Never throws.
ExtractOutcome runExtraction(const fs::path& dataDir, const fs::path& romPath, const fs::path& exeDir,
                             const std::string& romSha1) {
    std::error_code ec;

    const fs::path extractBin = exeDir / kExtractBinaryName;
    const fs::path recipesDir = exeDir / kRecipesDirName;
    if (!fileExists(extractBin)) {
        gdx_port_logf("[extract] extractor component missing (%s). Cannot build %s — booting from the "
                      "raw ROM. Reinstall G-Diffuser to restore the extractor.\n",
                      extractBin.string().c_str(), kArchiveName);
        gdxAsyncPublishError("The extractor component (gdx-extract) is missing. Reinstall G-Diffuser.");
        return ExtractOutcome::FailedRawFallback;
    }
    if (!fs::is_directory(recipesDir, ec)) {
        ec.clear();
        gdx_port_logf("[extract] recipe data missing (%s). Cannot build %s — booting from the raw ROM.\n",
                      recipesDir.string().c_str(), kArchiveName);
        gdxAsyncPublishError("The recipe data (decomp-recipes) is missing. Reinstall G-Diffuser.");
        return ExtractOutcome::FailedRawFallback;
    }

    // Disk-space guard (C8): require >= 3x the estimated archive size.
    auto space = fs::space(dataDir, ec);
    if (!ec && space.available < kRequiredFreeBytes) {
        gdx_port_logf("[extract] not enough free disk space in %s: %.0f MB available, ~%.0f MB needed. "
                      "Free some space and relaunch. Booting from the raw ROM for now.\n",
                      dataDir.string().c_str(),
                      static_cast<double>(space.available) / (1024.0 * 1024.0),
                      static_cast<double>(kRequiredFreeBytes) / (1024.0 * 1024.0));
        gdxAsyncPublishError("Not enough free disk space to build the asset archive. Free some space "
                             "and retry.");
        return ExtractOutcome::FailedRawFallback;
    }
    ec.clear();

    // Fresh temp staging dir on the same filesystem as the final archive (so the install is a rename).
    const fs::path tmpDir = dataDir / kTempSubdir;
    removeIfExists(tmpDir);
    fs::create_directories(tmpDir, ec);
    if (ec) {
        gdx_port_logf("[extract] ERROR: could not create temp dir %s: %s. Booting from the raw ROM.\n",
                      tmpDir.string().c_str(), ec.message().c_str());
        return ExtractOutcome::FailedRawFallback;
    }
    ec.clear();

    // MAX_PATH guard (Windows, and harmless elsewhere): the deepest recipe path appends
    // "/assets/yaml/us/rev0/<longest yaml name>" (~54 chars) to the recipes dir, and the classic
    // Windows limit is 260 including the terminator. A deep install directory pushes the extractor's
    // plain ifstream opens past it (verified failure at exactly 260 chars). When the projected path
    // is too long, stage a copy of the small (~455 KB) recipe tree under the system temp dir, which
    // is short on real systems, and hand THAT to the extractor. Cleaned up after the run.
    fs::path effectiveRecipes = recipesDir;
    fs::path stagedRecipes;
    constexpr size_t kDeepestRecipeSuffix = 54; // "/assets/yaml/us/rev0/expansion_kit_textures_beta.yaml"
    if (recipesDir.string().size() + kDeepestRecipeSuffix >= 248) {
        std::error_code sec;
        fs::path shortBase = fs::temp_directory_path(sec);
        if (!sec && shortBase.string().size() + 16 + kDeepestRecipeSuffix < 248) {
            stagedRecipes = shortBase / "gdx-recipes";
            removeIfExists(stagedRecipes);
#ifdef _WIN32
            // The staging copy must itself read >=260-char source paths; std::filesystem does not
            // add the \\?\ long-path prefix on Windows, so provide it explicitly for the source.
            fs::path copySrc = fs::absolute(recipesDir, sec);
            if (!sec) {
                copySrc = fs::path(L"\\\\?\\" + copySrc.wstring());
            } else {
                sec.clear();
                copySrc = recipesDir;
            }
#else
            const fs::path& copySrc = recipesDir;
#endif
            fs::copy(copySrc, stagedRecipes, fs::copy_options::recursive, sec);
            if (!sec) {
                effectiveRecipes = stagedRecipes;
                gdx_port_logf("[extract] recipes path is too deep for MAX_PATH; staged a copy at %s\n",
                              stagedRecipes.string().c_str());
            } else {
                removeIfExists(stagedRecipes);
                stagedRecipes.clear();
                gdx_port_logf("[extract] WARNING: recipes path may exceed MAX_PATH and staging failed "
                              "(%s); attempting extraction in place.\n",
                              sec.message().c_str());
            }
        } else {
            gdx_port_logf("[extract] WARNING: install path is extremely deep; extraction may fail on "
                          "MAX_PATH. Consider moving G-Diffuser to a shorter folder.\n");
        }
    }

    // Invocation shape (C2): gdx-extract o2r <rom.z64> -s <recipesDir> -d <tmpDir> -u <version>.
    // Never pass -v (its debug mode dumps entries to CWD). The version stamped into the archive is the
    // US-rev0 ROM CRC (C4); Torch derives the CRC itself, and -u carries the numeric game version.
    char versionArg[16];
    std::snprintf(versionArg, sizeof(versionArg), "%u", static_cast<unsigned>(kExpectedRomCrc));

    gdx_port_logf("[extract] building %s from the ROM (one-time). recipes=%s out=%s\n", kArchiveName,
                  recipesDir.string().c_str(), tmpDir.string().c_str());

    ExtractState state;
    int exitCode = 1;
    bool ok = false;
#ifdef _WIN32
    // Build a single command line; quote paths that may contain spaces.
    auto q = [](const std::wstring& s) { return L"\"" + s + L"\""; };
    std::wstring cmd = q(extractBin.wstring());
    cmd += L" o2r ";
    cmd += q(romPath.wstring());
    cmd += L" -s ";
    cmd += q(effectiveRecipes.wstring());
    cmd += L" -d ";
    cmd += q(tmpDir.wstring());
    cmd += L" -u ";
    cmd += std::wstring(versionArg, versionArg + std::strlen(versionArg));
    ok = runExtractorWindows(extractBin, cmd, tmpDir, exitCode, state);
#else
    std::vector<std::string> args = { "o2r",
                                      romPath.string(),
                                      "-s",
                                      effectiveRecipes.string(),
                                      "-d",
                                      tmpDir.string(),
                                      "-u",
                                      versionArg };
    ok = runExtractorPosix(extractBin, args, tmpDir, exitCode, state);
#endif

    // Delete the stray torch.hash.yml the extractor may drop in the output dir and the CWD (C2),
    // and any MAX_PATH staging copy of the recipes.
    removeIfExists(tmpDir / kTorchHashName);
    removeIfExists(dataDir / kTorchHashName);
    if (!stagedRecipes.empty()) {
        removeIfExists(stagedRecipes);
    }

    if (!ok) {
        gdx_port_logf("[extract] ERROR: extractor exited with code %d. Keeping any previous %s; booting "
                      "from the raw ROM.\n",
                      exitCode, kArchiveName);
        gdxAsyncPublishError(exitCode == 124
                                 ? std::string("The extractor timed out (exceeded the 120s deadline).")
                                 : ("The extractor failed (exit code " + std::to_string(exitCode) + ")."));
        removeIfExists(tmpDir);
        return ExtractOutcome::FailedRawFallback;
    }

    // ── C5 validation (in order) ─────────────────────────────────────────────────────────────────
    const fs::path producedArchive = tmpDir / kExtractorOutputName;
    if (!fileExists(producedArchive)) {
        gdx_port_logf("[extract] ERROR: extractor exited 0 but produced no %s. Booting from the raw ROM.\n",
                      kExtractorOutputName);
        removeIfExists(tmpDir);
        return ExtractOutcome::FailedRawFallback;
    }

    // Step 2: entry count.
    long entries = zipEntryCount(producedArchive);
    if (entries >= 0 && entries != static_cast<long>(GDX_O2R_EXPECTED_ENTRY_COUNT)) {
        gdx_port_logf("[extract] ERROR: extracted archive has %ld entries, expected %d. Discarding; "
                      "booting from the raw ROM.\n",
                      entries, static_cast<int>(GDX_O2R_EXPECTED_ENTRY_COUNT));
        gdxAsyncPublishError("The extracted archive failed validation (entry-count mismatch). This "
                             "build's recipes/extractor may not match.");
        removeIfExists(tmpDir);
        return ExtractOutcome::FailedRawFallback;
    }

    // Step 3: archive SHA-256 == golden constant. This is the strongest gate and, because extraction
    // is deterministic (C2), an exact match. It also transitively proves the version entry (C5 step 4)
    // and full key completeness (C3/C6), since the archive is byte-identical to the golden reference.
    std::string archiveSha = sha256File(producedArchive);
    std::string expectedSha = toLowerHex(std::string(GDX_O2R_EXPECTED_SHA256));
    if (archiveSha.empty()) {
        gdx_port_logf("[extract] ERROR: could not hash the extracted archive. Booting from the raw ROM.\n");
        removeIfExists(tmpDir);
        return ExtractOutcome::FailedRawFallback;
    }
    if (archiveSha != expectedSha) {
        gdx_port_logf("[extract] ERROR: extracted archive SHA-256 mismatch.\n"
                      "  got:      %s\n  expected: %s\n"
                      "The archive does not match this build's golden reference (recipe/extractor "
                      "drift, or a corrupt build). Discarding; booting from the raw ROM.\n",
                      archiveSha.c_str(), expectedSha.c_str());
        gdxAsyncPublishError("The extracted archive does not match this build's golden reference "
                             "(recipe/extractor drift or a corrupt build).");
        removeIfExists(tmpDir);
        return ExtractOutcome::FailedRawFallback;
    }

    // ── Atomic install (C5): rename temp -> final, preserving any old archive until this succeeds. ─
    const fs::path finalArchive = dataDir / kArchiveName;
    if (!atomicReplace(producedArchive, finalArchive)) {
        gdx_port_logf("[extract] ERROR: install failed; the previous %s (if any) is untouched. Booting "
                      "from the raw ROM.\n",
                      kArchiveName);
        removeIfExists(tmpDir);
        return ExtractOutcome::FailedRawFallback;
    }
    removeIfExists(tmpDir);

    // Persist the completion sidecar (C7).
    state.romSha1 = toLowerHex(romSha1);
    state.archiveSha256 = archiveSha;
    state.romSize = fs::file_size(romPath, ec);
    ec.clear();
    state.romMtime = fileMtime(romPath);
    saveSidecar(dataDir, state);

    gdx_port_logf("[extract] installed %s (%ld entries, sha256 verified).\n", kArchiveName,
                  entries >= 0 ? entries : static_cast<long>(GDX_O2R_EXPECTED_ENTRY_COUNT));
    return ExtractOutcome::Extracted;
}

} // namespace

ExtractOutcome GdxExtractEnsureArchive(const char* dataDirC, const char* romPathC, const char* exeDirC) {
    if (dataDirC == nullptr || dataDirC[0] == '\0' || romPathC == nullptr || romPathC[0] == '\0' ||
        exeDirC == nullptr || exeDirC[0] == '\0') {
        gdx_port_logf("[extract] missing data/ROM/exe path; skipping extraction (raw-ROM fallback).\n");
        return ExtractOutcome::FailedRawFallback;
    }
    std::error_code ec;
    const fs::path dataDir(dataDirC);
    const fs::path romPath(romPathC);
    const fs::path exeDir(exeDirC);

    // ── C7 warm-boot check, FIRST and ROM-independent ────────────────────────────────────────────
    // A present archive that hashes to the golden constant is valid regardless of the ROM's current
    // state — accept it before any ROM checks. This also closes the F3 audit finding: without this
    // ordering, an early ROM-related return would leave a stale/corrupt archive in place for the
    // mount path (whose C4 gate checks only the version entry, which bit rot can preserve).
    const fs::path archive = dataDir / kArchiveName;
    const std::string expectedSha = toLowerHex(std::string(GDX_O2R_EXPECTED_SHA256));
    if (fileExists(archive)) {
        std::string actual = sha256File(archive);
        if (!actual.empty() && actual == expectedSha) {
            ExtractState sidecar = loadSidecar(dataDir);
            if (!sidecar.valid || sidecar.archiveSha256 != expectedSha) {
                ExtractState st = sidecar;
                st.archiveSha256 = actual;
                if (fileExists(romPath)) { // ROM fields are best-effort bookkeeping
                    st.romSha1 = toLowerHex(sha1File(romPath));
                    st.romSize = fs::file_size(romPath, ec);
                    ec.clear();
                    st.romMtime = fileMtime(romPath);
                }
                saveSidecar(dataDir, st);
                gdx_port_logf("[extract] existing %s matches the golden reference; recorded state.\n",
                              kArchiveName);
            }
            return ExtractOutcome::UpToDate;
        }
        gdx_port_logf("[extract] existing %s does not match this build's golden reference.\n", kArchiveName);
    }

    // Any archive still present past this point is NON-golden. If we end up unable to extract a
    // replacement, it must not be left where the mount path would pick it up (F3): quarantine it.
    auto failRawQuarantine = [&]() {
        if (fileExists(archive)) {
            const fs::path bad = dataDir / (std::string(kArchiveName) + ".bad");
            std::error_code qec;
            fs::remove(bad, qec);
            qec.clear();
            fs::rename(archive, bad, qec);
            if (qec) {
                gdx_port_logf("[extract] WARNING: could not quarantine the stale %s (%s); the version "
                              "gate is the remaining defense.\n",
                              kArchiveName, qec.message().c_str());
            } else {
                gdx_port_logf("[extract] quarantined the stale archive as %s.bad (raw-ROM fallback).\n",
                              kArchiveName);
            }
        }
        return ExtractOutcome::FailedRawFallback;
    };

    if (!fileExists(romPath)) {
        gdx_port_logf("[extract] ROM not found at %s; skipping extraction (raw-ROM fallback).\n",
                      romPath.string().c_str());
        return failRawQuarantine();
    }

    // ── C1: validate ROM identity BEFORE any spawn ───────────────────────────────────────────────
    const fs::path configYml = exeDir / kRecipesDirName / kConfigYmlName;
    std::string expectedRomSha1 = expectedRomSha1FromConfig(configYml);
    if (expectedRomSha1.empty()) {
        expectedRomSha1 = kExpectedRomSha1Fallback;
        gdx_port_logf("[extract] WARNING: could not read expected ROM hash from %s; using the built-in "
                      "US-rev0 constant.\n",
                      configYml.string().c_str());
    }
    std::string romSha1 = sha1File(romPath);
    if (romSha1.empty()) {
        gdx_port_logf("[extract] ERROR: could not hash the ROM at %s; skipping extraction (raw-ROM "
                      "fallback).\n",
                      romPath.string().c_str());
        return failRawQuarantine();
    }
    if (toLowerHex(romSha1) != toLowerHex(expectedRomSha1)) {
        gdx_port_logf("[extract] ERROR: ROM does not match the US-rev0 recipe profile — extraction "
                      "skipped, booting from the raw ROM.\n"
                      "  ROM sha1:      %s\n  expected sha1: %s\n"
                      "Extraction only supports the big-endian US rev0 (.z64) cartridge.\n",
                      toLowerHex(romSha1).c_str(), toLowerHex(expectedRomSha1).c_str());
        gdxAsyncPublishError("This ROM does not match the US rev0 profile. Extraction only supports "
                             "the big-endian US rev0 (.z64) cartridge.");
        return failRawQuarantine();
    }

    // ── Extract ──────────────────────────────────────────────────────────────────────────────────
    // Any pre-existing non-golden archive stays in place while the replacement is produced; the
    // atomic install renames over it only after full C5 validation. If extraction fails, the
    // runExtraction failure path returns FailedRawFallback — quarantine there too so a stale
    // archive never reaches the mount.
    ExtractOutcome outcome = runExtraction(dataDir, romPath, exeDir, romSha1);
    if (outcome == ExtractOutcome::FailedRawFallback) {
        return failRawQuarantine();
    }
    return outcome;
}

const char* GdxExtractOutcomeString(ExtractOutcome outcome) {
    switch (outcome) {
        case ExtractOutcome::UpToDate:
            return "up to date (fzerox.o2r already valid)";
        case ExtractOutcome::Extracted:
            return "extracted and installed fzerox.o2r";
        case ExtractOutcome::FailedRawFallback:
            return "not available — booting from the raw ROM";
    }
    return "unknown";
}

// ── Async driver (see header) ────────────────────────────────────────────────────────────────────

void GdxExtractStartAsync(const char* dataDir, const char* romPath, const char* exeDir,
                          bool suppressNativeDialog) {
    AsyncExtractState& s = asyncState();
    if (s.phase.load() == 1) {
        gdx_port_logf("[extract] async extraction already running; ignoring duplicate start\n");
        return;
    }
    // Reclaim a previously finished worker before reusing the slot (e.g. a Retry after a failure).
    if (s.worker.joinable()) {
        s.worker.join();
    }
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.stage.clear();
        s.lastError.clear();
    }
    s.suppressDialog.store(suppressNativeDialog);
    s.outcome.store(static_cast<int>(ExtractOutcome::FailedRawFallback));
    s.phase.store(1);

    std::string d = (dataDir != nullptr) ? dataDir : "";
    std::string r = (romPath != nullptr) ? romPath : "";
    std::string e = (exeDir != nullptr) ? exeDir : "";
    s.worker = std::thread([d, r, e]() {
        ExtractOutcome o = GdxExtractEnsureArchive(d.c_str(), r.c_str(), e.c_str());
        AsyncExtractState& st = asyncState();
        st.outcome.store(static_cast<int>(o));
        st.phase.store(2); // publish outcome before flipping phase to Done
    });
}

ExtractProgress GdxExtractPollStatus() {
    AsyncExtractState& s = asyncState();
    ExtractProgress p;
    const int ph = s.phase.load();
    p.phase = (ph == 2) ? ExtractPhase::Done : (ph == 1) ? ExtractPhase::Running : ExtractPhase::Idle;
    p.outcome = static_cast<ExtractOutcome>(s.outcome.load());
    std::lock_guard<std::mutex> lk(s.mtx);
    p.stage = s.stage;
    p.lastError = s.lastError;
    return p;
}

void GdxExtractResetAsync() {
    AsyncExtractState& s = asyncState();
    if (s.worker.joinable()) {
        s.worker.join();
    }
    s.phase.store(0);
    s.suppressDialog.store(false);
    std::lock_guard<std::mutex> lk(s.mtx);
    s.stage.clear();
    s.lastError.clear();
}

std::string GdxExtractFileSha1(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return {};
    }
    return toLowerHex(sha1File(fs::path(path)));
}

std::string GdxExtractExpectedRomSha1(const char* exeDir) {
    if (exeDir != nullptr && exeDir[0] != '\0') {
        std::string fromConfig =
            expectedRomSha1FromConfig(fs::path(exeDir) / kRecipesDirName / kConfigYmlName);
        if (!fromConfig.empty()) {
            return toLowerHex(fromConfig);
        }
    }
    return toLowerHex(std::string(kExpectedRomSha1Fallback));
}

} // namespace gdx
