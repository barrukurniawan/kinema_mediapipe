import cv2
import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision
import socket
import json
import time
import os

# ── Model path ────────────────────────────────────────────────────────────────
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MODEL_PATH = os.path.join(SCRIPT_DIR, "pose_landmarker_lite.task")
if not os.path.exists(MODEL_PATH):
    raise FileNotFoundError(
        f"Pose model not found at {MODEL_PATH}.\n"
        "Run once to download it:\n"
        "  python -c \"import urllib.request; urllib.request.urlretrieve("
        "'https://storage.googleapis.com/mediapipe-models/pose_landmarker/"
        "pose_landmarker_lite/float16/1/pose_landmarker_lite.task', "
        "'tools/pose_landmarker_lite.task')\""
    )

# ── UDP Socket ────────────────────────────────────────────────────────────────
UDP_IP   = "127.0.0.1"
UDP_PORT = 8080
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# ── MediaPipe Pose Landmarker (Tasks API) ─────────────────────────────────────
# This is the modern replacement for the deprecated mp.solutions.pose API.
# It works natively on Apple Silicon (M-series) Macs.
base_options = python.BaseOptions(model_asset_path=MODEL_PATH)
options = vision.PoseLandmarkerOptions(
    base_options=base_options,
    running_mode=vision.RunningMode.VIDEO,
    num_poses=1,
    min_pose_detection_confidence=0.5,
    min_pose_presence_confidence=0.5,
    min_tracking_confidence=0.5,
)
landmarker = vision.PoseLandmarker.create_from_options(options)

# ── Webcam ────────────────────────────────────────────────────────────────────
cap = cv2.VideoCapture(0)
print(f"Starting MediaPipe Tracker (Tasks API)...")
print(f"Sending UDP stream to {UDP_IP}:{UDP_PORT}")
print("Press 'q' to quit.")

try:
    frame_timestamp_ms = 0
    while cap.isOpened():
        ret, frame = cap.read()
        if not ret:
            print("Failed to grab frame")
            break

        frame_timestamp_ms = int(time.time() * 1000)  # monotonically increasing

        # Convert BGR → RGB for MediaPipe
        rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb_frame)

        # Run pose detection
        result = landmarker.detect_for_video(mp_image, frame_timestamp_ms)

        if result.pose_landmarks:
            landmarks_data = []
            for idx, lm in enumerate(result.pose_landmarks[0]):
                landmarks_data.append({
                    "id": idx,
                    "x":  lm.x,
                    "y":  lm.y,
                    "z":  lm.z,
                    "v":  lm.visibility if hasattr(lm, "visibility") else 1.0,
                })

            message = {"timestamp": time.time(), "landmarks": landmarks_data}
            sock.sendto(json.dumps(message).encode("utf-8"), (UDP_IP, UDP_PORT))

            # Draw skeleton overlay on the frame
            for lm in result.pose_landmarks[0]:
                h, w, _ = frame.shape
                cx, cy = int(lm.x * w), int(lm.y * h)
                cv2.circle(frame, (cx, cy), 4, (0, 255, 0), -1)

        cv2.imshow("MediaPipe Tracker", frame)
        if cv2.waitKey(1) & 0xFF == ord("q"):
            break

except KeyboardInterrupt:
    print("\nStopping...")
finally:
    landmarker.close()
    cap.release()
    cv2.destroyAllWindows()
    sock.close()
    print("Tracker stopped.")
