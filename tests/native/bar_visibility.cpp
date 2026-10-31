// Synthetic cached bars only; exercise the production read and callback paths.
#include "WtBtCore/HisDataReplayer.h"
#include "Includes/WTSDataDef.hpp"
#include "WTSTools/WTSLogger.h"
#include <cassert>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>

struct NativeBarVisibilityTest {
    static void clock(HisDataReplayer& replay, uint32_t date, uint32_t time,
                      uint32_t trading_date = 20261027) {
        replay._cur_date = date;
        replay._cur_time = time;
        replay._cur_tdate = trading_date;
    }
    static auto& seed(HisDataReplayer& replay, const char* code, const char* period,
                      uint32_t times, std::vector<WTSBarStruct> bars) {
        const auto key = std::string(code) + "#" + period + "#" + std::to_string(times);
        auto list = std::make_shared<HisDataReplayer::BarsList>();
        list->_code = code;
        list->_period = period[0] == 'd' ? KP_DAY : (times % 5 ? KP_Minute1 : KP_Minute5);
        list->_times = times % 5 ? times : times / 5;
        list->_bars = std::move(bars);
        list->_count = list->_bars.size();
        replay._bars_cache[key] = list;
        return *list;
    }
    static void load(HisDataReplayer& replay) {
        assert(replay._bd_mgr.loadSessions("sessions.json"));
        assert(replay._bd_mgr.loadCommodities("commodities.json"));
    }
    static void minute_end(HisDataReplayer& replay, uint32_t time) {
        replay.onMinuteEnd(20261027, time);
    }
    static void seed_hft(HisDataReplayer& replay) {
        const auto seed = [](auto& list) {
            list._code = "TEST.rb.2701";
            list._date = 20261027;
            list._items.resize(3);
            list._count = 3;
            for (std::size_t i = 0; i < 3; ++i) {
                list._items[i].action_date = 20261027;
                list._items[i].action_time = (915 + i * 15) * 100000;
            }
        };
        seed(replay._ticks_cache["TEST.rb.2701"]);
        seed(replay._orddtl_cache["TEST.rb.2701"]);
        seed(replay._ordque_cache["TEST.rb.2701"]);
        seed(replay._trans_cache["TEST.rb.2701"]);
    }
    static void hft_cursor(HisDataReplayer& replay, std::size_t cursor) {
        replay._ticks_cache["TEST.rb.2701"]._cursor = cursor;
        replay._orddtl_cache["TEST.rb.2701"]._cursor = cursor;
        replay._ordque_cache["TEST.rb.2701"]._cursor = cursor;
        replay._trans_cache["TEST.rb.2701"]._cursor = cursor;
    }
    static bool mixed(HisDataReplayer& replay, uint64_t begin, uint64_t end) {
        return replay.replayHftDatas(begin, end);
    }
    static void prepare_mixed(HisDataReplayer& replay) {
        const auto prepare = [](auto& list) {
            list._cursor = 1;
            for (std::size_t i = 0; i < 3; ++i) {
                list._items[i].action_time = (i < 2 ? 915 : 930) * 100000;
                list._items[i].price = 101 + i;
            }
        };
        prepare(replay._ticks_cache["TEST.rb.2701"]);
        prepare(replay._orddtl_cache["TEST.rb.2701"]);
        prepare(replay._ordque_cache["TEST.rb.2701"]);
        prepare(replay._trans_cache["TEST.rb.2701"]);
        replay._tick_sub_map["TEST.rb.2701"];
        replay._orddtl_sub_map["TEST.rb.2701"];
        replay._ordque_sub_map["TEST.rb.2701"];
        replay._trans_sub_map["TEST.rb.2701"];
    }
};

static WTSBarStruct bar(uint32_t date, uint32_t time, double close) {
    WTSBarStruct result{};
    result.date = date;
    result.time = (date - 19900000) * 10000 + time;
    result.open = close - 1;
    result.high = close + 1;
    result.low = close - 2;
    result.close = close;
    result.vol = 10;
    return result;
}

static std::vector<double> read(HisDataReplayer& replay, const char* period = "m",
                                uint32_t times = 1, const char* code = "TEST.rb.2701",
                                uint32_t count = 100) {
    auto slice = replay.get_kline_slice(code, period, count, times);
    std::vector<double> values;
    if (slice) {
        for (uint32_t i = 0; i < slice->size(); ++i)
            values.push_back(slice->at(i)->close);
        slice->release();
    }
    return values;
}

struct Sink : IDataSink {
    int bars = 0;
    void handle_tick(const char*, WTSTickData*, uint32_t) override {}
    void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override { ++bars; }
    void handle_schedule(uint32_t, uint32_t) override {}
    void handle_init() override {}
    void handle_session_begin(uint32_t) override {}
    void handle_session_end(uint32_t) override {}
};

