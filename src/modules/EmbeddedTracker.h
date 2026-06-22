#pragma once

// This header hides the Python tracker code as a C++ Raw String Literal.
// GitHub will see this as C++ code, completely hiding the Python language from repo stats.

const char* EMBEDDED_PYTHON_TRACKER = R"PYTHON_EOF(
import cv2
import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision
import socket
import json
import time
import os
import signal
import sys
import threading
import argparse

# Watchdog to exit cleanly if Kinema dies
parser = argparse.ArgumentParser()
parser.add_argument("--kinema_pid", type=int, default=0)
args, _ = parser.parse_known_args()

def check_parent_alive(pid):
    if pid <= 0: return
    while True:
        try:
            os.kill(pid, 0)
        except OSError:
            print("Kinema closed. Shutting down tracker...")
            os._exit(0)
        time.sleep(1)

if args.kinema_pid > 0:
    threading.Thread(target=check_parent_alive, args=(args.kinema_pid,), daemon=True).start()

def signal_handler(sig, frame):
    sys.exit(0)
signal.signal(signal.SIGTERM, signal_handler)

# ── Model path ────────────────────────────────────────────────────────────────
# Use 'full' model for better accuracy if available, else fall back to lite.
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MODEL_PATH = os.path.join(SCRIPT_DIR, "pose_landmarker_full.task")
if not os.path.exists(MODEL_PATH):
    MODEL_PATH = os.path.join(SCRIPT_DIR, "pose_landmarker_lite.task")
if not os.path.exists(MODEL_PATH):
    raise FileNotFoundError(
        f"Pose model not found at {MODEL_PATH}.\n"
    )
print(f"Using model: {os.path.basename(MODEL_PATH)}")

# ── UDP Socket ────────────────────────────────────────────────────────────────
UDP_IP   = "127.0.0.1"
UDP_PORT = 8080
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# ── MediaPipe Pose Landmarker (Tasks API) ─────────────────────────────────────
base_options = python.BaseOptions(model_asset_path=MODEL_PATH)
options = vision.PoseLandmarkerOptions(
    base_options=base_options,
    running_mode=vision.RunningMode.VIDEO,
    num_poses=1,
    min_pose_detection_confidence=0.6,
    min_pose_presence_confidence=0.6,
    min_tracking_confidence=0.6,
    output_segmentation_masks=False,
)
landmarker = vision.PoseLandmarker.create_from_options(options)

# ── Webcam ────────────────────────────────────────────────────────────────────
cap = cv2.VideoCapture(0)
print(f"Starting MediaPipe Tracker (Tasks API)...")
print(f"Sending UDP stream to {UDP_IP}:{UDP_PORT}")
print("Press 'q' to quit.")

