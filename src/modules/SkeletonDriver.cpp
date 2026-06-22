#define GLM_ENABLE_EXPERIMENTAL
#include "modules/SkeletonDriver.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <scene/GameObject.h>
#include <scene/Skeleton.h>

#include <unordered_map>

namespace
{
// Given a target world position for `bone`, compute the local position that makes
// its world end up at `targetWorld` (leaving parent's pose unchanged).
glm::vec3 WorldToLocalPos(Geni::GameObject *bone, const glm::vec3 &targetWorld)
{
    auto *parent = bone->GetParent();
    if (!parent)
    {
        return targetWorld;
    }
    glm::mat4 parentInv = glm::inverse(parent->GetWorldTransform());
    return glm::vec3(parentInv * glm::vec4(targetWorld, 1.0f));
}

// Local rotation needed so that parent.world * local == desiredWorldRot.
glm::quat WorldToLocalRot(Geni::GameObject *bone, const glm::quat &desiredWorldRot)
{
    auto *parent = bone->GetParent();
    if (!parent)
    {
        return desiredWorldRot;
    }
    glm::quat parentWorldRot = glm::quat_cast(parent->GetWorldTransform());
    return glm::inverse(parentWorldRot) * desiredWorldRot;
}

// Index observations by id so repeat lookups are O(1).
std::unordered_map<int, const MarkerObservation *> IndexById(const std::vector<MarkerObservation> &obs)
{
    std::unordered_map<int, const MarkerObservation *> map;
    map.reserve(obs.size());
    for (const auto &o : obs)
    {
        map[o.id] = &o;
    }
    return map;
}

// Build a quaternion that rotates `from` onto `to`. Handles the 180° edge case by
// picking an arbitrary axis orthogonal to `from`.
glm::quat RotationBetween(const glm::vec3 &from, const glm::vec3 &to)
{
    glm::vec3 f = glm::normalize(from);
    glm::vec3 t = glm::normalize(to);
    float cosTheta = glm::dot(f, t);
    if (cosTheta > 0.99999f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    if (cosTheta < -0.99999f)
    {
        glm::vec3 axis = glm::cross(glm::vec3(1, 0, 0), f);
        if (glm::length(axis) < 1e-4f)
        {
            axis = glm::cross(glm::vec3(0, 1, 0), f);
        }
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    glm::vec3 axis = glm::normalize(glm::cross(f, t));
    float angle = std::acos(glm::clamp(cosTheta, -1.0f, 1.0f));
    return glm::angleAxis(angle, axis);
}

// Lazily sample bind-pose geometry for a chain from the skeleton's inverse-bind
// matrices (bind_world = inverse(inverse_bind)). Captures lengths, world-space
// bone directions, and the bind world rotation for each joint — all constant
// across frames, so compute once and reuse.
void PrimeIKChain(IKChain &chain, const Geni::Skeleton &skeleton, int rootIdx, int midIdx, int endIdx)
{
    if (chain.upperLen > 0.0f)
        return;

    glm::mat4 rootBind = glm::inverse(skeleton.GetInverseBindMatrix(rootIdx));
    glm::mat4 midBind = glm::inverse(skeleton.GetInverseBindMatrix(midIdx));
    glm::mat4 endBind = glm::inverse(skeleton.GetInverseBindMatrix(endIdx));

    glm::vec3 rootPos(rootBind[3]);
    glm::vec3 midPos(midBind[3]);
    glm::vec3 endPos(endBind[3]);

    chain.upperLen = glm::length(midPos - rootPos);
    chain.lowerLen = glm::length(endPos - midPos);
    chain.upperBindDirWorld =
        chain.upperLen > 1e-5f ? (midPos - rootPos) / chain.upperLen : glm::vec3(0, 1, 0);
    chain.lowerBindDirWorld =
        chain.lowerLen > 1e-5f ? (endPos - midPos) / chain.lowerLen : glm::vec3(0, 1, 0);
    chain.rootBindWorldPos = rootPos;
    chain.rootBindWorldRot = glm::quat_cast(rootBind);
    chain.midBindWorldRot = glm::quat_cast(midBind);
}

// Segment-driven arm solver. The shoulder joint (`rootPos`) is held fixed; the
// two bones are aimed using *inter-marker* directions so they live in the same
// frame as each other (the rig shoulder's absolute world height is unrelated to
// the camera/marker space, so aiming from rig-shoulder to a marker would let a
// rest-down arm read as "lifted"):
//   upper arm  = lowerArm marker - upperArm marker   (U → L)
//   forearm    = palm marker      - lowerArm marker   (L → P)
// At rest (all three markers vertically aligned) both directions point straight
// down, so the avatar arm hangs down. Bone lengths from the bind pose are
// preserved (no stretch). `haveU/haveL/haveP` let a marker drop out: a bone is
// only re-aimed when both of its endpoints are visible, otherwise it holds its
// current pose.
void SolveArmChain(Geni::GameObject *root, Geni::GameObject *mid, const IKChain &chain,
                   const glm::vec3 &rootPos, const glm::vec3 &upperMarker, bool haveU,
                   const glm::vec3 &lowerMarker, bool haveL, const glm::vec3 &palmMarker, bool haveP)
{
    if (!root || !mid)
        return;
    if (chain.upperLen < 1e-5f || chain.lowerLen < 1e-5f)
        return;

    // Upper-arm bone: aim along the upper-arm → lower-arm marker vector.
    glm::quat shoulderDelta(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 upperDirDesired = chain.upperBindDirWorld;
    if (haveU && haveL)
    {
        glm::vec3 dir = lowerMarker - upperMarker;
        if (glm::length(dir) > 1e-5f)
        {
            upperDirDesired = glm::normalize(dir);
            shoulderDelta = RotationBetween(chain.upperBindDirWorld, upperDirDesired);
            glm::quat shoulderWorldRot = shoulderDelta * chain.rootBindWorldRot;
            root->SetRotation(WorldToLocalRot(root, shoulderWorldRot));
        }
    }

    // Forearm bone: aim along the lower-arm → palm marker vector, pivoting at the
    // resolved elbow (bind-pose upper-arm length along the upper direction, so a
    // longer/shorter physical arm doesn't stretch the rig mesh).
    if (!(haveL && haveP))
        return;
    glm::vec3 lowerDir = palmMarker - lowerMarker;
    if (glm::length(lowerDir) < 1e-5f)
        return;
    glm::vec3 lowerDirDesired = glm::normalize(lowerDir);

    glm::vec3 lowerDirAfterShoulder = shoulderDelta * chain.lowerBindDirWorld;
    glm::quat elbowDelta = RotationBetween(lowerDirAfterShoulder, lowerDirDesired);
    glm::quat elbowWorldRot = elbowDelta * shoulderDelta * chain.midBindWorldRot;
    mid->SetRotation(WorldToLocalRot(mid, elbowWorldRot));
}
} // namespace

void SkeletonDriver::SetBindings(std::vector<MarkerBinding> bindings)
{
    m_bindings = std::move(bindings);
}

const std::vector<MarkerBinding> &SkeletonDriver::GetBindings() const
{
    return m_bindings;
}

void SkeletonDriver::SetIKChains(std::vector<IKChain> chains)
{
    m_chains = std::move(chains);
}

const std::vector<IKChain> &SkeletonDriver::GetIKChains() const
{
    return m_chains;
}

void SkeletonDriver::Apply(Geni::Skeleton &skeleton, const std::vector<MarkerObservation> &observations,
                           const Unproject &unproject, const MediaPipePose &mpPose)
{
    auto byId = IndexById(observations);

    for (const auto &binding : m_bindings)
    {
        auto it = byId.find(binding.markerId);
        if (it == byId.end())
            continue;

        int jointIndex = skeleton.FindJoint(binding.boneName);
        if (jointIndex < 0)
            continue;
        Geni::GameObject *bone = skeleton.GetJointNode(jointIndex);
        if (!bone)
            continue;

        glm::vec3 targetWorld = unproject(*it->second) + binding.worldOffset;

        if (binding.mode == MarkerBinding::Mode::LookAt)
        {
            // Yaw-only rotation: rotate around world Y based on the marker's horizontal
            // position. This keeps the bone upright and only turns it left/right.
            // The angle is measured from the +Z axis toward +X in the XZ plane.
            glm::mat4 bindWorld = glm::inverse(skeleton.GetInverseBindMatrix(jointIndex));
            glm::quat boneBindRot = glm::quat_cast(bindWorld);

            float yaw = std::atan2(targetWorld.x, targetWorld.z);
            glm::quat yawDelta = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            bone->SetRotation(WorldToLocalRot(bone, yawDelta * boneBindRot));
        }
        else
        {
            bone->SetPosition(WorldToLocalPos(bone, targetWorld));
        }
    }

    for (auto &chain : m_chains)
    {
        // Cheap early-out: skip the chain entirely if none of its markers are
        // visible AND MediaPipe has no data for this chain's arm side.
        bool anyColorVisible = byId.count(chain.markerId) ||
                               (chain.upperArmMarkerId >= 0 && byId.count(chain.upperArmMarkerId)) ||
                               (chain.foreArmMarkerId >= 0 && byId.count(chain.foreArmMarkerId));
        bool hasMpData = !mpPose.empty();
        if (!anyColorVisible && !hasMpData)
            continue;

        int rootIdx = skeleton.FindJoint(chain.rootBoneName);
        int midIdx = skeleton.FindJoint(chain.midBoneName);
        int endIdx = skeleton.FindJoint(chain.endBoneName);
        if (rootIdx < 0 || midIdx < 0 || endIdx < 0)
            continue;

        PrimeIKChain(chain, skeleton, rootIdx, midIdx, endIdx);
        Geni::GameObject *rootBone = skeleton.GetJointNode(rootIdx);
        Geni::GameObject *midBone = skeleton.GetJointNode(midIdx);

        glm::vec3 rootPos = rootBone ? glm::vec3(rootBone->GetWorldTransform()[3]) : chain.rootBindWorldPos;

        // ── Sensor Fusion: determine per-bone source ──────────────────────────
        // Identify which arm (L/R) this chain is for by checking the root bone name.
        bool isLeft = (chain.rootBoneName.find("Left") != std::string::npos ||
                       chain.rootBoneName.find("left") != std::string::npos ||
                       chain.rootBoneName.find("_L") != std::string::npos);

        int mpShoulderIdx = isLeft ? MP_LEFT_SHOULDER  : MP_RIGHT_SHOULDER;
        int mpElbowIdx    = isLeft ? MP_LEFT_ELBOW     : MP_RIGHT_ELBOW;
        int mpWristIdx    = isLeft ? MP_LEFT_WRIST     : MP_RIGHT_WRIST;

        // Build per-marker source slots — prefer color markers when visible,
        // fall back to MediaPipe landmarks (mapped to the camera-view frame).
        auto fetch = [&](int colorId, glm::vec3 &out) -> bool {
            if (colorId < 0) return false;
            auto mIt = byId.find(colorId);
            if (mIt == byId.end()) return false;
            out = unproject(*mIt->second);
            out.z = rootPos.z;
            return true;
        };

        // Helper: map MediaPipe normalized (x,y,z) to the same world space that
        // the color markers live in.
        //
        // Formula mirrors Unproject2DtoWorld (color markers):
        //   worldX = (x - 0.5) * 2  ← NO negation on X (same as color markers)
        //   worldY = -(y - 0.5) * 2 ← negated because screen Y goes down, world Y up
        //
        // The old code negated X based on a wrong assumption. Negating X flips
        // the direction vector sign when the arm moves sideways, causing
        // RotationBetween() to rotate the bone INWARD (through the body/back)
        // instead of OUTWARD/UPWARD naturally. Color markers never needed X
        // negation, so MediaPipe should not either.
        //
        // DEPTH: Use bind-pose shoulder depth as stable reference (fallback 1.5m)
        // so we don't collapse to zero when depth calibration hasn't been done.
        auto mpToWorld = [&](int mpId, glm::vec3 &out) -> bool {
            if (!mpPose.hasJoint(mpId, 0.4f)) return false;
            glm::vec3 lm = mpPose.getJoint(mpId);
            float refZ = (chain.rootBindWorldPos.z > 0.1f) ? chain.rootBindWorldPos.z : 1.5f;
            float worldX =  (lm.x - 0.5f) * 2.0f * refZ;   // matches color marker formula
            float worldY = -(lm.y - 0.5f) * 2.0f * refZ;   // screen Y is inverted vs world Y
            // lm.z is metric depth from pose_world_landmarks (metres).
            // MediaPipe convention: Z is POSITIVE toward the camera.
            // So when the user punches forward (toward camera), lm.z increases.
            // We SUBTRACT it so worldZ decreases (= closer to camera in our world).
            float worldZ = refZ - lm.z;   // lm.z>0 when forward -> worldZ decreases toward camera
            out = glm::vec3(worldX, worldY, worldZ);
            return true;
        };

        glm::vec3 upperMarker(0.0f), lowerMarker(0.0f), palmMarker(0.0f);

        // Upper arm: always prefer color marker; fall back to MP shoulder→elbow midpoint.
        bool haveU = fetch(chain.upperArmMarkerId, upperMarker);
        if (!haveU) haveU = mpToWorld(mpShoulderIdx, upperMarker);

        // Forearm: prefer color marker; fall back to MP elbow.
        bool haveL = fetch(chain.foreArmMarkerId, lowerMarker);
        if (!haveL) haveL = mpToWorld(mpElbowIdx, lowerMarker);

        // Palm (end-effector): Sensor Fusion core logic.
        // Priority: color marker >> MediaPipe wrist
        bool haveP = fetch(chain.markerId, palmMarker);
        if (haveP)
        {
            palmMarker += chain.worldOffset;
        }
        else if (mpToWorld(mpWristIdx, palmMarker))
        {
            // MediaPipe fallback: use AI wrist position
            haveP = true;
        }

        if (armForward != 0.0f)
        {
            lowerMarker.z += armForward * 0.5f;
            palmMarker.z  += armForward;
        }

        SolveArmChain(rootBone, midBone, chain, rootPos, upperMarker, haveU, lowerMarker, haveL,
                      palmMarker, haveP);
    }

    // ── MediaPipe head tracking (yaw + pitch + roll) ──────────────────────────
    // Drives LookAt-mode bindings (head bone) using nose + ear landmarks.
    // The existing color-marker LookAt (yaw-only) runs first in the binding loop
    // above and skips if no marker is found. This block runs after and fills in
    // full 3-axis head rotation when MediaPipe data is available and the color
    // marker was NOT detected (so wearing an orange sticker still overrides).
    if (!mpPose.empty() && mpPose.hasJoint(MP_NOSE, 0.3f))
    {
        glm::vec3 nose  = mpPose.getJoint(MP_NOSE);
        bool haveLEar   = mpPose.hasJoint(MP_LEFT_EAR,  0.3f);
        bool haveREar   = mpPose.hasJoint(MP_RIGHT_EAR, 0.3f);
        glm::vec3 le    = haveLEar  ? mpPose.getJoint(MP_LEFT_EAR)  : nose;
        glm::vec3 re    = haveREar  ? mpPose.getJoint(MP_RIGHT_EAR) : nose;
        bool haveEars   = haveLEar && haveREar;

        // Ear midpoint: horizontal reference for yaw, vertical for roll
        glm::vec3 earMid = haveEars ? (le + re) * 0.5f : nose;

        // Ear-to-ear distance normalizes angles so distance from camera doesn't matter
        float earDist = haveEars ? glm::length(glm::vec2(re.x - le.x, re.y - le.y)) : 0.12f;
        earDist = std::max(earDist, 0.01f);

        // Shoulder midpoint: vertical pitch reference (keeps pitch independent of camera height)
        bool haveLS = mpPose.hasJoint(MP_LEFT_SHOULDER,  0.3f);
        bool haveRS = mpPose.hasJoint(MP_RIGHT_SHOULDER, 0.3f);
        float shoulderMidY = (haveLS && haveRS)
            ? (mpPose.getJoint(MP_LEFT_SHOULDER).y + mpPose.getJoint(MP_RIGHT_SHOULDER).y) * 0.5f
            : (nose.y + 0.25f);   // fallback: assume shoulders are 25% below nose

        // ── Yaw (turning left / right) ────────────────────────────────────────
        // nose.x relative to ear midpoint, NEGATED to match the camera mirror convention:
        // turning RIGHT → nose moves RIGHT on screen (high x) → we want positive yaw
        // but (nose.x - earMid.x) > 0, so we negate to get the correct direction.
        float yawRaw   = -(nose.x - earMid.x) / earDist;
        float yawAngle = glm::clamp(yawRaw * 1.6f, -1.2f, 1.2f);   // radians, ±~69°

        // ── Pitch (nodding up / down) ──────────────────────────────────────────
        // Use nose.y relative to earMid.y (NOT shoulder, which gave a huge static
        // offset that clamped all movement). The nose sits anatomically ~0.28×earDist
        // below the ear midpoint when looking straight ahead — subtract that rest
        // offset so pitchRaw ≈ 0 at neutral gaze.
        //
        //  Looking UP  → nose.y decreases → pitchRaw < 0 → negative pitchAngle
        //                glm::angleAxis(-angle, X) rotates chin back → head looks UP ✓
        //  Looking DOWN → nose.y increases → pitchRaw > 0 → positive pitchAngle  ✓
        constexpr float PITCH_REST_OFFSET = 0.28f;  // anatomical nose-below-ear ratio
        float pitchRaw   = (nose.y - earMid.y) / earDist - PITCH_REST_OFFSET;
        float pitchAngle = glm::clamp(pitchRaw * 2.0f, -0.7f, 0.6f);

        // ── Roll (head tilt) ───────────────────────────────────────────────────
        // left ear LOWER than right ear on screen (larger y) → head tilted RIGHT → positive roll
        float rollAngle = 0.0f;
        if (haveEars)
        {
            float rollRaw = (le.y - re.y) / earDist;   // positive = left ear lower
            rollAngle = glm::clamp(rollRaw * 1.0f, -0.6f, 0.6f);
        }

        // Apply to every LookAt binding that has no active color marker
        for (const auto &binding : m_bindings)
        {
            if (binding.mode != MarkerBinding::Mode::LookAt) continue;
            if (byId.count(binding.markerId))              continue; // color marker takes priority

            int jointIndex = skeleton.FindJoint(binding.boneName);
            if (jointIndex < 0) continue;
            Geni::GameObject *bone = skeleton.GetJointNode(jointIndex);
            if (!bone) continue;

            glm::mat4 bindWorld  = glm::inverse(skeleton.GetInverseBindMatrix(jointIndex));
            glm::quat headBindRot = glm::quat_cast(bindWorld);

            // Compose: yaw first (around world Y), then pitch (world X), then roll (world Z)
            glm::quat yawQ   = glm::angleAxis(yawAngle,   glm::vec3(0.0f, 1.0f, 0.0f));
            glm::quat pitchQ = glm::angleAxis(pitchAngle, glm::vec3(1.0f, 0.0f, 0.0f));
            glm::quat rollQ  = glm::angleAxis(rollAngle,  glm::vec3(0.0f, 0.0f, 1.0f));

            glm::quat headWorldRot = yawQ * pitchQ * rollQ * headBindRot;
            bone->SetRotation(WorldToLocalRot(bone, headWorldRot));
        }
    }

    // ── MediaPipe body rotation & lean tracking ────────────────────────────────
    // Uses shoulder landmarks to detect:
    //  • Torso YAW  – body turning left/right (shoulder Z difference from world landmarks)
    //  • Torso LEAN – body shifting left/right (shoulder X midpoint vs centre)
    // Applied to common Mixamo hip/spine bone names. Falls back gracefully when
    // the bone is not present in the loaded skeleton.
    if (!mpPose.empty() &&
        mpPose.hasJoint(MP_LEFT_SHOULDER,  0.3f) &&
        mpPose.hasJoint(MP_RIGHT_SHOULDER, 0.3f))
    {
        glm::vec3 ls = mpPose.getJoint(MP_LEFT_SHOULDER);
        glm::vec3 rs = mpPose.getJoint(MP_RIGHT_SHOULDER);

        // ── Torso YAW: shoulder Z-difference ──────────────────────────────
        // When the body turns RIGHT the right shoulder (rs, MP const 12 = person's left)
        // goes BACK (z more positive) and left shoulder (ls, MP const 11 = person's right)
        // comes FORWARD (z more negative).
        //   ls.z - rs.z < 0  →  body turned RIGHT  →  negative yaw (CW from above)
        float bodyYawRaw   = (ls.z - rs.z) * 2.5f;   // scale: ±0.3 m -> ±0.75 rad
        float bodyYawAngle = glm::clamp(bodyYawRaw, -glm::pi<float>() * 0.45f,
                                                     glm::pi<float>() * 0.45f);

        // ── Torso LEAN: shoulder X midpoint ───────────────────────────────
        // Shoulder midpoint shifts left/right when the body leans or steps.
        // (ls.x + rs.x)/2 > 0.5 → body shifted RIGHT on screen.
        // Without X negation (same convention as colour markers):
        //   high midX → positive worldX → body leans to character's right.
        // We express this as a roll angle around the world Z axis.
        float shoulderMidX  = (ls.x + rs.x) * 0.5f;
        float bodyLeanRaw   = (shoulderMidX - 0.5f) * 1.5f;   // [-0.75, +0.75] rad
        float bodyLeanAngle = glm::clamp(bodyLeanRaw, -0.5f, 0.5f);

        // Apply to the FIRST bone found among standard Mixamo torso bone names.
        static const std::vector<std::string> TORSO_BONES = {
            "mixamorig:Hips", "mixamorig:Spine", "Hips", "Spine"
        };
        for (const auto &boneName : TORSO_BONES)
        {
            int jointIndex = skeleton.FindJoint(boneName);
            if (jointIndex < 0) continue;
            Geni::GameObject *bone = skeleton.GetJointNode(jointIndex);
            if (!bone) continue;

            glm::mat4 bindWorld   = glm::inverse(skeleton.GetInverseBindMatrix(jointIndex));
            glm::quat torsoBindRot = glm::quat_cast(bindWorld);

            glm::quat yawQ  = glm::angleAxis(bodyYawAngle,  glm::vec3(0.0f, 1.0f, 0.0f));
            glm::quat leanQ = glm::angleAxis(bodyLeanAngle, glm::vec3(0.0f, 0.0f, 1.0f));

            glm::quat torsoWorldRot = yawQ * leanQ * torsoBindRot;
            bone->SetRotation(WorldToLocalRot(bone, torsoWorldRot));
            break;  // only drive one torso bone
        }
    }
}
