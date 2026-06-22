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
            world_lms = (result.pose_world_landmarks[0] if result.pose_world_landmarks else None)
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

                landmarks_data.append({
                    "id": idx,
                    "x":  new_x,
                    "y":  new_y,
                    "z":  new_z,
                    "v":  lm.visibility if hasattr(lm, "visibility") else 1.0,
                })

            message = {"timestamp": time.time(), "landmarks": landmarks_data}
            sock.sendto(json.dumps(message).encode("utf-8"), (UDP_IP, UDP_PORT))

        # Sleep briefly to yield thread and prevent 100% CPU usage
        time.sleep(0.001)

except KeyboardInterrupt:
    pass
finally:
    if cap.isOpened():
        cap.release()
    cv2.destroyAllWindows()
    sock.close()

)PYTHON_EOF";
