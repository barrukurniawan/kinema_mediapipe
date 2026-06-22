#define GLM_ENABLE_EXPERIMENTAL
#include "Application.h"
#include "modules/MarkerConfigIO.h"
#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <GLFW/glfw3.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

namespace
{
std::shared_ptr<Geni::Skeleton> FindSkeleton(Geni::GameObject *root)
{
    if (!root)
        return nullptr;
    if (auto *skin = root->GetComponent<Geni::SkinnedMeshComponent>())
    {
        return skin->GetSkeleton();
    }
    for (const auto &child : root->GetChildren())
    {
        if (auto s = FindSkeleton(child.get()))
            return s;
    }
    return nullptr;
}

void CollectBoneNames(Geni::GameObject *root, std::vector<std::string> &out)
{
    if (!root)
        return;
    out.push_back(root->GetName());
    for (const auto &child : root->GetChildren())
    {
        CollectBoneNames(child.get(), out);
    }
}

// Seed the UI with sensible default bindings so a fresh launch with the Mixamo
// smoke rig + three markers drives head + both hands immediately.
MarkerSlot MakeSlot(const char *name, const float color[3], const char *bone,
                    BindingKind kind = BindingKind::Position, const char *ikRoot = "", const char *ikMid = "")
{
    MarkerSlot slot;
    std::strncpy(slot.name, name, sizeof(slot.name) - 1);
    slot.overlayColor[0] = color[0];
    slot.overlayColor[1] = color[1];
    slot.overlayColor[2] = color[2];
    slot.overlayColor[3] = 1.0f;
    std::strncpy(slot.boneName, bone, sizeof(slot.boneName) - 1);
    slot.binding = kind;
    std::strncpy(slot.ikRootBone, ikRoot, sizeof(slot.ikRootBone) - 1);
    std::strncpy(slot.ikMidBone, ikMid, sizeof(slot.ikMidBone) - 1);
    return slot;
}
} // namespace

