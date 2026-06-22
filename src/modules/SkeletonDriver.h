#pragma once

#include "modules/MarkerDetection.h"
#include "modules/UDPReceiver.h"

#include <functional>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <string>
#include <vector>

namespace Geni
{
class Skeleton;
}

// Mapping from a marker observation to a bone. Supports Position (drag the
// bone to the unprojected marker pos) and LookAt (rotate bone toward marker).
// IK-driven limb posing is handled via separate `IKChain` entries.
struct MarkerBinding
{
    enum class Mode
    {
        Position, // set bone world position; rotation untouched
        LookAt,   // rotate bone toward marker direction; position unchanged
    };

    int markerId = -1;
    std::string boneName;
    Mode mode = Mode::Position;

    // Offset (world space) added after unprojection. Lets the user zero the rig
    // relative to the capture space without recalibrating the camera.
    glm::vec3 worldOffset{0.0f};
};

// Segment-driven two-bone arm: the shoulder joint stays fixed at its bind-pose
// position; the upper-arm bone (root) is aimed from there toward the upper-arm
// marker, and the forearm bone (mid) is aimed toward the palm marker (falling
// back to the lower-arm marker) so bending the elbow is read directly. Bone
// lengths and bind-pose directions are sampled once from the skeleton's
// inverse-bind matrices so repeated solves don't drift.
struct IKChain
{
    int markerId = -1;          // palm marker — forearm aim / end-effector
    int upperArmMarkerId = -1;  // marker on the upper arm; aims the upper-arm bone
    int foreArmMarkerId = -1;   // marker on the forearm; forearm-aim fallback
    std::string rootBoneName;   // e.g. "mixamorig:LeftArm" (shoulder / upper arm)
    std::string midBoneName;    // e.g. "mixamorig:LeftForeArm" (elbow / forearm)
    std::string endBoneName;    // e.g. "mixamorig:LeftHand" (hand)
    glm::vec3 worldOffset{0.0f};

    // Cached bind-pose geometry (filled lazily on first Apply). -1 lengths signal
    // "not yet sampled".
    mutable float upperLen = -1.0f;
    mutable float lowerLen = -1.0f;
    mutable glm::vec3 upperBindDirWorld{0.0f};   // shoulder → elbow (bind, world space)
    mutable glm::vec3 lowerBindDirWorld{0.0f};   // elbow → hand (bind, world space)
    mutable glm::vec3 rootBindWorldPos{0.0f};
    mutable glm::quat rootBindWorldRot{1, 0, 0, 0};
    mutable glm::quat midBindWorldRot{1, 0, 0, 0};
};

class SkeletonDriver
{
  public:
    using Unproject = std::function<glm::vec3(const MarkerObservation &)>;

    void SetBindings(std::vector<MarkerBinding> bindings);
    const std::vector<MarkerBinding> &GetBindings() const;

    void SetIKChains(std::vector<IKChain> chains);
    const std::vector<IKChain> &GetIKChains() const;

    // Apply bindings and IK chains to the skeleton.
    // - observations: live color-marker detections from HSV/RGB detector
    // - unproject:    2D->3D unprojection functor
    // - mpPose:       (optional) latest MediaPipe pose from UDPReceiver;
    //                 when non-empty, it provides the base arm skeleton.
    //                 Color markers override the end-effectors (Sensor Fusion).
    void Apply(Geni::Skeleton &skeleton, const std::vector<MarkerObservation> &observations,
               const Unproject &unproject, const MediaPipePose &mpPose = MediaPipePose{});

    // Constant forward lean for every IK arm chain. Tilts the whole arm (upper +
    // forearm) toward the front of the body by a fixed amount — applied at rest and
    // when bent. 0 = pure frontal-plane solve (markers stay in the shoulder plane).
    float armForward = 0.0f;

  private:
    std::vector<MarkerBinding> m_bindings;
    std::vector<IKChain> m_chains;

    IKChain m_leftLegChain;
    IKChain m_rightLegChain;
};