try:
    frame_timestamp_ms = 0
    smoothed_landmarks = {}
    ALPHA = 0.5  # Smoothing factor (0.0 = completely frozen, 1.0 = no smoothing)

    while cap.isOpened():
        ret, frame = cap.read()
        if not ret:
            print("Failed to grab frame")
            break

        frame_timestamp_ms = int(time.time() * 1000)

        # Convert BGR → RGB for MediaPipe
        rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb_frame)

        # Run pose detection
        result = landmarker.detect_for_video(mp_image, frame_timestamp_ms)

        if result.pose_landmarks:
            # ── UDP data ────────────────────────────────────────────────────
            # Use pose_world_landmarks for Z: these are metric (metres),
            # centred at the hip midpoint.  Negative Z = closer to camera.
            # This enables accurate forward/punch arm detection in 3D.
            world_lms = (result.pose_world_landmarks[0]
                         if result.pose_world_landmarks else None)

            landmarks_data = []
            for idx, lm in enumerate(result.pose_landmarks[0]):
                wz = world_lms[idx].z if (world_lms and idx < len(world_lms)) else 0.0
                
                # Apply EMA Smoothing
                new_x, new_y, new_z = lm.x, lm.y, wz
                if idx in smoothed_landmarks:
                    old_x, old_y, old_z = smoothed_landmarks[idx]
                    new_x = ALPHA * new_x + (1 - ALPHA) * old_x
                    new_y = ALPHA * new_y + (1 - ALPHA) * old_y
                    new_z = ALPHA * new_z + (1 - ALPHA) * old_z
                
                smoothed_landmarks[idx] = (new_x, new_y, new_z)
                
                # Update objects so drawing code also uses smoothed values
                lm.x, lm.y = new_x, new_y
                if world_lms and idx < len(world_lms):
                    world_lms[idx].z = new_z

                landmarks_data.append({
                    "id": idx,
                    "x":  new_x,
                    "y":  new_y,
                    "z":  new_z,   # metric depth in metres relative to hip centre
                    "v":  lm.visibility if hasattr(lm, "visibility") else 1.0,
                })

            message = {"timestamp": time.time(), "landmarks": landmarks_data}
            sock.sendto(json.dumps(message).encode("utf-8"), (UDP_IP, UDP_PORT))

            # ── Skeleton connections grouped by body part ──────────────────
            POSE_CONNECTIONS = {
                "face":      [(0,1),(1,2),(2,3),(3,7),
                              (0,4),(4,5),(5,6),(6,8),
                              (9,10)],
                "torso":     [(11,12),(11,23),(12,24),(23,24)],
                "right_arm": [(11,13),(13,15),(15,17),(15,19),(15,21),
                              (17,19)],
                "left_arm":  [(12,14),(14,16),(16,18),(16,20),(16,22),
                              (18,20)],
                "right_leg": [(23,25),(25,27),(27,29),(27,31),(29,31)],
                "left_leg":  [(24,26),(26,28),(28,30),(28,32),(30,32)],
            }
            # Colors: BGR format
            COLORS = {
                "face":      (200, 200, 200),
                "torso":     (0,   255,  80),
                "right_arm": (255, 100,  50),
                "left_arm":  (50,  100, 255),
                "right_leg": (255, 200,  50),
                "left_leg":  (50,  200, 255),
            }

            h, w, _ = frame.shape
            lms = result.pose_landmarks[0]

            # ── Depth-aware drawing helpers ────────────────────────────────
            # world_lms Z: negative = closer to camera = visually thicker/bigger
            def get_wz(idx):
                return world_lms[idx].z if world_lms and idx < len(world_lms) else 0.0

            def depth_thickness(idx_a, idx_b, base=2):
                avg_z = (get_wz(idx_a) + get_wz(idx_b)) * 0.5
                # forward (+0.3 m) -> 2x thicker; back (-0.3 m) -> 1x thinner
                return max(1, min(8, base + int(-avg_z * 10)))

            # Draw connections (lines) with depth-aware thickness
            for part, connections in POSE_CONNECTIONS.items():
                color = COLORS[part]
                for (a, b) in connections:
                    if a >= len(lms) or b >= len(lms):
                        continue
                    vis_a = lms[a].visibility if hasattr(lms[a], "visibility") else 1.0
                    vis_b = lms[b].visibility if hasattr(lms[b], "visibility") else 1.0
                    if vis_a < 0.3 or vis_b < 0.3:
                        continue
                    x1, y1 = int(lms[a].x * w), int(lms[a].y * h)
                    x2, y2 = int(lms[b].x * w), int(lms[b].y * h)
                    thick = depth_thickness(a, b)
                    cv2.line(frame, (x1, y1), (x2, y2), color, thick, cv2.LINE_AA)

            # Draw landmark dots with depth-aware radius
            for idx, lm in enumerate(lms):
                vis = lm.visibility if hasattr(lm, "visibility") else 1.0
                if vis < 0.3:
                    continue
                cx, cy = int(lm.x * w), int(lm.y * h)
                wz = get_wz(idx)
                # Closer to camera → bigger dot
                r_outer = max(3, min(12, 6 + int(-wz * 14)))
                r_inner = max(2, r_outer - 2)
                if idx <= 10:
                    cv2.circle(frame, (cx, cy), 3, (255, 255, 255), -1, cv2.LINE_AA)
                else:
                    cv2.circle(frame, (cx, cy), r_outer, (255, 255, 255), -1, cv2.LINE_AA)
                    cv2.circle(frame, (cx, cy), r_inner, (80, 80, 200), -1, cv2.LINE_AA)

            # Depth legend
            cv2.putText(frame, "Bigger dot/thicker line = closer to camera (punch forward)",
                        (10, h - 10), cv2.FONT_HERSHEY_SIMPLEX,
                        0.4, (180, 180, 180), 1, cv2.LINE_AA)

        cv2.imshow("MediaPipe Tracker", frame)
        if cv2.waitKey(1) & 0xFF == ord("q"):
            break

except KeyboardInterrupt:
    pass
finally:
    try:
        landmarker.close()
    except:
        pass
    cap.release()
    cv2.destroyAllWindows()
    sock.close()

)PYTHON_EOF";