bool Application::Init()
{
    // Defaults: 7 markers — head + upper-arm/lower-arm/palm for each arm. The
    // shoulder is never moved: the palm slot is the IK end-effector (forearm aim),
    // the upper-arm slot aims the upper-arm bone, and the lower-arm slot is a
    // forearm-aim fallback when the palm is occluded. Upper-arm + lower-arm slots
    // are Hint-only (detected, referenced by the matching palm chain). Palette
    // gives each arm three hues far apart on the hue circle to reduce same-arm
    // mis-attribution.
    const float orange[3]  = {1.00f, 0.60f, 0.20f};
    const float blue[3]    = {0.35f, 0.50f, 1.00f};
    const float green[3]   = {0.30f, 1.00f, 0.40f};
    const float red[3]     = {1.00f, 0.30f, 0.30f};
    const float cyan[3]    = {0.30f, 0.85f, 1.00f};
    const float magenta[3] = {1.00f, 0.40f, 0.95f};
    const float yellow[3]  = {1.00f, 0.95f, 0.20f};

    auto head       = MakeSlot("head",        orange,  "mixamorig:Head", BindingKind::LookAt);
    auto upperArmL  = MakeSlot("upperarm_L",  blue,    "", BindingKind::Hint);
    auto lowerArmL  = MakeSlot("lowerarm_L",  green,   "", BindingKind::Hint);
    auto palmL      = MakeSlot("palm_L",      red,     "mixamorig:LeftHand",  BindingKind::IKTarget,
                               "mixamorig:LeftArm",  "mixamorig:LeftForeArm");
    auto upperArmR  = MakeSlot("upperarm_R",  cyan,    "", BindingKind::Hint);
    auto lowerArmR  = MakeSlot("lowerarm_R",  magenta, "", BindingKind::Hint);
    auto palmR      = MakeSlot("palm_R",      yellow,  "mixamorig:RightHand", BindingKind::IKTarget,
                               "mixamorig:RightArm", "mixamorig:RightForeArm");

    // Wire IK chains to their hint markers. Slot indices match push order below.
    palmL.upperArmMarkerSlot = 1; // upperarm_L
    palmL.foreArmMarkerSlot  = 2; // lowerarm_L
    palmR.upperArmMarkerSlot = 4; // upperarm_R
    palmR.foreArmMarkerSlot  = 5; // lowerarm_R

    // Narrow default HSV bands per color. Tune from the UI under real lighting.
    // Red wraps the hue circle; we expose the high band (165-179) — users can
    // add a second slot for low-band red if lighting demands it.
    head.hsv      = {  5,  18, 150, 255, 100, 255}; // orange
    upperArmL.hsv = {105, 125, 150, 255,  80, 255}; // blue
    lowerArmL.hsv = { 45,  75, 150, 255,  80, 255}; // green
    palmL.hsv     = {165, 179, 150, 255,  80, 255, true, 0, 10}; // red (dual hue: wraps 0/179)
    upperArmR.hsv = { 85, 100, 150, 255,  80, 255}; // cyan
    lowerArmR.hsv = {140, 159, 150, 255,  80, 255}; // magenta
    palmR.hsv     = { 22,  35, 150, 255, 100, 255}; // yellow

    m_uiState.markers = {head, upperArmL, lowerArmL, palmL, upperArmR, lowerArmR, palmR};
    m_uiState.markersDirty = true;
    m_uiState.loadModelRequested = true;

    // Default export filename: kinema_YYYYMMDD — user can overwrite from the UI.
    {
        std::time_t t = std::time(nullptr);
        std::tm tmLocal{};
#if defined(_WIN32)
        localtime_s(&tmLocal, &t);
#else
        localtime_r(&t, &tmLocal);
#endif
        std::strftime(m_uiState.exportName, sizeof(m_uiState.exportName), "kinema_%Y%m%d", &tmLocal);
    }
    if (const char *home = std::getenv("HOME"))
    {
        std::strncpy(m_uiState.exportDir, home, sizeof(m_uiState.exportDir) - 1);
        m_uiState.exportDir[sizeof(m_uiState.exportDir) - 1] = 0;
    }

    SetupScene();
    RebuildDetectorFromState();

    // Start MediaPipe UDP listener (port 8080). If the Python tracker is not
    // running, the receiver simply receives nothing — Kinema works normally.
    m_udpReceiver = std::make_unique<UDPReceiver>(8080);
    m_udpReceiver->Start();

    m_uiManager.Init(Geni::Engine::GetInstance().GetWindow());
    return true;
}

void Application::SetupScene()
{
    auto &engine = Geni::Engine::GetInstance();
    m_scene = new Geni::Scene();
    engine.SetScene(m_scene);

    m_cameraObj = m_scene->CreateObject("Camera");
    m_cameraObj->AddComponent(new Geni::CameraComponent());
    auto orbit = new Geni::OrbitCameraComponent();
    orbit->SetTarget(glm::vec3(0, 1.0f, 0));
    orbit->SetDistance(4.0f);
    orbit->SetPitch(15.0f);
    m_cameraObj->AddComponent(orbit);
    m_scene->SetMainCamera(m_cameraObj);

    m_lightObj = m_scene->CreateObject("Light");
    m_lightObj->SetPosition(glm::vec3(5, 8, 5));
    auto light = new Geni::LightComponent();
    light->SetColor(glm::vec3(1.0f, 0.95f, 0.9f));
    m_lightObj->AddComponent(light);

    m_unlitMat = Geni::Material::Load("materials/unlit.mat");

    std::vector<glm::vec3> gridPos, gridCol;
    glm::vec3 gridColor(0.4f);
    for (int i = -10; i <= 10; ++i)
    {
        gridPos.push_back({-10.0f, 0.0f, static_cast<float>(i)});
        gridPos.push_back({10.0f, 0.0f, static_cast<float>(i)});
        gridCol.push_back(gridColor);
        gridCol.push_back(gridColor);
        gridPos.push_back({static_cast<float>(i), 0.0f, -10.0f});
        gridPos.push_back({static_cast<float>(i), 0.0f, 10.0f});
        gridCol.push_back(gridColor);
        gridCol.push_back(gridColor);
    }
    m_gridMesh = Geni::Mesh::CreateLines(gridPos, gridCol);
    m_gridObj = m_scene->CreateObject("Grid");
    m_gridObj->AddComponent(new Geni::MeshComponent(m_unlitMat, m_gridMesh));

    std::vector<glm::vec3> axesPos = {
        {0, 0, 0}, {1.5f, 0, 0}, {0, 0, 0}, {0, 1.5f, 0}, {0, 0, 0}, {0, 0, 1.5f},
    };
    std::vector<glm::vec3> axesCol = {
        {1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 1},
    };
    m_axesMesh = Geni::Mesh::CreateLines(axesPos, axesCol);
    m_axesObj = m_scene->CreateObject("Axes");
    m_axesObj->AddComponent(new Geni::MeshComponent(m_unlitMat, m_axesMesh));
}

