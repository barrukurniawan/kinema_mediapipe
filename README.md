# Kinema

**Kinema** is a real-time motion capture and 3D skeleton driving application. It tracks colored markers via a webcam and maps that motion onto a rigged 3D model in real-time.

![Kinema Interface](assets/preview.png) *(Placeholder for your preview image)*

## Key Features

- **HSV Color Tracking**: Tracks bright, solid-colored markers (orange, green, yellow, red).
- **Real-time Skeleton Driving**: Map tracked points to specific bones (Position, Rotation, IK Target, or Look At).
- **Built-in IK Solver**: 2-bone analytical IK for arms and legs.
- **Recording & Playback**: Record motion sequences and preview them immediately on the 3D rig.
- **Flexible Export**:
  - **JSON**: Export per-bone local-space keyframes.
  - **Video**: Render and export your motion as MP4 or AVI.
  - **GLB**: Bundle the recorded animation back into the source GLB file.

---

## Getting Started

### Prerequisites

- **CMake** (3.15+)
- **C++17 Compiler** (MSVC on Windows, Clang on macOS)
- **Webcam**

### Building on Windows (Recommended)

We recommend using **vcpkg** for dependency management.

1.  **Install vcpkg**:
    ```powershell
    git clone https://github.com/microsoft/vcpkg.git
    cd vcpkg
    .\bootstrap-vcpkg.bat
    ```
2.  **Configure and Build**:
    From the `Kinema` root directory:
    ```powershell
    # Replace [path/to/vcpkg] with your actual installation path
    cmake -B build -S . "-DCMAKE_TOOLCHAIN_FILE=[path/to/vcpkg]/scripts/buildsystems/vcpkg.cmake"
    cmake --build build --config Release
    ```
3.  **Run**:
    ```powershell
    ./build/Release/kinema.exe
    ```

### Building on macOS

1.  **Install Dependencies**:
    ```bash
    brew install opencv glfw glew cmake
    ```
2.  **Build**:
    ```bash
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ```
3.  **Run**:
    ```bash
    ./build/kinema
    ```

---

## MediaPipe Integration (Sensor Fusion)

Kinema supports combining MediaPipe's AI skeletal tracking with color marker tracking (Inverse Kinematics) for smoother, more reliable motion capture. This is run as a separate Python microservice that streams data via UDP.

### 1. Python Setup (macOS / Apple Silicon M-Series)

> **Important:** MediaPipe only supports **Python 3.12 or lower**. It does NOT work with Python 3.13/3.14. We use a dedicated virtual environment to avoid conflicts.

**Step 1 — Install Python 3.12 (if not already installed):**
```bash
brew install python@3.12
```

**Step 2 — Create a virtual environment inside the Kinema folder:**
```bash
python3.12 -m venv tools/mediapipe-env
```

**Step 3 — Activate the environment:**
```bash
source tools/mediapipe-env/bin/activate
```

**Step 4 — Install required packages:**
```bash
pip install mediapipe opencv-python
```

You only need to do steps 1–4 **once**. After that, just activate the environment before running the tracker.

### 2. Running the Tracker

Every time you want to use MediaPipe tracking, open a terminal and run:
```bash
# From the Kinema root directory
source tools/mediapipe-env/bin/activate
python tools/mediapipe_tracker.py
```
This opens your webcam, runs the AI pose estimation, and streams the 33 body landmarks to Kinema via UDP on port `8080`.

Press `q` in the tracker window to quit.

### 3. Sensor Fusion in Kinema
- When the tracker is running, Kinema will automatically use the AI data for the base skeleton (spine, shoulders, basic arm position).
- If you use color markers (e.g. a green marker taped to your palm), Kinema's **Inverse Kinematics (IK)** will override the AI's hand position with the highly precise color marker position — giving you the best of both worlds.
- If the Python tracker is **not** running, Kinema falls back to color-only tracking as before.

---

## Usage

1.  **Load a Model**: Use the **Model** panel to load a rigged GLB (Mixamo rigs work best).
2.  **Configure HSV Ranges**: Create marker slots, tune the Hue/Saturation/Value ranges, and bind them to specific bones or IK targets.
3.  **Add Markers**: Create marker slots and bind them to specific bones or IK targets.
4.  **Record**: Hit **Start Recording**, perform your motion, and then **Reconstruct 3D** to preview.
5.  **Export**: Save your animation as a GLB or JSON for use in Blender, Unity, or web apps.

For a detailed walkthrough of all interface panels, see the [User Guide (USAGE.md)](USAGE.md).

---

## Troubleshooting

- **No Camera?** Ensure your webcam isn't being used by another app. On macOS, check System Permissions.
- **Marker not detected?** Adjust the Hue/Saturation/Value sliders in the Markers panel. Bright lighting is essential for HSV tracking.
- **Low FPS?** Ensure you are running a **Release** build.

## Credits

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
