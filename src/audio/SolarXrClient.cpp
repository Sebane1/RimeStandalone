#include <flatbuffers/flatbuffers.h>
#include <solarxr_protocol/generated/all_generated.h>

#include "SolarXrClient.h"
#include "OscProxy.h"
#include <iostream>
#include <vector>
#include <cmath>

#pragma comment(lib, "winhttp.lib")

using namespace solarxr_protocol;
using namespace solarxr_protocol::data_feed;
using namespace solarxr_protocol::data_feed::device_data;
using namespace solarxr_protocol::data_feed::tracker;
using namespace solarxr_protocol::datatypes;
using namespace solarxr_protocol::datatypes::math;
using namespace solarxr_protocol::datatypes::hardware_info;
using namespace solarxr_protocol::pub_sub;
using namespace solarxr_protocol::rpc;

SolarXrClient::SolarXrClient(OscProxy* proxy)
    : m_running(false),
      m_hSession(NULL),
      m_hConnect(NULL),
      m_hRequest(NULL),
      m_hWebSocket(NULL),
      m_proxy(proxy)
{
}

SolarXrClient::~SolarXrClient() {
    stop();
}

bool SolarXrClient::start() {
    if (m_running) return true;

    m_running = true;
    m_thread = std::thread(&SolarXrClient::clientLoop, this);
    
    printf("[SolarXR Client] Thread started.\n");
    return true;
}

