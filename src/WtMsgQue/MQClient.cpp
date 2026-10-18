/* WonderTrader, Wesley Liu. NNG transport migration, 2026-10-15. */
#include "MQClient.h"
#include "MQManager.h"
#include <nng/protocol/pubsub0/sub.h>
#include <chrono>
#include <cstring>
USING_NS_WTP;
namespace { std::atomic<uint32_t> client_id{5001}; }
MQClient::MQClient(MQManager* mgr): _mgr(mgr), _id(client_id.fetch_add(1)) {}
MQClient::~MQClient()
{
    m_bTerminated = true;
    if (m_bReady) nng_close(_sock); // Wakes the receiver before joining.
    if (m_thrdRecv && m_thrdRecv->joinable()) m_thrdRecv->join();
}
bool MQClient::init(const char* url, FuncMQCallback cb)
{
    if (m_bReady) return true;
    if (!url || !cb) return false;
    int rv = nng_sub0_open(&_sock);
    if (rv) { _mgr->log_client(_id, nng_strerror(rv)); return false; }
    if ((rv = nng_socket_set(_sock, NNG_OPT_SUB_SUBSCRIBE, "", 0)) ||
        (rv = nng_socket_set_int(_sock, NNG_OPT_RECVBUF, 1024)) ||
        (rv = nng_socket_set_size(_sock, NNG_OPT_RECVMAXSZ, 64 * 1024 * 1024)) ||
        (rv = nng_socket_set_ms(_sock, NNG_OPT_RECVTIMEO, 100)) ||
        (rv = nng_dial(_sock, url, nullptr, NNG_FLAG_NONBLOCK))) {
        _mgr->log_client(_id, nng_strerror(rv));
        nng_close(_sock);
        return false;
    }
    _cb_message = cb;
    m_bReady = true;
    _mgr->log_client(_id, "NNG subscriber ready");
    return true;
}
void MQClient::sub_topic(const char* topic)
{
    if (topic) { std::lock_guard<std::mutex> lock(_topicMutex); _topics.insert(topic); }
}
bool MQClient::is_allowed(const char* topic)
{
    std::lock_guard<std::mutex> lock(_topicMutex);
    return _topics.empty() || _topics.count(topic);
}
void MQClient::start()
{
    if (!m_bReady || m_bTerminated || m_thrdRecv) return;
    m_thrdRecv.reset(new StdThread([this] { receive(); }));
}
void MQClient::receive()
{
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    bool needs_check = false;
    while (!m_bTerminated) {
        void* data = nullptr;
        size_t size = 0;
        const int rv = nng_recv(_sock, &data, &size, NNG_FLAG_ALLOC);
        if (!rv) {
            last = Clock::now();
            needs_check = true;
            // Existing ctypes callbacks expose c_char_p, so retain a trailing NUL
            // beyond the length-delimited frame even though NNG does not add one.
            const std::string frame(static_cast<const char*>(data), size);
            extract_message(frame.c_str(), frame.size());
            nng_free(data, size);
        } else if (rv == NNG_ECLOSED || m_bTerminated) {
            break;
        } else if (rv != NNG_ETIMEDOUT && rv != NNG_EAGAIN) {
            _mgr->log_client(_id, nng_strerror(rv));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (needs_check && Clock::now() - last >= std::chrono::seconds(60)) {
            _cb_message(_id, "TIMEOUT", "", 0);
            needs_check = false;
        }
    }
}
void MQClient::extract_message(const char* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        if (size - offset < sizeof(MQPacket)) {
            _mgr->log_client(_id, "Dropped truncated MQ header"); return;
        }
        uint32_t length;
        std::memcpy(&length, data + offset + 32, sizeof(length));
        if (length > size - offset - sizeof(MQPacket)) {
            _mgr->log_client(_id, "Dropped truncated MQ payload"); return;
        }
        char topic[33] = {};
        std::memcpy(topic, data + offset, 32);
        if (is_allowed(topic)) _cb_message(_id, topic, data + offset + sizeof(MQPacket), length);
        offset += sizeof(MQPacket) + length;
    }
}
