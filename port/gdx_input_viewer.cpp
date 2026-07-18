#include "gdx_input_viewer.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "fast/Fast3dGui.h"
#include "libultraship/bridge/consolevariablebridge.h"
#include "ship/Context.h"
#include "ship/window/Window.h"

extern "C" int gdx_input_viewer_state(unsigned short* outButtons, signed char* outStickX,
                                        signed char* outStickY);

namespace {

constexpr unsigned short kButtonA = 0x8000;
constexpr unsigned short kButtonB = 0x4000;
constexpr unsigned short kButtonZ = 0x2000;
constexpr unsigned short kButtonStart = 0x1000;
constexpr unsigned short kDpadUp = 0x0800;
constexpr unsigned short kDpadDown = 0x0400;
constexpr unsigned short kDpadLeft = 0x0200;
constexpr unsigned short kDpadRight = 0x0100;
constexpr unsigned short kButtonL = 0x0020;
constexpr unsigned short kButtonR = 0x0010;
constexpr unsigned short kCUp = 0x0008;
constexpr unsigned short kCDown = 0x0004;
constexpr unsigned short kCLeft = 0x0002;
constexpr unsigned short kCRight = 0x0001;

constexpr int kOutlineAlways = 0;
constexpr int kOutlineWhileReleased = 1;
constexpr int kOutlineWhilePressed = 2;
constexpr int kOutlineNever = 3;
constexpr float kTextureWidth = 327.0f;
constexpr float kTextureHeight = 175.0f;
constexpr float kMaximumAxis = 80.0f;

struct TextureLayer {
    const char* name;
    const char* path;
};

constexpr std::array<TextureLayer, 31> kTextureLayers = {{
    { "Gdx-IV-Background", "textures/buttons/InputViewerBackground.png" },
    { "Gdx-IV-A", "textures/buttons/ABtn.png" },
    { "Gdx-IV-A-Outline", "textures/buttons/ABtnOutline.png" },
    { "Gdx-IV-B", "textures/buttons/BBtn.png" },
    { "Gdx-IV-B-Outline", "textures/buttons/BBtnOutline.png" },
    { "Gdx-IV-L", "textures/buttons/LBtn.png" },
    { "Gdx-IV-L-Outline", "textures/buttons/LBtnOutline.png" },
    { "Gdx-IV-R", "textures/buttons/RBtn.png" },
    { "Gdx-IV-R-Outline", "textures/buttons/RBtnOutline.png" },
    { "Gdx-IV-Z", "textures/buttons/ZBtn.png" },
    { "Gdx-IV-Z-Outline", "textures/buttons/ZBtnOutline.png" },
    { "Gdx-IV-Start", "textures/buttons/StartBtn.png" },
    { "Gdx-IV-Start-Outline", "textures/buttons/StartBtnOutline.png" },
    { "Gdx-IV-CLeft", "textures/buttons/CLeft.png" },
    { "Gdx-IV-CLeft-Outline", "textures/buttons/CLeftOutline.png" },
    { "Gdx-IV-CRight", "textures/buttons/CRight.png" },
    { "Gdx-IV-CRight-Outline", "textures/buttons/CRightOutline.png" },
    { "Gdx-IV-CUp", "textures/buttons/CUp.png" },
    { "Gdx-IV-CUp-Outline", "textures/buttons/CUpOutline.png" },
    { "Gdx-IV-CDown", "textures/buttons/CDown.png" },
    { "Gdx-IV-CDown-Outline", "textures/buttons/CDownOutline.png" },
    { "Gdx-IV-Analog", "textures/buttons/AnalogStick.png" },
    { "Gdx-IV-Analog-Outline", "textures/buttons/AnalogStickOutline.png" },
    { "Gdx-IV-DLeft", "textures/buttons/DPadLeft.png" },
    { "Gdx-IV-DLeft-Outline", "textures/buttons/DPadLeftOutline.png" },
    { "Gdx-IV-DRight", "textures/buttons/DPadRight.png" },
    { "Gdx-IV-DRight-Outline", "textures/buttons/DPadRightOutline.png" },
    { "Gdx-IV-DUp", "textures/buttons/DPadUp.png" },
    { "Gdx-IV-DUp-Outline", "textures/buttons/DPadUpOutline.png" },
    { "Gdx-IV-DDown", "textures/buttons/DPadDown.png" },
    { "Gdx-IV-DDown-Outline", "textures/buttons/DPadDownOutline.png" },
}};

bool ShouldDrawOutline(int mode, bool pressed) {
    return mode == kOutlineAlways || (mode == kOutlineWhileReleased && !pressed) ||
           (mode == kOutlineWhilePressed && pressed);
}

} // namespace