void SolarXrClient::stop() {
    if (!m_running) return;
    m_running = false;
    
    if (m_hWebSocket) {
        WinHttpWebSocketClose(m_hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        WinHttpCloseHandle(m_hWebSocket);
        m_hWebSocket = NULL;
    }
    
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void SolarXrClient::clientLoop() {
    while (m_running) {
        connectAndReceive();
        
        if (m_running) {
            printf("[SolarXR Client] Connection dropped or failed. Retrying in 1 second...\n");
            Sleep(1000);
        }
    }
}

void SolarXrClient::connectAndReceive() {
    m_hSession = WinHttpOpen(L"RimeBridge/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!m_hSession) return;

    // Set aggressive 2-second timeouts so it doesn't hang if SlimeVR Server is offline
    WinHttpSetTimeouts(m_hSession, 2000, 2000, 2000, 2000);

    m_hConnect = WinHttpConnect(m_hSession, L"127.0.0.1", 21110, 0);
    if (!m_hConnect) {
        WinHttpCloseHandle(m_hSession);
        return;
    }

    m_hRequest = WinHttpOpenRequest(m_hConnect, L"GET", L"/", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!m_hRequest) {
        WinHttpCloseHandle(m_hConnect);
        WinHttpCloseHandle(m_hSession);
        return;
    }

    if (!WinHttpSetOption(m_hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) goto cleanup;
    if (!WinHttpSendRequest(m_hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto cleanup;
    if (!WinHttpReceiveResponse(m_hRequest, NULL)) goto cleanup;

    m_hWebSocket = WinHttpWebSocketCompleteUpgrade(m_hRequest, NULL);
    if (!m_hWebSocket) goto cleanup;
    
    printf("[SolarXR Client] Connected to SlimeVR Server WebSocket!\n");

    sendDataFeedSubscription();

    {
        std::vector<BYTE> buffer(65536);
        while (m_running) {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;
            
            DWORD dwError = WinHttpWebSocketReceive(m_hWebSocket, buffer.data(), buffer.size(), &bytesRead, &bufferType);
            
            if (dwError != ERROR_SUCCESS) break; // Connection lost
            if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
            
            if (bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE && bytesRead > 0) {
                auto messageBundle = flatbuffers::GetRoot<solarxr_protocol::MessageBundle>(buffer.data());
                
                if (messageBundle->data_feed_msgs()) {
                    for (auto dfMsg : *messageBundle->data_feed_msgs()) {
                        if (dfMsg->message_type() == DataFeedMessage::DataFeedUpdate) {
                            auto update = static_cast<const DataFeedUpdate*>(dfMsg->message());
                            
                            // Extract hardware IMU type to predict drift time dynamically
                            if (update->devices()) {
                                for (auto device : *update->devices()) {
                                    if (device->trackers()) {
                                        for (auto tracker : *device->trackers()) {
                                            if (tracker->info() && tracker->info()->body_part() == BodyPart::HEAD) {
                                                auto imuType = tracker->info()->imu_type();
                                                float driftMultiplier = 1.0f;
                                                switch(imuType) {
                                                    // Recommended (Very low drift)
                                                    case ImuType::ICM45686:
                                                    case ImuType::ICM45605:
                                                    case ImuType::LSM6DSV:
                                                    case ImuType::LSM6DSR:
                                                        driftMultiplier = 0.2f; break;
                                                    // Acceptable
                                                    case ImuType::LSM6DSO:
                                                    case ImuType::LSM6DS3TRC:
                                                        driftMultiplier = 0.5f; break;
                                                    // Poor
                                                    case ImuType::BNO085:
                                                    case ImuType::BNO080:
                                                    case ImuType::BNO086:
                                                    case ImuType::BMI270:
                                                    case ImuType::ICM42688:
                                                    case ImuType::ICM20948:
                                                    case ImuType::BNO055:
                                                        driftMultiplier = 1.0f; break;
                                                    // Avoid (Terrible drift)
                                                    case ImuType::BMI160:
                                                    case ImuType::MPU9250:
                                                    case ImuType::MPU6500:
                                                    case ImuType::MPU6050:
                                                        driftMultiplier = 2.0f; break;
                                                    default:
                                                        driftMultiplier = 1.0f; break;
                                                }
                                                
                                                bool isOpenVR = false;
                                                if (device->hardware_info()) {
                                                    auto hw = device->hardware_info();
                                                    if (hw->manufacturer() && strstr(hw->manufacturer()->c_str(), "OpenVR") != nullptr) isOpenVR = true;
                                                    if (hw->model() && strstr(hw->model()->c_str(), "OpenVR") != nullptr) isOpenVR = true;
                                                    if (hw->display_name() && strstr(hw->display_name()->c_str(), "OpenVR") != nullptr) isOpenVR = true;
                                                }
                                                if (device->custom_name() && strstr(device->custom_name()->c_str(), "OpenVR") != nullptr) isOpenVR = true;

                                                if (isOpenVR) {
                                                    driftMultiplier = 0.0f; // Absolute tracking from HMD, no drift!
                                                }
                                                
                                                if (m_proxy) {
                                                    m_proxy->setDriftMultiplier(driftMultiplier);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            
                            // Process fully calibrated synthetic trackers (bones) instead of raw uncalibrated hardware IMUs
                            if (update->synthetic_trackers()) {
                                for (auto tracker : *update->synthetic_trackers()) {
                                    if (tracker->info() && tracker->info()->body_part() == BodyPart::HEAD) {
                                        if (tracker->rotation()) {
                                            auto rot = tracker->rotation();
                                            float x = rot->x(), y = rot->y(), z = rot->z(), w = rot->w();
                                            
                                            // SlimeVR Global Coordinate System is X-Right, Y-Up, Z-Back
                                            // Map directly to standard Y-Up Euler extraction:
                                            
                                            // Pitch (X-axis)
                                            float sinp = 2.0f * (w * x - y * z);
                                            float pitch = 0.0f;
                                            if (std::abs(sinp) >= 1.0f)
                                                pitch = std::copysign(3.1415926535f / 2.0f, sinp);
                                            else
                                                pitch = std::asin(sinp);
                                            pitch *= (180.0f / 3.1415926535f);

                                            // Yaw (Y-axis)
                                            float siny_cosp = 2.0f * (w * y + z * x);
                                            float cosy_cosp = 1.0f - 2.0f * (x * x + y * y);
                                            float yaw = std::atan2(siny_cosp, cosy_cosp) * (180.0f / 3.1415926535f);

                                            // Roll (Z-axis)
                                            float sinr_cosp = 2.0f * (w * z + x * y);
                                            float cosr_cosp = 1.0f - 2.0f * (x * x + z * z);
                                            float roll = std::atan2(sinr_cosp, cosr_cosp) * (180.0f / 3.1415926535f);

                                            // Invert yaw to match coordinate space
                                            yaw = -yaw;
                                            
                                            static int debugCounter = 0;
                                            if (++debugCounter % 100 == 0) {
                                                printf("[SolarXR Client] Extracted SYNTHETIC HEAD Yaw: %f, Pitch: %f, Roll: %f\n", yaw, pitch, roll);
                                            }

                                            if (m_proxy) {
                                                m_proxy->injectSolarXrRotation(yaw, roll, pitch);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                
                // Process RpcMsgs for Recalibration/Reset events
                if (messageBundle->rpc_msgs()) {
                    for (auto rpcMsg : *messageBundle->rpc_msgs()) {
                        if (rpcMsg->message_type() == RpcMessage::ResetResponse) {
                            auto resetResponse = static_cast<const ResetResponse*>(rpcMsg->message());
                            // ResetStatus enum might be ResetStatus_FINISHED or ResetStatus::FINISHED
                            if (resetResponse->status() == solarxr_protocol::rpc::ResetStatus::FINISHED) { // 1 = FINISHED
                                if (m_proxy) {
                                    m_proxy->triggerReset();
                                }
                            }
                        }
                    }
                }
            }
        }
    }

cleanup:
    if (m_hWebSocket) { WinHttpCloseHandle(m_hWebSocket); m_hWebSocket = NULL; }
    if (m_hRequest) { WinHttpCloseHandle(m_hRequest); m_hRequest = NULL; }
    if (m_hConnect) { WinHttpCloseHandle(m_hConnect); m_hConnect = NULL; }
    if (m_hSession) { WinHttpCloseHandle(m_hSession); m_hSession = NULL; }
}

void SolarXrClient::sendDataFeedSubscription() {
    if (!m_hWebSocket) return;

    flatbuffers::FlatBufferBuilder builder(1024);

    // 1. Build Physical Tracker Mask
    TrackerDataMaskBuilder tdmBuilder1(builder);
    tdmBuilder1.add_info(true);
    tdmBuilder1.add_status(true);
    tdmBuilder1.add_position(true);
    tdmBuilder1.add_rotation(true);
    auto physicalTrackersMask = tdmBuilder1.Finish();

    // 2. Build Device Data Mask
    DeviceDataMaskBuilder ddmBuilder(builder);
    ddmBuilder.add_tracker_data(physicalTrackersMask);
    ddmBuilder.add_device_data(true);
    auto deviceDataMask = ddmBuilder.Finish();

    // 3. Build Synthetic Tracker Mask
    TrackerDataMaskBuilder tdmBuilder2(builder);
    tdmBuilder2.add_info(true);
    tdmBuilder2.add_status(true);
    tdmBuilder2.add_position(true);
    tdmBuilder2.add_rotation(true);
    auto syntheticTrackersMask = tdmBuilder2.Finish();

    // 4. Build DataFeedConfig
    DataFeedConfigBuilder dfcBuilder(builder);
    dfcBuilder.add_minimum_time_since_last(10); // 100fps
    dfcBuilder.add_data_mask(deviceDataMask);
    dfcBuilder.add_synthetic_trackers_mask(syntheticTrackersMask);
    dfcBuilder.add_bone_mask(true);
    auto dataFeedConfig = dfcBuilder.Finish();

    // 5. Build StartDataFeed
    std::vector<flatbuffers::Offset<DataFeedConfig>> dfcVec;
    dfcVec.push_back(dataFeedConfig);
    auto dataFeedsVector = builder.CreateVector(dfcVec);

    StartDataFeedBuilder sdfBuilder(builder);
    sdfBuilder.add_data_feeds(dataFeedsVector);
    auto startDataFeed = sdfBuilder.Finish();

    // 6. Build DataFeedMessageHeader
    DataFeedMessageHeaderBuilder dfmhBuilder(builder);
    dfmhBuilder.add_message_type(DataFeedMessage::StartDataFeed);
    dfmhBuilder.add_message(startDataFeed.Union());
    auto dataFeedMsgHeader = dfmhBuilder.Finish();

    // 7. Build MessageBundle
    std::vector<flatbuffers::Offset<DataFeedMessageHeader>> dfmhVec;
    dfmhVec.push_back(dataFeedMsgHeader);
    auto dataFeedMsgsVector = builder.CreateVector(dfmhVec);

    MessageBundleBuilder mbBuilder(builder);
    mbBuilder.add_data_feed_msgs(dataFeedMsgsVector);
    auto messageBundle = mbBuilder.Finish();

    builder.Finish(messageBundle);

    // Send binary FlatBuffer over WebSocket
    DWORD bytesSent = 0;
    WinHttpWebSocketSend(m_hWebSocket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, 
                         (PVOID)builder.GetBufferPointer(), builder.GetSize());
    
    printf("[SolarXR Client] DataFeed subscription sent (%d bytes).\n", builder.GetSize());
}
