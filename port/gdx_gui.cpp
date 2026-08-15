#include "gdx_gui.h"

#include <imgui.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui_internal.h>

#include "ship/Context.h"
#include "ship/window/gui/Fonts.h"
#include "ship/window/gui/IconsFontAwesome4.h"

#include "gdx_imgui_nav.h"
#include "gdx_swkbd.h"

#include "port_log.h"
#include "gdx_dev_gates.h"

#if defined(__SWITCH__)
#define GDX_GUI_GL_PROBE 1
#include <glad/glad.h>
#elif defined(__linux__)
#define GDX_GUI_GL_PROBE 1
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengles2.h>
#endif

namespace {

ImFont* sFontStandard = nullptr;
ImFont* sFontLarge = nullptr;
ImFont* sFontMono = nullptr;

ImFont* LoadFontWithIcons(const std::string& path, float size) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig textConfig;
    textConfig.OversampleH = 2;
    textConfig.OversampleV = 2;

    ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &textConfig);
    if (font == nullptr) {
        return nullptr;
    }

    static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.GlyphMinAdvanceX = size * 2.0f / 3.0f;
    io.Fonts->AddFontFromMemoryCompressedBase85TTF(fontawesome_compressed_data_base85, size * 2.0f / 3.0f,
                                                   &iconConfig, iconRanges);
    return font;
}

} // namespace

void GdxFast3dGui::ImGuiWMInit() {
    // The renderer backend is initialized after this virtual returns, so fonts added here land in
    // its first font-atlas upload.
    Fast::Fast3dGui::ImGuiWMInit();

    const std::string fontRoot = Ship::Context::GetPathRelativeToAppDirectory("fonts/");
    sFontStandard = LoadFontWithIcons(fontRoot + "Montserrat-Regular.ttf", 18.0f);
    sFontLarge = LoadFontWithIcons(fontRoot + "Montserrat-Regular.ttf", 22.0f);
    sFontMono = LoadFontWithIcons(fontRoot + "Inconsolata-Regular.ttf", 17.0f);

    // Missing loose assets must never block boot. ImGui's default font already includes Font
    // Awesome through libultraship, so it is a complete fallback.
    ImFont* fallback = ImGui::GetIO().Fonts->Fonts.empty() ? nullptr : ImGui::GetIO().Fonts->Fonts[0];
    if (sFontStandard == nullptr) {
        sFontStandard = fallback;
    }
    if (sFontLarge == nullptr) {
        sFontLarge = sFontStandard;
    }
    if (sFontMono == nullptr) {
        sFontMono = sFontStandard;
    }
    if (sFontStandard != nullptr) {
        ImGui::GetIO().FontDefault = sFontStandard;
    }
}

// GDX_DIAG_IMGUI. What the renderer was actually handed last frame.
static void GdxDiagDrawData() {
    if (!gdx_dev_gate(GDX_GATE_DIAG_IMGUI)) {
        return;
    }
    static double sNext = 0.0;
    const double now = ImGui::GetTime();
    if (now < sNext) {
        return;
    }
    sNext = now + 1.0;

    const ImDrawData* dd = ImGui::GetDrawData();
    if (dd == nullptr || !dd->Valid) {
        gdx_port_logf("[imguidiag] drawdata: none/invalid\n");
        return;
    }
    gdx_port_logf("[imguidiag] drawdata: lists=%d vtx=%d idx=%d pos=(%.0f,%.0f) size=%.0fx%.0f "
                  "fbscale=%.2fx%.2f\n",
                  dd->CmdListsCount, dd->TotalVtxCount, dd->TotalIdxCount, dd->DisplayPos.x,
                  dd->DisplayPos.y, dd->DisplaySize.x, dd->DisplaySize.y, dd->FramebufferScale.x,
                  dd->FramebufferScale.y);

    // Per-list breakdown of what the renderer is handed, in submission order.
    static const struct {
        ImU32 color;
        const char* what;
    } kWanted[] = { { IM_COL32(9, 64, 209, 255), "section-fill(blue)" } };

    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList* list = dd->CmdLists[n];
        int found[IM_ARRAYSIZE(kWanted)] = { 0 };
        for (int v = 0; v < list->VtxBuffer.Size; v++) {
            for (int k = 0; k < IM_ARRAYSIZE(kWanted); k++) {
                if (list->VtxBuffer[v].col == kWanted[k].color) {
                    found[k]++;
                }
            }
        }
        gdx_port_logf("[imguidiag]   list[%d] owner=%s vtx=%d cmds=%d blue=%d\n", n,
                      (list->_OwnerName != nullptr) ? list->_OwnerName : "(fg/bg)", list->VtxBuffer.Size,
                      list->CmdBuffer.Size, found[0]);
        for (int k = 0; k < IM_ARRAYSIZE(kWanted); k++) {
            if (found[k] == 0) {
                continue;
            }
            for (int v = 0; v < list->VtxBuffer.Size; v++) {
                if (list->VtxBuffer[v].col != kWanted[k].color) {
                    continue;
                }
                const ImDrawVert& fv = list->VtxBuffer[v];
                gdx_port_logf("[imguidiag]     %s vtx[%d] pos=(%.1f,%.1f) uv=(%.5f,%.5f)\n", kWanted[k].what, v,
                              fv.pos.x, fv.pos.y, fv.uv.x, fv.uv.y);
                break;
            }
            for (int c = 0; c < list->CmdBuffer.Size; c++) {
                const ImDrawCmd& cmd = list->CmdBuffer[c];
                gdx_port_logf("[imguidiag]     %s cmd[%d] elems=%u clip=(%.1f,%.1f)-(%.1f,%.1f) tex=%p\n",
                              kWanted[k].what, c, cmd.ElemCount, cmd.ClipRect.x, cmd.ClipRect.y,
                              cmd.ClipRect.z, cmd.ClipRect.w, cmd.GetTexID());
            }
            break; // one command dump per list is enough to see the clip regime
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int atlasW = 0;
    int atlasH = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasW, &atlasH);
    const ImVec2 uv = io.Fonts->TexUvWhitePixel;
    gdx_port_logf("[imguidiag] atlas: %dx%d built=%d texid=%p uvwhite=(%.5f,%.5f)=texel(%d,%d) fonts=%d "
                  "bakedlines=%d\n",
                  atlasW, atlasH, io.Fonts->TexReady ? 1 : 0, io.Fonts->TexID, uv.x, uv.y,
                  static_cast<int>(uv.x * static_cast<float>(atlasW)),
                  static_cast<int>(uv.y * static_cast<float>(atlasH)), io.Fonts->Fonts.Size,
                  (io.Fonts->Flags & ImFontAtlasFlags_NoBakedLines) ? 0 : 1);

    if (pixels != nullptr && atlasW > 0 && atlasH > 0) {
        const int px = static_cast<int>(uv.x * static_cast<float>(atlasW));
        const int py = static_cast<int>(uv.y * static_cast<float>(atlasH));
        const unsigned char* t = pixels + (static_cast<size_t>(py) * atlasW + px) * 4;
        gdx_port_logf("[imguidiag] atlas white texel @(%d,%d) = %u,%u,%u,%u\n", px, py, t[0], t[1], t[2],
                      t[3]);
    }
}

