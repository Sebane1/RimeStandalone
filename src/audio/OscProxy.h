#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <atomic>
#include <string>

class OscProxy {
public:
    OscProxy(uint16_t listenPort, uint16_t forwardPort);
    ~OscProxy();

    bool start();
    void stop();

    void injectSolarXrRotation(float yaw, float pitch, float roll);
    void triggerReset();

    void setDriftMultiplier(float multiplier);

private:
    void proxyLoop();
    void processPacket(char* buffer, int length);
    void forward(const char* buffer, int length);
    float applyDriftCorrection(float rawYaw);

    uint16_t m_listenPort;
    uint16_t m_forwardPort;
    std::atomic<bool> m_running;
    std::thread m_thread;
    
    SOCKET m_listenSocket;
    SOCKET m_forwardSocket;
    sockaddr_in m_forwardAddr;

    // Algorithm state
    float m_driftOffset;
    float m_lastRawYaw;
    uint32_t m_lastTimeMs;

    // Tunable Parameters
    float m_stillThreshold; // degrees per sec (pause drift if moving faster)
    float m_driftSpeed;     // degrees per sec to correct towards center
    float m_driftMultiplier; // dynamic speed multiplier based on hardware IMU quality

    std::atomic<bool> m_solarXrActive;
};