GdxInputViewer::GdxInputViewer() : Ship::GuiWindow("gOpenWindows.InputViewer", false, "Input Viewer") {
    CVarRegisterFloat("gInputViewer.Scale", 1.0f);
    CVarRegisterFloat("gInputViewer.Opacity", 1.0f);
    CVarRegisterInteger("gInputViewer.EnableDragging", 1);
    CVarRegisterInteger("gInputViewer.ShowBackground", 1);
    CVarRegisterInteger("gInputViewer.ShowAnalogValues", 0);
    CVarRegisterInteger("gInputViewer.ShowDpad", 0);
    CVarRegisterInteger("gInputViewer.ButtonOutlineMode", kOutlineWhileReleased);
    CVarRegisterInteger("gInputViewer.StyleVersion", 0);

    // The previous vector prototype had a boxed top-left layout and different defaults. Migrate it
    // once so existing users immediately receive SoH's lower-right transparent presentation.
    if (CVarGetInteger("gInputViewer.StyleVersion", 0) < 1) {
        CVarSetFloat("gInputViewer.Opacity", 1.0f);
        CVarSetInteger("gInputViewer.ShowBackground", 1);
        CVarSetInteger("gInputViewer.ShowAnalogValues", 0);
        CVarSetInteger("gInputViewer.ShowDpad", 0);
        CVarSetInteger("gInputViewer.ButtonOutlineMode", kOutlineWhileReleased);
        CVarSetInteger("gInputViewer.StyleVersion", 1);

        auto context = Ship::Context::GetInstance();
        if (context != nullptr && context->GetWindow() != nullptr && context->GetWindow()->GetGui() != nullptr) {
            context->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
    }
}

void GdxInputViewer::InitElement() {
}

void GdxInputViewer::UpdateElement() {
}

void GdxInputViewer::Draw() {
    if (!IsVisible()) {
        return;
    }

    DrawElement();
    SyncVisibilityConsoleVariable();
}

void GdxInputViewer::DrawElement() {
    auto context = Ship::Context::GetInstance();
    if (context == nullptr || context->GetWindow() == nullptr) {
        return;
    }
    auto gui = std::dynamic_pointer_cast<Fast::Fast3dGui>(context->GetWindow()->GetGui());
    if (gui == nullptr) {
        return;
    }

    if (!mTexturesLoaded) {
        for (const TextureLayer& layer : kTextureLayers) {
            gui->LoadTextureFromRawImage(layer.name, layer.path);
        }
        mTexturesLoaded = true;
    }

    const float scale = std::clamp(CVarGetFloat("gInputViewer.Scale", 1.0f), 0.5f, 2.5f);
    const float opacity = std::clamp(CVarGetFloat("gInputViewer.Opacity", 1.0f), 0.2f, 1.0f);
    const bool showAnalogValues = CVarGetInteger("gInputViewer.ShowAnalogValues", 0) != 0;
    const ImVec2 textureSize(kTextureWidth * scale, kTextureHeight * scale);
    const float valuesHeight = showAnalogValues ? ImGui::GetTextLineHeightWithSpacing() : 0.0f;
    const ImVec2 windowSize(textureSize.x + 20.0f, textureSize.y + valuesHeight + 20.0f);
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowContentSize(ImVec2(textureSize.x, textureSize.y + valuesHeight));
    ImGui::SetNextWindowPos(viewport->WorkPos + viewport->WorkSize - textureSize - ImVec2(30.0f, 30.0f),
                            ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking;
    if (CVarGetInteger("gInputViewer.EnableDragging", 1) == 0) {
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("Input Viewer##GdxSohOverlayV2", nullptr, flags)) {
        ImGui::SetCursorPos(ImVec2(10.0f, 10.0f));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(std::round(opacity * 255.0f)));

        auto drawLayer = [&](const char* name, ImVec2 offset = ImVec2(0.0f, 0.0f)) {
            ImTextureID texture = gui->GetTextureByName(name);
            if (texture != nullptr) {
                drawList->AddImage(texture, origin + offset, origin + offset + textureSize, ImVec2(0.0f, 0.0f),
                                   ImVec2(1.0f, 1.0f), tint);
            }
        };

        if (CVarGetInteger("gInputViewer.ShowBackground", 1) != 0) {
            drawLayer("Gdx-IV-Background");
        }

        unsigned short buttons = 0;
        signed char stickX = 0;
        signed char stickY = 0;
        gdx_input_viewer_state(&buttons, &stickX, &stickY);
        const int outlineMode = std::clamp(CVarGetInteger("gInputViewer.ButtonOutlineMode", kOutlineWhileReleased),
                                           kOutlineAlways, kOutlineNever);

        auto drawButton = [&](const char* active, const char* outline, unsigned short mask) {
            const bool pressed = (buttons & mask) != 0;
            if (ShouldDrawOutline(outlineMode, pressed)) {
                drawLayer(outline);
            }
            if (pressed) {
                drawLayer(active);
            }
        };

        drawButton("Gdx-IV-B", "Gdx-IV-B-Outline", kButtonB);
        drawButton("Gdx-IV-A", "Gdx-IV-A-Outline", kButtonA);
        drawButton("Gdx-IV-CUp", "Gdx-IV-CUp-Outline", kCUp);
        drawButton("Gdx-IV-CLeft", "Gdx-IV-CLeft-Outline", kCLeft);
        drawButton("Gdx-IV-CRight", "Gdx-IV-CRight-Outline", kCRight);
        drawButton("Gdx-IV-CDown", "Gdx-IV-CDown-Outline", kCDown);
        drawButton("Gdx-IV-L", "Gdx-IV-L-Outline", kButtonL);
        drawButton("Gdx-IV-R", "Gdx-IV-R-Outline", kButtonR);
        drawButton("Gdx-IV-Z", "Gdx-IV-Z-Outline", kButtonZ);
        drawButton("Gdx-IV-Start", "Gdx-IV-Start-Outline", kButtonStart);

        if (CVarGetInteger("gInputViewer.ShowDpad", 0) != 0) {
            drawButton("Gdx-IV-DLeft", "Gdx-IV-DLeft-Outline", kDpadLeft);
            drawButton("Gdx-IV-DRight", "Gdx-IV-DRight-Outline", kDpadRight);
            drawButton("Gdx-IV-DUp", "Gdx-IV-DUp-Outline", kDpadUp);
            drawButton("Gdx-IV-DDown", "Gdx-IV-DDown-Outline", kDpadDown);
        }

        drawLayer("Gdx-IV-Analog-Outline");
        constexpr float kAnalogMovement = 12.0f;
        const ImVec2 analogOffset(kAnalogMovement * (static_cast<float>(stickX) / kMaximumAxis) * scale,
                                  -kAnalogMovement * (static_cast<float>(stickY) / kMaximumAxis) * scale);
        drawLayer("Gdx-IV-Analog", analogOffset);

        ImGui::SetCursorPos(ImVec2(10.0f, 10.0f));
        ImGui::InvisibleButton("##GdxInputViewerCanvas", textureSize);
        if (showAnalogValues) {
            ImGui::SetCursorPos(ImVec2(20.0f, textureSize.y + 10.0f));
            ImGui::Text("Stick  X %+d   Y %+d", static_cast<int>(stickX), static_cast<int>(stickY));
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