#ifdef GDX_GUI_GL_PROBE
static void GdxDiagGlReadBlock(int x, int y, int w, int h, const char* what, const unsigned char* cpu,
                               int atlasW) {
    ImVector<unsigned char> gpu;
    gpu.resize(w * h * 4);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gpu.Data);

    int gpuOpaque = 0;
    int gpuNonZero = 0;
    int cpuOpaque = 0;
    int cpuNonZero = 0;
    for (int i = 0; i < w * h; i++) {
        const unsigned char* g = gpu.Data + i * 4;
        gpuOpaque += (g[0] == 255 && g[1] == 255 && g[2] == 255 && g[3] == 255) ? 1 : 0;
        gpuNonZero += (g[3] != 0) ? 1 : 0;
        if (cpu != nullptr) {
            const unsigned char* c = cpu + (static_cast<size_t>(y + i / w) * atlasW + (x + i % w)) * 4;
            cpuOpaque += (c[0] == 255 && c[1] == 255 && c[2] == 255 && c[3] == 255) ? 1 : 0;
            cpuNonZero += (c[3] != 0) ? 1 : 0;
        }
    }
    const unsigned char* g0 = gpu.Data;
    gdx_port_logf("[imguidiag] gl-readback %s @(%d,%d) %dx%d: gpu[0]=%u,%u,%u,%u gpu opaque=%d/%d "
                  "nonzero=%d cpu opaque=%d nonzero=%d\n",
                  what, x, y, w, h, g0[0], g0[1], g0[2], g0[3], gpuOpaque, w * h, gpuNonZero, cpuOpaque,
                  cpuNonZero);
}

static void GdxDiagGlProbe() {
    static bool sDone = false;
    if (sDone || !gdx_dev_gate(GDX_GATE_DIAG_IMGUI)) {
        return;
    }
    sDone = true;

    ImGuiIO& io = ImGui::GetIO();
    const GLuint tex = static_cast<GLuint>(reinterpret_cast<uintptr_t>(io.Fonts->TexID));
    gdx_port_logf("[imguidiag] gl: vendor='%s' renderer='%s' version='%s' glsl='%s' fonttex=%u\n",
                  reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
                  reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                  reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                  reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)), tex);
    if (tex == 0) {
        return;
    }

    GLint maxTex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);

    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint minFilter = 0;
    GLint magFilter = 0;
    GLint wrapS = 0;
    GLint wrapT = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapS);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrapT);
    gdx_port_logf("[imguidiag] gl: maxtexsize=%d minfilter=0x%X magfilter=0x%X wrap=(0x%X,0x%X)\n", maxTex,
                  minFilter, magFilter, wrapS, wrapT);

    unsigned char* pixels = nullptr;
    int atlasW = 0;
    int atlasH = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasW, &atlasH);

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        gdx_port_logf("[imguidiag] gl: font texture is not readable through an FBO (status=0x%X)\n", status);
    } else {
        GdxDiagGlReadBlock(32, 0, 8, 8, "imgui-white", pixels, atlasW);
        GdxDiagGlReadBlock(0, 64, 64, 64, "glyphs", pixels, atlasW);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glDeleteFramebuffers(1, &fbo);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        gdx_port_logf("[imguidiag] gl: probe left error 0x%X\n", err);
    }
}
#endif

