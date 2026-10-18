/* WonderTrader, Wesley Liu. NNG transport migration, 2026-10-15. */
#pragma once
#include <atomic>
#include <set>
#include <nng/nng.h>
#include "PorterDefs.h"
#include "../Share/StdUtils.hpp"
NS_WTP_BEGIN
class MQManager;
class MQClient
{
public:
    MQClient(MQManager* mgr);
    ~MQClient();
    uint32_t id() const { return _id; }
    bool init(const char* url, FuncMQCallback cb);
    void start();
    void sub_topic(const char* topic);
private:
    void receive();
    void extract_message(const char* data, size_t size);
    bool is_allowed(const char* topic);
    nng_socket _sock = NNG_SOCKET_INITIALIZER;
    bool m_bReady = false;
    MQManager* _mgr;
    uint32_t _id;
    StdThreadPtr m_thrdRecv;
    std::atomic<bool> m_bTerminated{false};
    FuncMQCallback _cb_message = nullptr;
    std::mutex _topicMutex;
    std::set<std::string> _topics;
};
NS_WTP_END
