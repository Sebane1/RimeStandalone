#include "OscProxy.h"
#include <iostream>
#include <cmath>

#pragma comment(lib, "ws2_32.lib")

static float readBEFloat(const char* ptr) {
    uint32_t raw;
    std::memcpy(&raw, ptr, 4);
    raw = ntohl(raw);
    float f;
    std::memcpy(&f, &raw, 4);
    return f;
}

static void writeBEFloat(char* ptr, float f) {
    uint32_t raw;
    std::memcpy(&raw, &f, 4);
    raw = htonl(raw);
    std::memcpy(ptr, &raw, 4);
}

OscProxy::OscProxy(uint16_t listenPort, uint16_t forwardPort)
    : m_listenPort(listenPort), 
      m_forwardPort(forwardPort), 
      m_running(false),
      m_listenSocket(INVALID_SOCKET),
      m_forwardSocket(INVALID_SOCKET),
      m_driftOffset(0.0f),
      m_lastRawYaw(0.0f),
      m_lastTimeMs(0),
      m_stillThreshold(8.0f),
      m_driftSpeed(0.5f),
      m_driftMultiplier(1.0f),
      m_solarXrActive(false)
{
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

OscProxy::~OscProxy() {
    stop();
    WSACleanup();
}

bool OscProxy::start() {
    if (m_running) return true;

    m_listenSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_listenSocket == INVALID_SOCKET) return false;

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(m_listenPort);

    if (bind(m_listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(m_listenSocket);
        return false;
    }

    m_forwardSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_forwardSocket == INVALID_SOCKET) {
        closesocket(m_listenSocket);
        return false;
    }

    m_forwardAddr = {};
    m_forwardAddr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &m_forwardAddr.sin_addr);
    m_forwardAddr.sin_port = htons(m_forwardPort);

    m_running = true;
    m_thread = std::thread(&OscProxy::proxyLoop, this);
    
    std::cout << "[OSC Proxy] Listening on port " << m_listenPort 
              << ", Auto-Centering Yaw, Forwarding to port " << m_forwardPort << "\n";
    return true;
}

void OscProxy::stop() {
    if (!m_running) return;
    m_running = false;
    
    if (m_listenSocket != INVALID_SOCKET) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }
    if (m_forwardSocket != INVALID_SOCKET) {
        closesocket(m_forwardSocket);
        m_forwardSocket = INVALID_SOCKET;
    }
    
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void OscProxy::proxyLoop() {
    char buffer[2048];
    while (m_running) {
        int bytesRead = recv(m_listenSocket, buffer, sizeof(buffer), 0);
        if (bytesRead > 0) {
            processPacket(buffer, bytesRead);
        } else {
            if (WSAGetLastError() != WSAEINTR) {
                // Socket error, abort
                break;
            }
        }
    }
}

void OscProxy::forward(const char* buffer, int length) {
    if (m_forwardSocket != INVALID_SOCKET) {
        sendto(m_forwardSocket, buffer, length, 0, (sockaddr*)&m_forwardAddr, sizeof(m_forwardAddr));
    }
}

void OscProxy::processPacket(char* buffer, int length) {
    // We only process if it's large enough to hold an address, type tag, and at least 3 floats
    if (length < 16) return forward(buffer, length);

    // Find the end of the address string
    int addrLen = 0;
    while (addrLen < length && buffer[addrLen] != '\0') addrLen++;
    int paddedAddrLen = (addrLen + 4) & ~3; // OSC strings are padded to 4-byte boundaries
    
    if (paddedAddrLen >= length) return forward(buffer, length);

    std::string address(buffer, addrLen);
    
    // Check for explicit configuration commands from SlimeVR Server
    if (address == "/rime/reset") {
        m_driftOffset = 0.0f;
        std::cout << "[OSC Proxy] Received explicit recalibration command. Offset zeroed.\n";
        return;
    }

    // Parse Type Tag
    int typeIdx = paddedAddrLen;
    if (buffer[typeIdx] != ',') return forward(buffer, length);

    int typeLen = 0;
    while (typeIdx + typeLen < length && buffer[typeIdx + typeLen] != '\0') typeLen++;
    int paddedTypeLen = (typeLen + 4) & ~3;

    int dataIdx = typeIdx + paddedTypeLen;
    std::string typeTag(buffer + typeIdx, typeLen);
    
    // Handle configuration variable updates
    if (address == "/rime/drift_speed" || address == "/rime/drift_threshold") {
        if (typeTag == ",f" && dataIdx + 4 <= length) {
            float val = readBEFloat(buffer + dataIdx);
            if (address == "/rime/drift_speed") {
                m_driftSpeed = val;
                std::cout << "[OSC Proxy] Drift speed updated to " << m_driftSpeed << " deg/sec\n";
            } else {
                m_stillThreshold = val;
                std::cout << "[OSC Proxy] Still threshold updated to " << m_stillThreshold << " deg/sec\n";
            }
        }
        return;
    }

    // We only care about /ypr
    if (address != "/ypr") return forward(buffer, length);

    // Suppress UDP OSC drift correction if native FlatBuffers are actively streaming to us
    if (m_solarXrActive) return;

    // We need at least 3 floats (12 bytes)
    if (dataIdx + 12 > length) return forward(buffer, length);

    // We expect ,fff for three floats
    if (typeTag != ",fff") return forward(buffer, length);

    // Parse the yaw, pitch, roll
    // /ypr <yaw> <pitch> <roll>
    float rawYaw = readBEFloat(buffer + dataIdx);
    
    // Apply the drift correction to center the view
    float correctedYaw = applyDriftCorrection(rawYaw);

    // Pack the corrected yaw back into the binary OSC payload
    writeBEFloat(buffer + dataIdx, correctedYaw);

    // Forward the packet (now containing the corrected yaw) to the plugin!
    forward(buffer, length);
}