template<typename Getter>
static void check_hft(Getter getter) {
    HisDataReplayer replay;
    NativeBarVisibilityTest::seed_hft(replay);
    const auto size = [&](uint64_t cutoff = 0, uint32_t count = 100) {
        auto slice = (replay.*getter)("TEST.rb.2701", count, cutoff);
        if (!slice) return 0u;
        const uint32_t result = slice->size();
        slice->release();
        return result;
    };
    NativeBarVisibilityTest::clock(replay, 20261027, 900);
    assert(size() == 0); // No first future event, even with a future requested cutoff.
    assert(size(202610281500) == 0);
    NativeBarVisibilityTest::clock(replay, 20261027, 930);
    assert(size(202610281500) == 2);
    assert(size(202610270915) == 1);
    assert(size(0, 0) == 0);
    NativeBarVisibilityTest::hft_cursor(replay, 1);
    assert(size() == 0); // Next event is still pending delivery.
    NativeBarVisibilityTest::hft_cursor(replay, 2);
    assert(size() == 1);
    NativeBarVisibilityTest::hft_cursor(replay, 0);
    assert(size() == 0);
    NativeBarVisibilityTest::hft_cursor(replay, UINT_MAX);
    NativeBarVisibilityTest::clock(replay, 20261027, 1500);
    assert(size() == 3); // Iterator at end is a valid historical slice.
}

struct MixedSink : Sink {
    HisDataReplayer& replay;
    std::array<uint32_t, 4> seen{};
    explicit MixedSink(HisDataReplayer& source) : replay(source) {}
    template<typename Getter>
    void check(Getter getter, uint32_t expected) {
        auto slice = (replay.*getter)("TEST.rb.2701", 100, 0);
        assert((slice ? slice->size() : 0) == expected);
        if (slice) {
            for (uint32_t i = 0; i < expected; ++i)
                assert(slice->at(i)->price == 101 + i);
            slice->release();
        }
    }
    void check_all() {
        check(&HisDataReplayer::get_tick_slice, seen[0]);
        check(&HisDataReplayer::get_order_detail_slice, seen[1]);
        check(&HisDataReplayer::get_order_queue_slice, seen[2]);
        check(&HisDataReplayer::get_transaction_slice, seen[3]);
        auto tick = replay.get_last_tick("TEST.rb.2701");
        assert((tick != nullptr) == (seen[0] != 0));
        if (tick) {
            assert(tick->price() == 100 + seen[0]);
            tick->release();
        }
    }
    void handle_tick(const char*, WTSTickData*, uint32_t) override { ++seen[0]; check_all(); }
    void handle_order_detail(const char*, WTSOrdDtlData*) override { ++seen[1]; check_all(); }
    void handle_order_queue(const char*, WTSOrdQueData*) override { ++seen[2]; check_all(); }
    void handle_transaction(const char*, WTSTransData*) override { ++seen[3]; check_all(); }
    void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override {
        ++bars;
        assert(read(replay).size() == static_cast<std::size_t>(bars));
        check_all();
    }
};

static void check_mixed() {
    HisDataReplayer replay;
    NativeBarVisibilityTest::load(replay);
    NativeBarVisibilityTest::seed_hft(replay);
    NativeBarVisibilityTest::prepare_mixed(replay);
    NativeBarVisibilityTest::clock(replay, 20261027, 900);
    NativeBarVisibilityTest::seed(replay, "TEST.rb.2701", "m", 1,
        {bar(20261027, 916, 501), bar(20261027, 931, 502)});
    assert(read(replay).empty());
    MixedSink sink(replay);
    replay.register_sink(&sink, "synthetic-mixed");
    assert(NativeBarVisibilityTest::mixed(replay, 202610270900, 202610270916));
    assert((sink.seen == std::array<uint32_t, 4>{2, 2, 2, 2}));
    NativeBarVisibilityTest::clock(replay, 20261027, 916);
    NativeBarVisibilityTest::minute_end(replay, 916);
    assert(sink.bars == 1);
    assert(NativeBarVisibilityTest::mixed(replay, 202610270916, 202610270931));
    assert((sink.seen == std::array<uint32_t, 4>{3, 3, 3, 3}));
    NativeBarVisibilityTest::clock(replay, 20261027, 931);
    NativeBarVisibilityTest::minute_end(replay, 931);
    assert(sink.bars == 2);
    std::cout << "MIXED_VISIBILITY_PASS events_per_type=3 duplicate_timestamp=isolated last_event=delivered bars=2\n";
}

