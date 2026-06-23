#pragma once
#include "modules/MarkerDetection.h"
#include "modules/MarkerDetector.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/mat4x4.hpp>
#include <imgui.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

// How a marker slot drives its bound bone.
enum class BindingKind
{
    Position,         // bone.worldPos = unproject(marker)
    IKTarget,         // marker is end-effector; ikRoot + ikMid posed via analytical 2-bone IK
    LookAt,           // bone rotates toward marker; position is never changed (no neck stretch)
    Hint,             // detected only; not bound to a bone (used as elbow/shoulder reference for an IKTarget slot)
};

// One user-configured marker. Matches by slot index.
struct MarkerSlot
{
    char name[32] = "marker";
    float overlayColor[4] = {0.2f, 1.0f, 0.4f, 1.0f};

    HSVRange hsv;
    RGBRatioRange rgb; // used when UIState.detectionMode == RGBRatio

    BindingKind binding = BindingKind::Position;
    char boneName[64] = "";   // target bone (end-effector when IKTarget)
    char ikRootBone[64] = ""; // IKTarget only
    char ikMidBone[64] = "";  // IKTarget only

    // IKTarget-only: vector indices into UIState.markers picking the upper-arm
    // (aims the upper-arm bone) and lower-arm (forearm-aim fallback) hint slots.
    // -1 = unset. Stale indices (after delete) are clamped to -1 in
    // Application::RebuildBindingsFromState.
    int upperArmMarkerSlot = -1;
    int foreArmMarkerSlot = -1;

    // >=0: this slot is the second marker of a color pair — it has no range of
    // its own; it is resolved as the second-largest blob of the referenced
    // slot's range (left/right disambiguated by MarkerAssigner). -1 = standalone.
    int pairSourceSlot = -1;
};

// Which detector backend drives all marker slots.
enum class DetectionMode
{
    HSV,      // HSVMarkerDetector
    RGBRatio, // RGBRatioMarkerDetector
};

struct UIState
{
    // Marker slots
    std::vector<MarkerSlot> markers;
    bool markersDirty = false; // set when any slot config changes (bindings, HSV ranges, etc.)

    // Detector backend selection
    DetectionMode detectionMode = DetectionMode::HSV;
    bool enableColorDetection = true;
    bool detectionModeDirty = false; // set when mode changes; triggers detector rebuild
    int rgbPoolSize = 8;             // RGB-ratio average-pool block size
    bool showPooledFrame = false;    // show pooled frame instead of raw in camera feed

    // Marker jitter filter (deadzone + EMA on 2D centroids); live-tunable
    float markerSmoothing = 0.25f;   // EMA factor 0..1 (lower = smoother)
    float markerDeadzone = 0.015f;   // normalized centroid radius held as no-move
    float occlusionHoldSec = 0.3f;   // extrapolate lost markers this long; 0 = off
    float armForward = 0.0f;         // constant forward lean of IK arms (world units)

    // 2D→3D depth reference: a blob of depthRefArea pixels sits depthRefDist away.
    // "Calibrate" captures the chosen slot's current blob area as the reference.
    float depthRefDist = 2.0f;
    float depthRefArea = 10000.0f;
    int depthCalibSlot = 0;
    bool calibrateDepthRequested = false;

    // Marker config persistence (HSV bands + bindings reuse across sessions)
    char markerConfigPath[512] = "markers.json";
    bool exportMarkersRequested = false;
    bool importMarkersRequested = false;

    // Model import
    char modelPath[512] = "models/mixamo_lowpoly/defeated.glb";
    std::string loadedModelPath;
    std::vector<std::string> availableBones;
    bool loadModelRequested = false;
    bool resetModelRequested = false;

    // Recording
    bool isRecording = false;
    float recordingDuration = 0.0f;
    int keyframeCount = 0;
    bool startRecordRequested = false;
    bool stopRecordRequested = false;
    bool reconstructRequested = false;

    // Export
    int exportFmtIndex = 0;
    int exportFps = 30;
    char exportDir[512] = "";   // filled by Application::Init ($HOME or cwd)
    char exportName[128] = "";  // filled by Application::Init (kinema_YYYYMMDD)
    bool saveJsonRequested = false;
    bool exportVideoRequested = false;
    bool exportGlbRequested = false;

    // Camera feed texture
    GLuint cameraFeedTexture = 0;
    int feedWidth = 0;
    int feedHeight = 0;
    bool showCameraFeed = false;
};

class UIManager
{
  public:
    bool Init(GLFWwindow *window);
    void Destroy();

    void BuildUI(UIState &state, const std::vector<MarkerObservation> &observations, const glm::mat4 &viewMatrix);
    void Render();
    void UploadCameraFeed(const cv::Mat &frame, UIState &state);

  private:
    void DrawMenuBar(UIState &state);
    void DrawSidebar(UIState &state, const std::vector<MarkerObservation> &observations);
    void DrawDetectorSection(UIState &state);
    void DrawMarkersSection(UIState &state, const std::vector<MarkerObservation> &observations);
    void DrawModelSection(UIState &state);
    void DrawRecordingSection(UIState &state);
    void DrawExportSection(UIState &state);
    void DrawStatusBar(UIState &state, float fps, bool detectedThisFrame);
    void DrawCameraFeedWindow(UIState &state, const std::vector<MarkerObservation> &observations);
    void DrawAxisGizmo(const glm::mat4 &viewMatrix);
};

struct GLFWwindow;
