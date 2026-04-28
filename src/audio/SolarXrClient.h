#pragma once

#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <thread>
#include <atomic>
#include <string>

class OscProxy;

class SolarXrClient {
public:
    SolarXrClient(OscProxy* proxy = nullptr);
    ~SolarXrClient();

    bool start();
    void stop();

private:
    void clientLoop();
    void connectAndReceive();
    void sendDataFeedSubscription();

    std::atomic<bool> m_running;
    std::thread m_thread;
    
    HINTERNET m_hSession;
    HINTERNET m_hConnect;
    HINTERNET m_hRequest;
    HINTERNET m_hWebSocket;
    
    OscProxy* m_proxy;
};