int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    std::filesystem::create_directories(argv[1]);
    std::filesystem::current_path(argv[1]);
    WTSLogger::init(R"({"root":{"level":"error","sinks":[{"type":"ostream_sink","pattern":"%v"}]}})", false);
    std::ofstream("sessions.json") << R"({
      "NIGHT":{"name":"night","offset":180,"sections":[{"from":2100,"to":2300},{"from":900,"to":1500}]},
      "LATE":{"name":"late","offset":-300,"sections":[{"from":1700,"to":1600}]},
      "MIDNIGHT":{"name":"midnight","offset":-60,"sections":[{"from":100,"to":0}]}
    })";
    std::ofstream("commodities.json") << R"({"TEST":{
      "rb":{"name":"synthetic","session":"NIGHT","holiday":"CHINA","pricetick":1,"volscale":1},
      "late":{"name":"synthetic","session":"LATE","holiday":"CHINA","pricetick":1,"volscale":1},
      "mid":{"name":"synthetic","session":"MIDNIGHT","holiday":"CHINA","pricetick":1,"volscale":1}
    }})";
    if (argc == 3) {
        assert(std::string(argv[2]) == "mixed");
        check_mixed();
        return 0;
    }

    for (uint32_t times : {1u, 5u, 15u}) {
        HisDataReplayer replay;
        NativeBarVisibilityTest::load(replay);
        auto& list = NativeBarVisibilityTest::seed(replay, "TEST.rb.2701", "m", times,
            {bar(20261027, 915, 101), bar(20261027, 930, 102)});
        NativeBarVisibilityTest::clock(replay, 20261027, 900);
        assert(read(replay, "m", times).empty()); // First bar is in the future.
        assert(list._cursor == 0);
        assert(read(replay, "m", times).empty()); // Safe repeat at cursor zero.
        NativeBarVisibilityTest::clock(replay, 20261027, 915);
        assert(read(replay, "m", times) == std::vector<double>{101});
        assert(list._cursor == 0); // Reading must not consume replay callbacks.
        assert(read(replay, "m", times) == std::vector<double>{101});
        list._bars[1] = bar(20261027, 1200, 999999);
        list._bars.push_back(bar(20261028, 900, -12345));
        list._count = list._bars.size();
        assert(read(replay, "m", times) == std::vector<double>{101});
        NativeBarVisibilityTest::clock(replay, 20261029, 900);
        assert(read(replay, "m", times).size() == 3); // upper_bound == end keeps history.
        assert(read(replay, "m", times, "TEST.rb.2701", 1) == std::vector<double>{-12345});
        NativeBarVisibilityTest::clock(replay, 20261027, 900);
        assert(read(replay, "m", times).empty()); // Requery does not expose stale progress.
        NativeBarVisibilityTest::clock(replay, 20261027, 915);
        Sink sink;
        replay.register_sink(&sink, "synthetic");
        assert(read(replay, "m", times).size() == 1);
        NativeBarVisibilityTest::minute_end(replay, 915);
        assert(sink.bars == 1 && list._cursor == 1);
    }
    {
        HisDataReplayer replay;
        NativeBarVisibilityTest::load(replay);
        auto& list = NativeBarVisibilityTest::seed(replay, "TEST.rb.2701", "d", 1,
            {bar(20261027, 0, 101), bar(20261028, 0, 102)});
        NativeBarVisibilityTest::clock(replay, 20261026, 2100);
        assert(read(replay, "d").empty()); // Night session belongs to tomorrow.
        NativeBarVisibilityTest::clock(replay, 20261027, 1459);
        assert(read(replay, "d").empty());
        assert(list._cursor == 0);
        NativeBarVisibilityTest::clock(replay, 20261027, 1500);
        assert(read(replay, "d") == std::vector<double>{101});
        list._bars[1].close = 999999;
        assert(read(replay, "d") == std::vector<double>{101});
    }
    for (const auto code : {"TEST.late.2701", "TEST.mid.2701"}) {
        HisDataReplayer replay;
        NativeBarVisibilityTest::load(replay);
        NativeBarVisibilityTest::seed(replay, code, "d", 1, {bar(20261027, 0, 101)});
        const uint32_t close = std::string(code).find("late") != std::string::npos ? 1600 : 0;
        NativeBarVisibilityTest::clock(replay, 20261027, 2359);
        assert(read(replay, "d", 1, code).empty()); // Same trading date is still incomplete.
        if (close) {
            NativeBarVisibilityTest::clock(replay, 20261028, 1559);
            assert(read(replay, "d", 1, code).empty());
        }
        NativeBarVisibilityTest::clock(replay, 20261028, close);
        assert(read(replay, "d", 1, code) == std::vector<double>{101});
    }
    {
        HisDataReplayer replay;
        NativeBarVisibilityTest::load(replay);
        NativeBarVisibilityTest::seed(replay, "TEST.rb.2701", "m", 1, {});
        NativeBarVisibilityTest::clock(replay, 20261027, 900);
        assert(read(replay).empty());
        assert(read(replay).empty());
    }
    check_hft(&HisDataReplayer::get_tick_slice);
    check_hft(&HisDataReplayer::get_order_detail_slice);
    check_hft(&HisDataReplayer::get_order_queue_slice);
    check_hft(&HisDataReplayer::get_transaction_slice);
    {
        HisDataReplayer replay;
        NativeBarVisibilityTest::seed_hft(replay);
        NativeBarVisibilityTest::clock(replay, 20261027, 900);
        assert(replay.get_last_tick("TEST.rb.2701") == nullptr);
        NativeBarVisibilityTest::clock(replay, 20261027, 930);
        auto tick = replay.get_last_tick("TEST.rb.2701");
        assert(tick && tick->actiontime() == 93000000);
        tick->release();
    }
    std::cout << "BAR_VISIBILITY_PASS minute=1,5,15 daily=night,overnight,midnight callback=preserved future_tail=stable hft=tick,detail,queue,transaction,last_tick\n";
    check_mixed();
}
