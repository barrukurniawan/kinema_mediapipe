#pragma once
#include <vector>
#include <glm/glm.hpp>
#include <mutex>
#include <thread>
#include <atomic>

// MediaPipe landmark IDs used for arms (from mediapipe pose)
constexpr int MP_RIGHT_SHOULDER = 12;
constexpr int MP_LEFT_SHOULDER = 11;
constexpr int MP_RIGHT_ELBOW = 14;
constexpr int MP_LEFT_ELBOW = 13;
constexpr int MP_RIGHT_WRIST = 16;
constexpr int MP_LEFT_WRIST = 15;

struct MediaPipeLandmark {
    int id;
    glm::vec3 pos; // raw normalized (x,y) and metric (z)
    float visibility;
};

struct MediaPipePose {
    double timestamp = 0.0;
    std::vector<MediaPipeLandmark> landmarks;
    
    glm::vec3 getJoint(int id) const {
        for (const auto& lm : landmarks) {
            if (lm.id == id) return lm.pos;
        }
        return glm::vec3(0.0f);
    }
    
    bool hasJoint(int id, float visibility_threshold = 0.5f) const {
        for (const auto& lm : landmarks) {
            if (lm.id == id && lm.visibility >= visibility_threshold) return true;
        }
        return false;
    }

    bool empty() const {
        return landmarks.empty();
    }
};

class UDPReceiver {
public:
    UDPReceiver(int port = 8080);
    ~UDPReceiver();

    bool Start();
    void Stop();

    // Returns a copy of the latest pose data
    MediaPipePose GetLatestPose();

private:
    void ReceiveLoop();

    int m_port;
    int m_socket = -1;
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    std::mutex m_mutex;
    MediaPipePose m_latestPose;
};
