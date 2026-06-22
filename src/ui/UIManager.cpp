#include "ui/UIManager.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <glm/mat3x3.hpp>
#include <opencv2/imgproc.hpp>
#include <portable-file-dialogs.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace
{
constexpr ImGuiWindowFlags kSidebarFlags = ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoCollapse;

constexpr float kSidebarW = 320.0f;
constexpr float kMenuBarH = 20.0f;
constexpr float kStatusBarH = 24.0f;

const char *const kBindingLabels[] = {"Position", "IK Target (2-bone)", "Look At (rotation only)",
                                      "Hint (detect only)"};

// Tokyo Night palette — https://github.com/enkia/tokyo-night-vscode-theme
void ApplyTokyoNightTheme()
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec4 *c = style.Colors;

    const ImVec4 bg       = ImVec4(0.102f, 0.106f, 0.149f, 1.00f); // #1a1b26
    const ImVec4 bgDark   = ImVec4(0.086f, 0.086f, 0.118f, 1.00f); // #16161e
    const ImVec4 bgHigh   = ImVec4(0.161f, 0.180f, 0.259f, 1.00f); // #292e42
    const ImVec4 surface  = ImVec4(0.231f, 0.259f, 0.380f, 1.00f); // #3b4261
    const ImVec4 terminal = ImVec4(0.255f, 0.282f, 0.408f, 1.00f); // #414868
    const ImVec4 fg       = ImVec4(0.753f, 0.792f, 0.961f, 1.00f); // #c0caf5
    const ImVec4 fgDark   = ImVec4(0.663f, 0.694f, 0.839f, 1.00f); // #a9b1d6
    const ImVec4 comment  = ImVec4(0.337f, 0.373f, 0.537f, 1.00f); // #565f89
    const ImVec4 blue     = ImVec4(0.478f, 0.635f, 0.969f, 1.00f); // #7aa2f7
    const ImVec4 cyan     = ImVec4(0.490f, 0.812f, 1.000f, 1.00f); // #7dcfff
    const ImVec4 magenta  = ImVec4(0.733f, 0.604f, 0.969f, 1.00f); // #bb9af7
    const ImVec4 red      = ImVec4(0.969f, 0.463f, 0.557f, 1.00f); // #f7768e

    c[ImGuiCol_Text]                 = fg;
    c[ImGuiCol_TextDisabled]         = comment;
    c[ImGuiCol_WindowBg]             = bg;
    c[ImGuiCol_ChildBg]              = bg;
    c[ImGuiCol_PopupBg]              = bgDark;
    c[ImGuiCol_Border]               = ImVec4(surface.x, surface.y, surface.z, 0.60f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = bgDark;
    c[ImGuiCol_FrameBgHovered]       = bgHigh;
    c[ImGuiCol_FrameBgActive]        = surface;
    c[ImGuiCol_TitleBg]              = bgDark;
    c[ImGuiCol_TitleBgActive]        = bgHigh;
    c[ImGuiCol_TitleBgCollapsed]     = bgDark;
    c[ImGuiCol_MenuBarBg]            = bgDark;
    c[ImGuiCol_ScrollbarBg]          = bgDark;
    c[ImGuiCol_ScrollbarGrab]        = surface;
    c[ImGuiCol_ScrollbarGrabHovered] = terminal;
    c[ImGuiCol_ScrollbarGrabActive]  = comment;
    c[ImGuiCol_CheckMark]            = blue;
    c[ImGuiCol_SliderGrab]           = blue;
    c[ImGuiCol_SliderGrabActive]     = cyan;
    c[ImGuiCol_Button]               = bgHigh;
    c[ImGuiCol_ButtonHovered]        = surface;
    c[ImGuiCol_ButtonActive]         = terminal;
    c[ImGuiCol_Header]               = bgHigh;
    c[ImGuiCol_HeaderHovered]        = surface;
    c[ImGuiCol_HeaderActive]         = terminal;
    c[ImGuiCol_Separator]            = surface;
    c[ImGuiCol_SeparatorHovered]     = blue;
    c[ImGuiCol_SeparatorActive]      = cyan;
    c[ImGuiCol_ResizeGrip]           = surface;
    c[ImGuiCol_ResizeGripHovered]    = blue;
    c[ImGuiCol_ResizeGripActive]     = cyan;
    c[ImGuiCol_Tab]                  = bgDark;
    c[ImGuiCol_TabHovered]           = surface;
    c[ImGuiCol_TabActive]            = bgHigh;
    c[ImGuiCol_TabUnfocused]         = bgDark;
    c[ImGuiCol_TabUnfocusedActive]   = bgHigh;
    c[ImGuiCol_PlotLines]            = blue;
    c[ImGuiCol_PlotLinesHovered]     = cyan;
    c[ImGuiCol_PlotHistogram]        = magenta;
    c[ImGuiCol_PlotHistogramHovered] = red;
    c[ImGuiCol_TextSelectedBg]       = ImVec4(blue.x, blue.y, blue.z, 0.35f);
    c[ImGuiCol_NavHighlight]         = blue;
    c[ImGuiCol_DragDropTarget]       = cyan;
    (void)fgDark;

    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding       = 4.0f;
    style.WindowPadding     = ImVec2(10, 10);
    style.FramePadding      = ImVec2(8, 4);
    style.ItemSpacing       = ImVec2(8, 6);
}

// Helper: combo picking another marker slot by name. `selfIndex` is excluded so
// a slot can't reference itself. Selection is stored as the picked slot's vector
// index; -1 means "(none)".
bool SlotCombo(const char *label, int *current, const std::vector<MarkerSlot> &slots, size_t selfIndex)
{
    const char *preview = "(none)";
    if (*current >= 0 && *current < static_cast<int>(slots.size()))
        preview = slots[*current].name;

    bool changed = false;
    if (ImGui::BeginCombo(label, preview))
    {
        if (ImGui::Selectable("(none)", *current < 0))
        {
            if (*current != -1)
            {
                *current = -1;
                changed = true;
            }
        }
        for (size_t i = 0; i < slots.size(); ++i)
        {
            if (i == selfIndex)
                continue;
            ImGui::PushID(static_cast<int>(i));
            bool selected = (*current == static_cast<int>(i));
            char label2[48];
            std::snprintf(label2, sizeof(label2), "%zu: %s", i, slots[i].name);
            if (ImGui::Selectable(label2, selected))
            {
                *current = static_cast<int>(i);
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Helper: combo over available bone names writing into a char[N] field.
// Empty dropdown if the model hasn't been loaded yet.
bool BoneCombo(const char *label, char *buffer, size_t bufferSize, const std::vector<std::string> &bones)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, buffer[0] ? buffer : "(none)"))
    {
        if (ImGui::Selectable("(none)", buffer[0] == 0))
        {
            buffer[0] = 0;
            changed = true;
        }
        for (const auto &bone : bones)
        {
            bool selected = std::strcmp(buffer, bone.c_str()) == 0;
            if (ImGui::Selectable(bone.c_str(), selected))
            {
                std::strncpy(buffer, bone.c_str(), bufferSize - 1);
                buffer[bufferSize - 1] = 0;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}
} // namespace

bool UIManager::Init(GLFWwindow *window)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyTokyoNightTheme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    return true;
}

void UIManager::Destroy()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void UIManager::BuildUI(UIState &state, const std::vector<MarkerObservation> &observations, const glm::mat4 &viewMatrix)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    DrawMenuBar(state);
    DrawSidebar(state, observations);

    ImGuiIO &io = ImGui::GetIO();
    DrawStatusBar(state, io.Framerate, !observations.empty());

    DrawCameraFeedWindow(state, observations);
    DrawAxisGizmo(viewMatrix);

    ImGui::Render();
}

void UIManager::Render()
{
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void UIManager::UploadCameraFeed(const cv::Mat &frame, UIState &state)
{
    if (frame.empty())
        return;

    cv::Mat rgb;
    cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);

    if (state.cameraFeedTexture == 0)
    {
        glGenTextures(1, &state.cameraFeedTexture);
        glBindTexture(GL_TEXTURE_2D, state.cameraFeedTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, state.cameraFeedTexture);
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, rgb.cols, rgb.rows, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data);
    glBindTexture(GL_TEXTURE_2D, 0);

    state.feedWidth = rgb.cols;
    state.feedHeight = rgb.rows;
}

void UIManager::DrawMenuBar(UIState &state)
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Save JSON"))
                state.saveJsonRequested = true;
            if (ImGui::MenuItem("Export Video"))
                state.exportVideoRequested = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void UIManager::DrawSidebar(UIState &state, const std::vector<MarkerObservation> &observations)
{
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float sidebarH = display.y - kMenuBarH - kStatusBarH;
    if (sidebarH < 1.0f)
        sidebarH = 1.0f;

    ImGui::SetNextWindowPos(ImVec2(0, kMenuBarH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSidebarW, sidebarH), ImGuiCond_Always);
    ImGui::Begin("Sidebar", nullptr, kSidebarFlags);

    if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen))
        DrawModelSection(state);

    if (ImGui::CollapsingHeader("Detector", ImGuiTreeNodeFlags_DefaultOpen))
        DrawDetectorSection(state);

    if (ImGui::CollapsingHeader("Markers", ImGuiTreeNodeFlags_DefaultOpen))
        DrawMarkersSection(state, observations);

    if (ImGui::CollapsingHeader("Recording"))
        DrawRecordingSection(state);

    if (ImGui::CollapsingHeader("Export"))
        DrawExportSection(state);

    ImGui::End();
}