void OscProxy::triggerReset() {
    m_driftOffset = 0.0f;
    std::cout << "[OSC Proxy] Received explicit recalibration command. Offset zeroed.\n";
}

void OscProxy::setDriftMultiplier(float multiplier) {
    m_driftMultiplier = multiplier;
}

float OscProxy::applyDriftCorrection(float rawYaw) {
    uint32_t now = GetTickCount();
    if (m_lastTimeMs != 0) {
        float dt = (now - m_lastTimeMs) / 1000.0f;
        if (dt > 0.0f && dt < 1.0f) { // Ignore huge latency spikes
            
            // Calculate angular velocity to see if user is looking around
            float deltaYaw = rawYaw - m_lastRawYaw;
            while (deltaYaw > 180.0f) deltaYaw -= 360.0f;
            while (deltaYaw < -180.0f) deltaYaw += 360.0f;
            
            float velocity = std::abs(deltaYaw) / dt;
            
            // Detect SlimeVR Recalibration/Reset event via teleport-jump (Legacy OSC mode fallback)
            // If the angular velocity is impossibly fast for a human (> 1000 deg/sec) and lands near zero, it's a reset.
            if (std::abs(rawYaw) < 2.0f && velocity > 1000.0f) {
                m_driftOffset = 0.0f; // Instant reset
                std::cout << "[OSC Proxy] Reset detected via zero-jump teleport! Offset zeroed.\n";
            }
            // If the head is relatively still, slowly drift the offset back towards center
            else if (velocity < m_stillThreshold) {
                // Calculate current corrected yaw to determine which zone we are in
                float currentCorrected = rawYaw - m_driftOffset;
                while (currentCorrected > 180.0f) currentCorrected -= 360.0f;
                while (currentCorrected < -180.0f) currentCorrected += 360.0f;
                
                // Determine target snap angle
                float targetYaw;
                if (currentCorrected >= -15.0f && currentCorrected <= 15.0f) {
                    // Strong magnetic snap to true center when looking roughly forward
                    targetYaw = 0.0f;
                } else {
                    // Lock to the nearest 1 degree increment elsewhere
                    const float SNAP_INTERVAL = 1.0f;
                    targetYaw = std::round(currentCorrected / SNAP_INTERVAL) * SNAP_INTERVAL;
                }
                
                // Ensure target yaw stays strictly within the standard -180 to 180 range
                while (targetYaw > 180.0f) targetYaw -= 360.0f;
                while (targetYaw <= -180.0f) targetYaw += 360.0f;
                
                // Calculate where the offset SHOULD be to achieve the target yaw
                float targetOffset = rawYaw - targetYaw;
                
                // Calculate distance from current offset to target offset
                float offsetError = targetOffset - m_driftOffset;
                while (offsetError > 180.0f) offsetError -= 360.0f;
                while (offsetError < -180.0f) offsetError += 360.0f;
                
                float currentSpeed = m_driftSpeed;
                if (targetYaw == 0.0f) {
                    currentSpeed *= 3.0f; // 3x faster correction when snapping to true center
                }
                float maxDrift = currentSpeed * m_driftMultiplier * dt;
                
                if (std::abs(offsetError) <= maxDrift) {
                    m_driftOffset = targetOffset; // Snapped
                } else if (offsetError > 0.0f) {
                    m_driftOffset += maxDrift;
                } else {
                    m_driftOffset -= maxDrift;
                }
                
                // Keep drift offset wrapped cleanly
                while (m_driftOffset > 180.0f) m_driftOffset -= 360.0f;
                while (m_driftOffset <= -180.0f) m_driftOffset += 360.0f;
            }
        }
    }
    m_lastTimeMs = now;
    m_lastRawYaw = rawYaw;

    // Apply the offset to center the view
    float correctedYaw = rawYaw - m_driftOffset;
    while (correctedYaw > 180.0f) correctedYaw -= 360.0f;
    while (correctedYaw <= -180.0f) correctedYaw += 360.0f;
    
    return correctedYaw;
}

void OscProxy::injectSolarXrRotation(float yaw, float pitch, float roll) {
    m_solarXrActive = true;
    
    // Apply our custom drift-correction/auto-centering algorithm directly to the native SolarXR feed
    float correctedYaw = applyDriftCorrection(yaw);
    
    // Construct a perfectly formatted /ypr OSC packet and forward it directly to the VST3 plugin
    char buffer[32];
    std::memset(buffer, 0, sizeof(buffer));
    
    std::memcpy(buffer, "/ypr\0\0\0\0", 8);
    std::memcpy(buffer + 8, ",fff\0\0\0\0", 8);
    
    writeBEFloat(buffer + 16, correctedYaw);
    writeBEFloat(buffer + 20, pitch);
    writeBEFloat(buffer + 24, roll);
    
    forward(buffer, 28);
}
