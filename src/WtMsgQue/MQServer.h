/* WonderTrader, Wesley Liu. NNG transport migration, 2026-10-15. */
#pragma once
#include <atomic>
#include <queue>
#include <nng/nng.h>
#include "../Includes/WTSMarcos.h"
#include "../Share/StdUtils.hpp"
NS_WTP_BEGIN
class MQManager;
class MQServer
{
public:
    MQServer(MQManager* mgr);
    ~MQServer();
    uint32_t id() const { return _id; }
    bool init(const char* url, bool confirm = false);
    void publish(const char* topic, const void* data, uint32_t dataLen);
private:
    static void pipe_event(nng_pipe, nng_pipe_ev, void*);
    void cast();
    struct PubData {
        std::string topic, data;
        PubData(const char* t, const void* p, uint32_t n): topic(t), data(p ? std::string(static_cast<const char*>(p), n) : "") {}
    };
    std::string _url;
    nng_socket _sock = NNG_SOCKET_INITIALIZER;
    bool _ready = false;
    MQManager* _mgr;
    uint32_t _id;
    bool _confirm = false;
    std::atomic<bool> m_bTerminated{false};
    unsigned _connections = 0; // Protected by m_mtxCast; NNG pipe callbacks wake confirmed publishers.
    StdThreadPtr m_thrdCast;
    StdCondVariable m_condCast;
    StdUniqueMutex m_mtxCast;
    std::queue<PubData> m_dataQue;
};
NS_WTP_END
