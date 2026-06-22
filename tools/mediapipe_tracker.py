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
                "face":      (200, 200, 200),   # light grey
                "torso":     (0,   255,  80),   # green
                "right_arm": (255, 100,  50),   # orange-blue
                "left_arm":  (50,  100, 255),   # orange-red
                "right_leg": (255, 200,  50),   # cyan-ish
                "left_leg":  (50,  200, 255),   # yellow-ish
            }

            h, w, _ = frame.shape
            lms = result.pose_landmarks[0]

            # Draw connections (lines)
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
                    cv2.line(frame, (x1, y1), (x2, y2), color, 2, cv2.LINE_AA)

            # Draw landmark dots on top
            for idx, lm in enumerate(lms):
                vis = lm.visibility if hasattr(lm, "visibility") else 1.0
                if vis < 0.3:
                    continue
                cx, cy = int(lm.x * w), int(lm.y * h)
                # Face landmarks: smaller white dot; body: filled colored circle
                if idx <= 10:
                    cv2.circle(frame, (cx, cy), 3, (255, 255, 255), -1, cv2.LINE_AA)
                else:
                    cv2.circle(frame, (cx, cy), 6, (255, 255, 255), -1, cv2.LINE_AA)
                    cv2.circle(frame, (cx, cy), 4, (80,  80, 200), -1, cv2.LINE_AA)

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