static void GdxDiagFillGeometry() {
    static bool sDone = false;
    if (sDone || !gdx_dev_gate(GDX_GATE_DIAG_IMGUI)) {
        return;
    }
    ImDrawListSharedData* shared = ImGui::GetDrawListSharedData();
    if (shared == nullptr || shared->Font == nullptr) {
        return;
    }
    sDone = true;

    gdx_port_logf("[imguidiag] shared: fringe=%.4f circleerr=%.4f arccutoff=%.4f curvetol=%.4f "
                  "arc[0]=(%.4f,%.4f) arc[12]=(%.4f,%.4f) arc[24]=(%.4f,%.4f) arc[36]=(%.4f,%.4f)\n",
                  shared->InitialFringeScale, shared->CircleSegmentMaxError, shared->ArcFastRadiusCutoff,
                  shared->CurveTessellationTol, shared->ArcFastVtx[0].x, shared->ArcFastVtx[0].y,
                  shared->ArcFastVtx[12].x, shared->ArcFastVtx[12].y, shared->ArcFastVtx[24].x,
                  shared->ArcFastVtx[24].y, shared->ArcFastVtx[36].x, shared->ArcFastVtx[36].y);

    ImDrawList probe(shared);
    probe._ResetForNewFrame();
    probe.PushClipRectFullScreen();
    probe.PushTextureID(ImGui::GetIO().Fonts->TexID);

    const ImVec2 min(10.0f, 10.0f);
    const ImVec2 max(50.0f, 50.0f);
    static const struct {
        float rounding;
        ImDrawListFlags flags;
        const char* what;
    } kCases[] = { { 0.0f, ImDrawListFlags_None, "square/noAA" },
                   { 0.0f, ImDrawListFlags_AntiAliasedFill, "square/AA" },
                   { 6.0f, ImDrawListFlags_None, "round/noAA" },
                   { 6.0f, ImDrawListFlags_AntiAliasedFill, "round/AA" } };

    for (int c = 0; c < IM_ARRAYSIZE(kCases); c++) {
        probe._Path.Size = 0;
        probe.PathRect(min, max, kCases[c].rounding, ImDrawFlags_None);
        const int pathCount = probe._Path.Size;
        const ImVec2 p0 = (pathCount > 0) ? probe._Path[0] : ImVec2(0.0f, 0.0f);
        const ImVec2 p1 = (pathCount > 1) ? probe._Path[1] : ImVec2(0.0f, 0.0f);
        probe._Path.Size = 0;

        probe.Flags = kCases[c].flags;
        const int before = probe.VtxBuffer.Size;
        probe.AddRectFilled(min, max, IM_COL32(255, 0, 0, 255), kCases[c].rounding);
        const int after = probe.VtxBuffer.Size;
        const ImVec2 v0 = (after > before) ? probe.VtxBuffer[before].pos : ImVec2(0.0f, 0.0f);
        const ImVec2 v1 = (after > before + 1) ? probe.VtxBuffer[before + 1].pos : ImVec2(0.0f, 0.0f);
        gdx_port_logf("[imguidiag] fillprobe %-11s path=%d p0=(%.2f,%.2f) p1=(%.2f,%.2f) vtx=%d "
                      "v0=(%.2f,%.2f) v1=(%.2f,%.2f)\n",
                      kCases[c].what, pathCount, p0.x, p0.y, p1.x, p1.y, after - before, v0.x, v0.y, v1.x,
                      v1.y);
    }
}

void GdxFast3dGui::ImGuiRenderDrawData(ImDrawData* data) {
#ifdef GDX_GUI_GL_PROBE
    GdxDiagGlProbe();
#endif
    Fast::Fast3dGui::ImGuiRenderDrawData(data);
}

void GdxFast3dGui::ImGuiWMNewFrame() {
    GdxDiagDrawData();
    GdxDiagFillGeometry();
    Fast::Fast3dGui::ImGuiWMNewFrame();
    // Must land between the platform backend's gamepad poll (just above) and ImGui::NewFrame, which
    // the caller runs next: the feed both reads the backend's HasGamepad claim and queues key events
    // for this frame. See port/gdx_imgui_nav.h.
    gdx_imgui_nav_tick();
    // Reads the WantTextInput ImGui::NewFrame left behind last frame.
    gdx_swkbd_tick();
}

ImFont* GdxGuiFontStandard() {
    return sFontStandard;
}

ImFont* GdxGuiFontLarge() {
    return sFontLarge;
}

ImFont* GdxGuiFontMono() {
    return sFontMono;
}
