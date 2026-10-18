/* WonderTrader, Wesley Liu. NNG transport migration, 2026-10-15. */
#include "MQServer.h"
#include "MQManager.h"
#include <nng/protocol/pubsub0/pub.h>
#include <algorithm>
#include <cstring>
USING_NS_WTP;
namespace { std::atomic<uint32_t> server_id{1001}; }
MQServer::MQServer(MQManager* mgr): _mgr(mgr), _id(server_id.fetch_add(1)) {}
MQServer::~MQServer()
{
    m_bTerminated = true;
    m_condCast.notify_all();
    if (_ready) nng_close(_sock); // Also interrupts an outstanding send.
    if (m_thrdCast && m_thrdCast->joinable()) m_thrdCast->join();
}
void MQServer::pipe_event(nng_pipe, nng_pipe_ev ev, void* arg)
{
    auto* self = static_cast<MQServer*>(arg);
    {
        StdUniqueLock lock(self->m_mtxCast);
        if (ev == NNG_PIPE_EV_ADD_POST) ++self->_connections;
        else if (ev == NNG_PIPE_EV_REM_POST && self->_connections) --self->_connections;
    }
    self->m_condCast.notify_all();
}
bool MQServer::init(const char* url, bool confirm)
{
    if (_ready) return true;
    if (!url) return false;
    int rv = nng_pub0_open(&_sock);
    if (rv) { _mgr->log_server(_id, nng_strerror(rv)); return false; }
    // NNG buffers count messages, whereas nanomsg counted bytes.
    if ((rv = nng_socket_set_int(_sock, NNG_OPT_SENDBUF, 1024)) ||
        (rv = nng_socket_set_ms(_sock, NNG_OPT_SENDTIMEO, 1000)) ||
        (rv = nng_pipe_notify(_sock, NNG_PIPE_EV_ADD_POST, pipe_event, this)) ||
        (rv = nng_pipe_notify(_sock, NNG_PIPE_EV_REM_POST, pipe_event, this)) ||
        (rv = nng_listen(_sock, url, nullptr, 0))) {
        _mgr->log_server(_id, nng_strerror(rv));
        nng_close(_sock);
        return false;
    }
    _url = url;
    _confirm = confirm;
    _ready = true;
    m_thrdCast.reset(new StdThread([this] { cast(); }));
    _mgr->log_server(_id, "NNG publisher ready");
    return true;
}
void MQServer::publish(const char* topic, const void* data, uint32_t dataLen)
{
    if (!_ready || !topic || !data || !dataLen || m_bTerminated) return;
    {
        StdUniqueLock lock(m_mtxCast);
        m_dataQue.emplace(topic, data, dataLen);
    }
    m_condCast.notify_all();
}
void MQServer::cast()
{
    while (!m_bTerminated) {
        std::queue<PubData> pending;
        {
            StdUniqueLock lock(m_mtxCast);
            const bool signalled = m_condCast.wait_for(lock, std::chrono::seconds(60), [this] {
                return m_bTerminated || ((!_confirm || _connections) && !m_dataQue.empty());
            });
            if (m_bTerminated) break;
            if (_confirm && !_connections) continue;
            if (!signalled && m_dataQue.empty()) m_dataQue.emplace("HEARTBEAT", nullptr, 0);
            pending.swap(m_dataQue);
        }
        while (!pending.empty() && !m_bTerminated) {
            const auto& pub = pending.front();
            std::string frame(sizeof(MQPacket) + pub.data.size(), '\0');
            // Keep the established 32-byte topic + uint32 length + payload wire format.
            std::memcpy(&frame[0], pub.topic.data(), std::min(pub.topic.size(), size_t(32)));
            const uint32_t length = static_cast<uint32_t>(pub.data.size());
            std::memcpy(&frame[32], &length, sizeof(length));
            if (length) std::memcpy(&frame[sizeof(MQPacket)], pub.data.data(), length);
            // NNG sends one complete message; a successful call returns zero, not a byte count.
            const int rv = nng_send(_sock, &frame[0], frame.size(), 0);
            if (rv && !m_bTerminated) _mgr->log_server(_id, nng_strerror(rv));
            pending.pop();
        }
    }
}
