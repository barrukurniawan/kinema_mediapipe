#include "modules/UDPReceiver.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

UDPReceiver::UDPReceiver(int port) : m_port(port) {}

UDPReceiver::~UDPReceiver() {
    Stop();
}

bool UDPReceiver::Start() {
    m_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket < 0) {
        std::cerr << "UDPReceiver: Failed to create socket" << std::endl;
        return false;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(m_port);

    if (bind(m_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "UDPReceiver: Failed to bind to port " << m_port << std::endl;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // Set non-blocking timeout for socket so we can stop the thread gracefully
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    m_running = true;
    m_thread = std::thread(&UDPReceiver::ReceiveLoop, this);
    std::cout << "UDPReceiver: Listening on port " << m_port << std::endl;
    return true;
}

void UDPReceiver::Stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
    }
}

MediaPipePose UDPReceiver::GetLatestPose() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_latestPose;
}

void UDPReceiver::ReceiveLoop() {
    char buffer[65536];
    while (m_running) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int bytes_received = recvfrom(m_socket, buffer, sizeof(buffer) - 1, 0,
                                      (struct sockaddr*)&client_addr, &client_len);
                                      
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            try {
                json j = json::parse(buffer);
                MediaPipePose pose;
                pose.timestamp = j.value("timestamp", 0.0);
                
                if (j.contains("landmarks")) {
                    for (const auto& lm : j["landmarks"]) {
                        MediaPipeLandmark m;
                        m.id = lm.value("id", 0);
                        m.pos = glm::vec3(lm.value("x", 0.0f), lm.value("y", 0.0f), lm.value("z", 0.0f));
                        m.visibility = lm.value("v", 0.0f);
                        pose.landmarks.push_back(m);
                    }
                }
                
                std::lock_guard<std::mutex> lock(m_mutex);
                m_latestPose = std::move(pose);
            } catch (const std::exception& e) {
                // Ignore parse errors from malformed packets
            }
        }
    }
}
