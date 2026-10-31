#pragma once
#include <chrono>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

NS_WTP_BEGIN
struct ExecutionQuote
{
    double bid = 0, ask = 0, last = 0;
    uint64_t updated_at = 0;
};

struct ExecutionStamp
{
    uint64_t wall, mono;
    static ExecutionStamp now()
    {
        using namespace std::chrono;
        return {uint64_t(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count()),
                uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count())};
    }
};

// Only order events are persisted. Quotes are sampled before the send call.
class ExecutionRecorder
{
public:
    using QuoteReader = std::function<ExecutionQuote(const char*)>;
    void open(const std::string& path, const std::string& channel)
    {
        _channel = channel;
        _stream.open(path, std::ios::app);
    }
    void setQuoteReader(const QuoteReader& reader) { _quote_reader = reader; }
    ExecutionQuote quote(const char* code) const { return _quote_reader ? _quote_reader(code) : ExecutionQuote{}; }
    void record(const char* event, uint32_t date, uint32_t localid,
                const char* entrust, const char* order, const char* trade,
                const char* code, bool isBuy, double qty, double price,
                ExecutionStamp stamp, ExecutionQuote quote = {},
                double filled = -1, double left = -1, bool canceled = false)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_stream.is_open()) return;
        namespace rj = rapidjson;
        rj::Document row(rj::kObjectType);
        auto& a = row.GetAllocator();
        auto text = [&](const char* key, const char* value) {
            row.AddMember(rj::Value(key, a), rj::Value(value ? value : "", a), a);
        };
        row.AddMember("schema", 1, a);
        text("event", event); text("channel", _channel.c_str()); text("clock_id", clockId().c_str());
        text("entrust_id", entrust); text("orderid", order); text("tradeid", trade);
        text("code", code); text("side", isBuy ? "buy" : "sell");
        row.AddMember("trading_date", date, a); row.AddMember("localid", localid, a);
        row.AddMember("qty", qty, a); row.AddMember("price", price, a);
        row.AddMember("wall_time_ns", stamp.wall, a); row.AddMember("mono_time_ns", stamp.mono, a);
        if (filled >= 0) row.AddMember("filled_qty", filled, a);
        if (left >= 0) row.AddMember("left_qty", left, a);
        row.AddMember("canceled", canceled, a);
        if (quote.updated_at)
        {
            rj::Value snapshot(rj::kObjectType);
            snapshot.AddMember("bid", quote.bid, a); snapshot.AddMember("ask", quote.ask, a);
            snapshot.AddMember("last", quote.last, a); snapshot.AddMember("updated_at", quote.updated_at, a);
            row.AddMember("quote", snapshot, a); text("benchmark_kind", "opposite_touch");
        }
        rj::StringBuffer buffer;
        rj::Writer<rj::StringBuffer> writer(buffer);
        row.Accept(writer);
        _stream.write(buffer.GetString(), buffer.GetSize());
        _stream.put('\n'); _stream.flush();
    }
private:
    static const std::string& clockId()
    {
#ifdef _WIN32
        const auto pid = _getpid();
#else
        const auto pid = getpid();
#endif
        static const std::string value = std::to_string(pid) + "-" + std::to_string(ExecutionStamp::now().wall);
        return value;
    }
    std::mutex _mutex;
    std::ofstream _stream;
    std::string _channel;
    QuoteReader _quote_reader;
};
NS_WTP_END