void UIManager::DrawModelSection(UIState &state)
{
    ImGui::InputText("Path##model_path", state.modelPath, sizeof(state.modelPath));

    float avail = ImGui::GetContentRegionAvail().x;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float halfW = (avail - spacing) * 0.5f;
    if (ImGui::Button("Load Model", ImVec2(halfW, 26)))
        state.loadModelRequested = true;
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(halfW, 26)))
        state.resetModelRequested = true;

    if (!state.loadedModelPath.empty())
    {
        ImGui::TextDisabled("Loaded: %s", state.loadedModelPath.c_str());
        ImGui::TextDisabled("Bones: %d", static_cast<int>(state.availableBones.size()));
    }
    ImGui::Spacing();
}

void UIManager::DrawDetectorSection(UIState &state)
{
    ImGui::Text("Color Tracking");
    ImGui::Checkbox("Enable Color Detection", &state.enableColorDetection);
    int modeIdx = static_cast<int>(state.detectionMode);
    if (ImGui::Combo("Mode##detection_mode", &modeIdx, "HSV\0RGB Ratio\0"))
    {
        state.detectionMode = static_cast<DetectionMode>(modeIdx);
        state.detectionModeDirty = true;
    }
    if (state.detectionMode == DetectionMode::RGBRatio)
    {
        ImGui::SliderInt("Pool size##rgb_pool", &state.rgbPoolSize, 2, 32);
        ImGui::Checkbox("Show pooled frame##show_pooled", &state.showPooledFrame);
    }
    ImGui::Spacing();

    ImGui::Text("Jitter Filter");
    ImGui::SliderFloat("Smoothing", &state.markerSmoothing, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Deadzone", &state.markerDeadzone, 0.0f, 0.05f, "%.3f");
    ImGui::SliderFloat("Occlusion hold", &state.occlusionHoldSec, 0.0f, 1.0f, "%.2f s");
    ImGui::SliderFloat("Arm forward", &state.armForward, -1.0f, 2.0f, "%.2f");
    ImGui::Spacing();

    ImGui::Text("Depth");
    if (ImGui::InputFloat("Ref distance##depth_dist", &state.depthRefDist, 0.1f, 1.0f, "%.2f"))
        state.depthRefDist = std::max(0.01f, state.depthRefDist);
    if (ImGui::InputFloat("Ref area##depth_area", &state.depthRefArea, 100.0f, 1000.0f, "%.0f px"))
        state.depthRefArea = std::max(1.0f, state.depthRefArea);
    SlotCombo("Calib marker##depth_slot", &state.depthCalibSlot, state.markers, state.markers.size());
    if (ImGui::Button("Calibrate from marker##depth_calib"))
        state.calibrateDepthRequested = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(stand at ref distance)");
    ImGui::Spacing();
}

void UIManager::DrawMarkersSection(UIState &state, const std::vector<MarkerObservation> &observations)
{
    if (ImGui::Button("+ Add marker"))
    {
        MarkerSlot slot;
        std::snprintf(slot.name, sizeof(slot.name), "marker_%d", static_cast<int>(state.markers.size()));
        state.markers.push_back(slot);
        state.markersDirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("- Remove last") && !state.markers.empty())
    {
        state.markers.pop_back();
        state.markersDirty = true;
    }

    // pfd's macOS backend (osascript "default location") requires an existing
    // directory; a relative or non-existent path makes the dialog fail to open
    // a usable picker. Resolve the last-used path to its parent directory,
    // falling back to $HOME / cwd.
    auto startDir = [&state]() -> std::string {
        std::error_code ec;
        std::filesystem::path p = state.markerConfigPath;
        std::filesystem::path dir = p.has_parent_path() ? p.parent_path() : std::filesystem::current_path(ec);
        if (std::filesystem::is_directory(dir, ec))
            return dir.string();
        if (const char *home = std::getenv("HOME"))
            return home;
        return ".";
    };

    if (ImGui::Button("Export config..."))
    {
        // pfd uses the default path as osascript's "default location", which must
        // be a folder — a file path errors out (-1700) and the dialog never opens.
        std::string picked =
            pfd::save_file("Export marker config", startDir(), {"JSON files", "*.json", "All Files", "*"}).result();
        if (!picked.empty())
        {
            std::strncpy(state.markerConfigPath, picked.c_str(), sizeof(state.markerConfigPath) - 1);
            state.markerConfigPath[sizeof(state.markerConfigPath) - 1] = 0;
            state.exportMarkersRequested = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import config..."))
    {
        auto picked =
            pfd::open_file("Import marker config", startDir(), {"JSON files", "*.json", "All Files", "*"}).result();
        if (!picked.empty())
        {
            std::strncpy(state.markerConfigPath, picked[0].c_str(), sizeof(state.markerConfigPath) - 1);
            state.markerConfigPath[sizeof(state.markerConfigPath) - 1] = 0;
            state.importMarkersRequested = true;
        }
    }
    ImGui::Spacing();

    for (size_t i = 0; i < state.markers.size(); ++i)
    {
        auto &slot = state.markers[i];
        ImGui::PushID(static_cast<int>(i));

        bool open = ImGui::TreeNodeEx((void *)i, ImGuiTreeNodeFlags_DefaultOpen, "%zu: %s", i, slot.name);
        if (open)
        {
            if (ImGui::InputText("Name##slot_name", slot.name, sizeof(slot.name)))
                state.markersDirty = true;
            if (ImGui::ColorEdit3("Color##slot_color", slot.overlayColor,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            {
                // The overlay color is independent of the active detection range.
                state.markersDirty = true;
            }

            int observationId = static_cast<int>(i);
            bool matched = false;
            bool held = false;
            for (const auto &o : observations)
                if (o.id == observationId)
                {
                    matched = true;
                    held = o.predicted;
                    break;
                }
            ImGui::SameLine();
            if (held)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "held");
            else
                ImGui::TextColored(matched ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                   matched ? "detected" : "—");

            ImGui::Text("Observation id: %d (row index)", static_cast<int>(i));

            // Color pairing: a follower slot has no range of its own — it is the
            // second-largest blob of the paired slot's range (left/right resolved
            // by the assigner), so its color sliders are hidden.
            if (SlotCombo("Pair with (shares color)##pair_src", &slot.pairSourceSlot, state.markers, i))
                state.markersDirty = true;

            if (slot.pairSourceSlot >= 0)
            {
                ImGui::TextDisabled("(uses color range of slot %d — second blob)", slot.pairSourceSlot);
            }
            else if (state.detectionMode == DetectionMode::HSV)
            {
                bool changed = false;
                changed |= ImGui::SliderInt("H min##h_min", &slot.hsv.hMin, 0, 179);
                changed |= ImGui::SliderInt("H max##h_max", &slot.hsv.hMax, 0, 179);
                changed |= ImGui::SliderInt("S min##s_min", &slot.hsv.sMin, 0, 255);
                changed |= ImGui::SliderInt("S max##s_max", &slot.hsv.sMax, 0, 255);
                changed |= ImGui::SliderInt("V min##v_min", &slot.hsv.vMin, 0, 255);
                changed |= ImGui::SliderInt("V max##v_max", &slot.hsv.vMax, 0, 255);

                // Red wraps the hue circle — enable a second hue band to catch both ends.
                changed |= ImGui::Checkbox("Dual hue (red)##dual_hue", &slot.hsv.dualHue);
                if (slot.hsv.dualHue)
                {
                    changed |= ImGui::SliderInt("H min 2##h_min2", &slot.hsv.hMin2, 0, 179);
                    changed |= ImGui::SliderInt("H max 2##h_max2", &slot.hsv.hMax2, 0, 179);
                }
                if (changed)
                    state.markersDirty = true;
            }
            else
            {
                bool changed = false;
                changed |= ImGui::InputFloat("r min##chroma_r_min", &slot.rgb.rMin, 0.01f, 0.1f, "%.2f");
                changed |= ImGui::InputFloat("r max##chroma_r_max", &slot.rgb.rMax, 0.01f, 0.1f, "%.2f");
                changed |= ImGui::InputFloat("g min##chroma_g_min", &slot.rgb.gMin, 0.01f, 0.1f, "%.2f");
                changed |= ImGui::InputFloat("g max##chroma_g_max", &slot.rgb.gMax, 0.01f, 0.1f, "%.2f");
                changed |= ImGui::InputFloat("Saturation min##chroma_sat", &slot.rgb.satMin, 0.01f, 0.1f, "%.2f");
                changed |= ImGui::InputInt("Brightness min##rgb_vmin", &slot.rgb.vMin, 1, 10);
                if (changed)
                {
                    // Chromaticity windows and saturation floor live in [0,1]; brightness in [0,255].
                    slot.rgb.rMin = std::clamp(slot.rgb.rMin, 0.0f, 1.0f);
                    slot.rgb.rMax = std::clamp(slot.rgb.rMax, 0.0f, 1.0f);
                    slot.rgb.gMin = std::clamp(slot.rgb.gMin, 0.0f, 1.0f);
                    slot.rgb.gMax = std::clamp(slot.rgb.gMax, 0.0f, 1.0f);
                    slot.rgb.satMin = std::clamp(slot.rgb.satMin, 0.0f, 1.0f);
                    slot.rgb.vMin = std::clamp(slot.rgb.vMin, 0, 255);
                    state.markersDirty = true;
                }
            }

            int bindingIdx = static_cast<int>(slot.binding);
            if (ImGui::Combo("Binding##binding", &bindingIdx, kBindingLabels,
                             IM_ARRAYSIZE(kBindingLabels)))
            {
                slot.binding = static_cast<BindingKind>(bindingIdx);
                state.markersDirty = true;
            }

            if (slot.binding == BindingKind::IKTarget)
            {
                if (BoneCombo("IK Root##ik_root", slot.ikRootBone, sizeof(slot.ikRootBone), state.availableBones))
                    state.markersDirty = true;
                if (BoneCombo("IK Mid##ik_mid", slot.ikMidBone, sizeof(slot.ikMidBone), state.availableBones))
                    state.markersDirty = true;
                if (BoneCombo("End##ik_end", slot.boneName, sizeof(slot.boneName), state.availableBones))
                    state.markersDirty = true;
                if (SlotCombo("Upper-arm marker##upperarm_slot", &slot.upperArmMarkerSlot, state.markers, i))
                    state.markersDirty = true;
                if (SlotCombo("Lower-arm marker##lowerarm_slot", &slot.foreArmMarkerSlot, state.markers, i))
                    state.markersDirty = true;
            }
            else if (slot.binding == BindingKind::Hint)
            {
                ImGui::TextDisabled("(detected only — reference by an IK Target slot)");
            }
            else
            {
                if (BoneCombo("Bone##bone", slot.boneName, sizeof(slot.boneName), state.availableBones))
                    state.markersDirty = true;
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
}

void UIManager::DrawRecordingSection(UIState &state)
{
    ImGui::Text("Status: %s", state.isRecording ? "RECORDING" : "Idle");
    ImGui::ProgressBar(state.recordingDuration / 30.0f, ImVec2(-1, 0));
    ImGui::Text("Duration: %.2f s", state.recordingDuration);
    ImGui::Text("Keyframes: %d", state.keyframeCount);

    if (!state.isRecording)
    {
        if (ImGui::Button("START RECORDING", ImVec2(-1, 32)))
            state.startRecordRequested = true;
    }
    else
    {
        if (ImGui::Button("STOP RECORDING", ImVec2(-1, 32)))
            state.stopRecordRequested = true;
    }

    ImGui::Separator();
    if (ImGui::Button("RECONSTRUCT 3D", ImVec2(-1, 28)))
        state.reconstructRequested = true;
    ImGui::Spacing();
}

void UIManager::DrawExportSection(UIState &state)
{
    ImGui::Text("Output Format");
    const char *fmts[] = {"MP4", "AVI"};
    ImGui::Combo("##format", &state.exportFmtIndex, fmts, 2);

    ImGui::SliderInt("FPS##export_fps", &state.exportFps, 1, 60);

    ImGui::InputText("Directory##export_dir", state.exportDir, sizeof(state.exportDir));
    ImGui::SameLine();
    if (ImGui::Button("Browse...##export_dir_browse"))
    {
        std::string start = state.exportDir[0] ? std::string(state.exportDir) : std::string(".");
        std::string picked = pfd::select_folder("Choose export directory", start).result();
        if (!picked.empty())
        {
            std::strncpy(state.exportDir, picked.c_str(), sizeof(state.exportDir) - 1);
            state.exportDir[sizeof(state.exportDir) - 1] = 0;
        }
    }

    ImGui::InputText("Filename (no extension)##export_name", state.exportName, sizeof(state.exportName));

    ImGui::Separator();
    if (ImGui::Button("SAVE JSON", ImVec2(-1, 24)))
        state.saveJsonRequested = true;
    if (ImGui::Button("EXPORT VIDEO", ImVec2(-1, 24)))
        state.exportVideoRequested = true;
    if (ImGui::Button("EXPORT GLB", ImVec2(-1, 24)))
        state.exportGlbRequested = true;
    ImGui::Spacing();
}

void UIManager::DrawStatusBar(UIState &state, float fps, bool detectedThisFrame)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - kStatusBarH));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, kStatusBarH));
    ImGui::Begin("##statusbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::Text("FPS: %.1f | %s | Markers: %s", fps, state.isRecording ? "Recording" : "Idle",
                detectedThisFrame ? "detected" : "none");

    ImGui::End();
}

void UIManager::DrawCameraFeedWindow(UIState &state, const std::vector<MarkerObservation> &observations)
{
    const float W = 340.0f;
    const float H = 260.0f;
    const float margin = 10.0f;

    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x - W - margin, display.y - H - kStatusBarH - margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
    ImGui::Begin("Camera Feed", nullptr, kSidebarFlags);

    if (state.cameraFeedTexture != 0)
    {
        ImVec2 imgSize(W - 20, H - 50);
        ImVec2 imgPos = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)(uintptr_t)state.cameraFeedTexture, imgSize);

        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y), true);

        for (const auto &obs : observations)
        {
            // Match the observation back to a slot for color + label.
            const MarkerSlot *slot = nullptr;
            for (size_t i = 0; i < state.markers.size(); ++i)
            {
                if (static_cast<int>(i) == obs.id)
                {
                    slot = &state.markers[i];
                    break;
                }
            }

            ImU32 col = slot ? IM_COL32(static_cast<int>(slot->overlayColor[0] * 255.0f),
                                        static_cast<int>(slot->overlayColor[1] * 255.0f),
                                        static_cast<int>(slot->overlayColor[2] * 255.0f), 255)
                             : IM_COL32(220, 220, 220, 255);

            ImVec2 center(imgPos.x + obs.centroidNorm.x * imgSize.x, imgPos.y + obs.centroidNorm.y * imgSize.y);
            dl->AddCircle(center, 10.0f, IM_COL32(0, 0, 0, 180), 16, 3.0f);
            dl->AddCircle(center, 9.0f, col, 16, 2.0f);
            dl->AddCircleFilled(center, 2.0f, col);
            if (slot)
                dl->AddText(ImVec2(center.x + 8.0f, center.y - 6.0f), col, slot->name);
        }
        dl->PopClipRect();
    }
    else
    {
        ImGui::TextWrapped("Camera not available");
    }

    ImGui::End();
}

void UIManager::DrawAxisGizmo(const glm::mat4 &view)
{
    const float radius = 40.0f;
    const float margin = 20.0f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 center(display.x - radius - margin, kMenuBarH + radius + margin);

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    glm::mat3 rot(view);

    glm::vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    ImU32 colors[3] = {
        IM_COL32(235, 60, 60, 255),
        IM_COL32(60, 220, 60, 255),
        IM_COL32(80, 140, 255, 255),
    };
    const char *labels[3] = {"X", "Y", "Z"};

    dl->AddCircleFilled(center, radius + 6.0f, IM_COL32(20, 20, 20, 160));

    for (int i = 0; i < 3; ++i)
    {
        glm::vec3 v = rot * axes[i];
        ImVec2 tip(center.x + v.x * radius, center.y - v.y * radius);
        dl->AddLine(center, tip, colors[i], 2.5f);
        dl->AddCircleFilled(tip, 3.0f, colors[i]);
        dl->AddText(ImVec2(tip.x + 4, tip.y - 7), colors[i], labels[i]);
    }
}