void Application::LoadRigFromState()
{
    // Destroy any previously loaded rig before loading its replacement.
    if (m_riggedObj)
    {
        m_riggedObj->MarkForDestroy();
        m_riggedObj = nullptr;
        m_riggedSkeleton.reset();
        m_uiState.availableBones.clear();
    }

    m_riggedObj = Geni::GameObject::LoadGLTF(m_uiState.modelPath);
    if (!m_riggedObj)
    {
        m_uiState.loadedModelPath = std::string("FAILED: ") + m_uiState.modelPath;
        return;
    }

    m_riggedObj->SetPosition(glm::vec3(0.0f));
    m_riggedSkeleton = FindSkeleton(m_riggedObj);
    m_uiState.loadedModelPath = m_uiState.modelPath;
    m_uiState.availableBones.clear();
    CollectBoneNames(m_riggedObj, m_uiState.availableBones);
    m_uiState.markersDirty = true; // re-validate bindings against the new skeleton
}

void Application::RebuildDetectorFromState()
{
    if (m_detector)
        m_detector->Destroy();

    if (m_uiState.detectionMode == DetectionMode::RGBRatio)
        m_detector = std::make_unique<RGBRatioMarkerDetector>();
    else
        m_detector = std::make_unique<HSVMarkerDetector>();
    m_detector->Init(0);
}

void Application::RebuildBindingsFromState()
{
    std::vector<MarkerBinding> bindings;
    std::vector<IKChain> chains;

    const int slotCount = static_cast<int>(m_uiState.markers.size());
    auto clampSlot = [slotCount](int idx) { return (idx >= 0 && idx < slotCount) ? idx : -1; };

    for (size_t i = 0; i < m_uiState.markers.size(); ++i)
    {
        const auto &slot = m_uiState.markers[i];
        int observationId = static_cast<int>(i);

        if (slot.binding == BindingKind::Hint)
        {
            // Detected only; no binding emitted. HSV ranges still flow into the
            // detector via slot index so the observation reaches IK chains that
            // reference this slot via upperArm/foreArmMarkerSlot.
            continue;
        }

        if (slot.binding == BindingKind::IKTarget)
        {
            if (slot.ikRootBone[0] == 0 || slot.ikMidBone[0] == 0 || slot.boneName[0] == 0)
                continue;
            IKChain chain;
            chain.markerId = observationId;
            chain.upperArmMarkerId = clampSlot(slot.upperArmMarkerSlot);
            chain.foreArmMarkerId = clampSlot(slot.foreArmMarkerSlot);
            chain.rootBoneName = slot.ikRootBone;
            chain.midBoneName = slot.ikMidBone;
            chain.endBoneName = slot.boneName;
            chains.push_back(chain);
        }
        else
        {
            if (slot.boneName[0] == 0)
                continue;
            MarkerBinding b;
            b.markerId = observationId;
            b.boneName = slot.boneName;
            b.mode = (slot.binding == BindingKind::LookAt) ? MarkerBinding::Mode::LookAt
                                                           : MarkerBinding::Mode::Position;
            bindings.push_back(b);
        }
    }

    m_skelDriver.SetBindings(std::move(bindings));
    m_skelDriver.SetIKChains(std::move(chains));

    // Derive color pairs for the assigner. A follower's pairSourceSlot must point
    // at a valid standalone (non-follower) slot; anything else degrades to -1.
    std::vector<MarkerPair> pairs;
    for (size_t i = 0; i < m_uiState.markers.size(); ++i)
    {
        int src = clampSlot(m_uiState.markers[i].pairSourceSlot);
        if (src < 0 || src == static_cast<int>(i) || m_uiState.markers[src].pairSourceSlot >= 0)
            continue;
        pairs.push_back({src, static_cast<int>(i)});
    }
    m_assigner.SetPairs(std::move(pairs));
}

glm::vec3 Application::Unproject2DtoWorld(const glm::vec2 &centroidNorm, float areaPixels)
{
    float worldX = (centroidNorm.x - 0.5f) * 2.0f;
    float worldY = -(centroidNorm.y - 0.5f) * 2.0f;
    float worldZ = m_uiState.depthRefDist * glm::sqrt(m_uiState.depthRefArea / (areaPixels + 1.0f));
    worldX *= (worldZ / m_uiState.depthRefDist);
    worldY *= (worldZ / m_uiState.depthRefDist);
    return glm::vec3(worldX, worldY, worldZ);
}

void Application::Update(float deltaTime)
{
    m_fps = 1.0f / (deltaTime > 0 ? deltaTime : 1e-4f);

    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)
    {
        auto &im = Geni::Engine::GetInstance().GetInputManager();
        im.SetMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT, false);
        im.SetMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT, false);
        im.SetMousePositionOld(im.GetMousePositionCurrent());
        im.SetScrollDelta(0.0f);
    }

    m_scene->Update(deltaTime);

    // React to UI-driven state changes.
    if (m_uiState.loadModelRequested)
    {
        LoadRigFromState();
        m_uiState.loadModelRequested = false;
    }
    if (m_uiState.resetModelRequested)
    {
        m_playback = false;
        m_playbackTime = 0.0f;
        if (m_riggedObj && !m_uiState.loadedModelPath.empty())
        {
            char savedPath[512];
            strncpy(savedPath, m_uiState.modelPath, sizeof(savedPath));
            strncpy(m_uiState.modelPath, m_uiState.loadedModelPath.c_str(), sizeof(m_uiState.modelPath));
            LoadRigFromState();
            strncpy(m_uiState.modelPath, savedPath, sizeof(m_uiState.modelPath));
        }
        m_uiState.resetModelRequested = false;
    }
    if (m_uiState.detectionModeDirty)
    {
        RebuildDetectorFromState(); // swap HSV <-> RGB-ratio backend
        m_assigner.Reset();
        m_stabilizer.Reset();
        m_uiState.detectionModeDirty = false;
    }
    if (m_uiState.markersDirty)
    {
        RebuildBindingsFromState(); // also re-derives color pairs (resets the assigner)
        m_stabilizer.Reset();       // slot ids may have shuffled; drop stale per-id state
        m_uiState.markersDirty = false;
    }

    // Each frame, sync per-slot color config into the detector — cheap and lets the
    // slider feedback be immediate without needing an explicit "apply" press.
    if (auto *hsv = dynamic_cast<HSVMarkerDetector *>(m_detector.get()))
    {
        std::vector<HSVRange> ranges;
        ranges.reserve(m_uiState.markers.size());
        for (const auto &slot : m_uiState.markers)
            ranges.push_back(slot.hsv);
        hsv->SetRanges(ranges);
    }
    else if (auto *rgb = dynamic_cast<RGBRatioMarkerDetector *>(m_detector.get()))
    {
        std::vector<RGBRatioRange> ranges;
        ranges.reserve(m_uiState.markers.size());
        for (const auto &slot : m_uiState.markers)
            ranges.push_back(slot.rgb);
        rgb->SetRanges(ranges);
        rgb->SetPoolSize(m_uiState.rgbPoolSize);
    }
    if (m_detector)
    {
        // Paired follower slots have no range of their own: skip them (0) and pull
        // two blobs from their primary's range instead.
        std::vector<int> counts(m_uiState.markers.size(), 1);
        for (size_t i = 0; i < m_uiState.markers.size(); ++i)
        {
            int src = m_uiState.markers[i].pairSourceSlot;
            if (src >= 0 && src < static_cast<int>(counts.size()))
            {
                counts[i] = 0;
                counts[src] = 2;
            }
        }
        m_detector->SetBlobCounts(counts);
    }

    std::vector<MarkerObservation> observations;
    if (m_detector && m_uiState.enableColorDetection)
    {
        observations = m_detector->Detect();

        // macOS camera-permission workaround: if the device is open but not
        // delivering frames, periodically rebuild the detector so a post-launch
        // permission grant is picked up without restarting the app.
        bool hasValidFrame = m_detector->IsOpen() && !m_detector->GetLastFrame().empty();
        if (!hasValidFrame)
        {
            m_cameraRetryTimer += deltaTime;
            if (m_cameraRetryTimer >= 1.0f)
            {
                RebuildDetectorFromState();
                m_cameraRetryTimer = 0.0f;
            }
        }
        else
        {
            m_cameraRetryTimer = 0.0f;
        }
    }

    // Resolve paired same-color blobs to unique slot ids before any per-id consumer.
    m_assigner.Assign(observations);

    // Deadzone + EMA before anything consumes the observations, so marker jitter
    // doesn't sway the rig. Knobs are live-tunable from the UI.
    m_stabilizer.alpha = m_uiState.markerSmoothing;
    m_stabilizer.deadzone = m_uiState.markerDeadzone;
    m_stabilizer.holdSec = m_uiState.occlusionHoldSec;
    m_stabilizer.Filter(observations, deltaTime);
    m_skelDriver.armForward = m_uiState.armForward;

    if (m_uiState.calibrateDepthRequested)
    {
        // Capture the chosen slot's current blob area as the depth reference: the
        // user stands at depthRefDist and presses the button.
        for (const auto &o : observations)
        {
            if (o.id == m_uiState.depthCalibSlot && !o.predicted)
            {
                m_uiState.depthRefArea = o.areaPixels;
                break;
            }
        }
        m_uiState.calibrateDepthRequested = false;
    }

    if (m_playback && m_riggedSkeleton)
    {
        // Playback overrides live detection so the recorded motion renders untouched.
        const auto &keyframes = m_recorder.GetKeyframes();
        if (keyframes.empty())
        {
            m_playback = false;
        }
        else
        {
            m_playbackTime += deltaTime;
            size_t idx = 0;
            while (idx + 1 < keyframes.size() && keyframes[idx + 1].timestamp <= m_playbackTime)
                ++idx;
            if (idx + 1 < keyframes.size())
            {
                // Blend toward the next keyframe so playback doesn't stair-step
                // when the capture rate was below the render rate.
                double span = keyframes[idx + 1].timestamp - keyframes[idx].timestamp;
                float t = span > 0.0 ? static_cast<float>((m_playbackTime - keyframes[idx].timestamp) / span)
                                     : 0.0f;
                MotionRecorder::ApplyInterpolated(keyframes[idx], keyframes[idx + 1], t, *m_riggedSkeleton);
            }
            else
            {
                MotionRecorder::ApplyKeyframe(keyframes[idx], *m_riggedSkeleton);
            }
            if (m_playbackTime >= keyframes.back().timestamp)
                m_playback = false;
        }
    }
    else if (m_riggedSkeleton)
    {
        // Pull the latest MediaPipe pose from the UDP receiver (may be empty
        // if the Python tracker is not running — graceful no-op).
        MediaPipePose mpPose;
        if (m_udpReceiver)
            mpPose = m_udpReceiver->GetLatestPose();

        m_skelDriver.Apply(*m_riggedSkeleton, observations, [this](const MarkerObservation &o) {
            return Unproject2DtoWorld(o.centroidNorm, o.areaPixels);
        }, mpPose);

        if (m_recorder.IsRecording())
            m_recorder.AddSkeletonKeyframe(*m_riggedSkeleton);
    }

    m_uiState.isRecording = m_recorder.IsRecording();
    m_uiState.keyframeCount = m_recorder.GetKeyframeCount();
    m_uiState.recordingDuration = m_recorder.GetDuration();

    if (m_detector && m_detector->IsOpen())
    {
        const cv::Mat *feed = &m_detector->GetLastFrame();
        if (m_uiState.showPooledFrame)
        {
            if (auto *rgb = dynamic_cast<RGBRatioMarkerDetector *>(m_detector.get()))
                feed = &rgb->GetPooledFrame();
        }
        m_uiManager.UploadCameraFeed(*feed, m_uiState);
    }

    auto *cam = m_cameraObj->GetComponent<Geni::CameraComponent>();
    glm::mat4 view = cam->GetViewMatrix();
    m_uiManager.BuildUI(m_uiState, observations, view);

    if (m_uiState.startRecordRequested)
    {
        m_recorder.StartRecording();
        m_uiState.startRecordRequested = false;
    }
    if (m_uiState.stopRecordRequested)
    {
        m_recorder.StopRecording();
        m_uiState.stopRecordRequested = false;
    }
    if (m_uiState.reconstructRequested)
    {
        m_playbackTime = 0.0f;
        m_playback = !m_recorder.GetKeyframes().empty();
        m_uiState.reconstructRequested = false;
    }
    if (m_uiState.exportMarkersRequested)
    {
        SaveMarkerConfig(m_uiState.markerConfigPath, m_uiState.markers, m_uiState.depthRefDist,
                         m_uiState.depthRefArea);
        m_uiState.exportMarkersRequested = false;
    }
    if (m_uiState.importMarkersRequested)
    {
        if (LoadMarkerConfig(m_uiState.markerConfigPath, m_uiState.markers, m_uiState.depthRefDist,
                             m_uiState.depthRefArea))
            m_uiState.markersDirty = true; // re-validate bindings + resync detector ranges
        m_uiState.importMarkersRequested = false;
    }

    auto joinPath = [this](const char *ext) {
        std::string p = m_uiState.exportDir;
        if (!p.empty() && p.back() != '/' && p.back() != '\\')
            p += '/';
        return p + m_uiState.exportName + ext;
    };

    if (m_uiState.saveJsonRequested)
    {
        m_recorder.SaveToJson(joinPath(".json"));
        m_uiState.saveJsonRequested = false;
    }
    if (m_uiState.exportVideoRequested)
    {
        const char *ext = (m_uiState.exportFmtIndex == 0) ? ".mp4" : ".avi";
        m_exporter.Export(m_recorder.GetKeyframes(), m_riggedSkeleton.get(), m_uiState.exportFps, joinPath(ext),
                          m_uiState.exportFmtIndex == 0);
        m_uiState.exportVideoRequested = false;
    }
    if (m_uiState.exportGlbRequested)
    {
        if (m_riggedSkeleton && !m_uiState.loadedModelPath.empty())
        {
            // loadedModelPath is the same relative string LoadGLTF was handed; cgltf_parse_file
            // uses fopen() so we must resolve it against the assets folder ourselves. An
            // absolute right-hand operand wins, so user-entered absolute paths still work.
            std::filesystem::path src = m_uiState.loadedModelPath;
            if (src.is_relative())
                src = Geni::Engine::GetInstance().GetFileSystem().GetAssetsFolder() / src;
            m_glbExporter.Export(src.string(), m_recorder.GetKeyframes(), *m_riggedSkeleton,
                                 joinPath(".glb"));
        }
        m_uiState.exportGlbRequested = false;
    }
}

void Application::Render()
{
    m_uiManager.Render();
}

void Application::Destroy()
{
    m_uiManager.Destroy();
    if (m_udpReceiver)
        m_udpReceiver->Stop();
    if (m_detector)
    {
        m_detector->Destroy();
    }
}
