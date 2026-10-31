// Synthetic live-mode Tick calls only. No parser, broker, socket, or timer thread runs.
#include "WtCore/WtCtaTicker.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtHelper.h"
#include "WtCore/WtDtMgr.h"
#include "WtCore/TraderAdapter.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSUtils/WTSCfgLoader.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "WTSTools/WTSLogger.h"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <vector>
using namespace wtp;

namespace wtp {
struct NativeLiveTickerTest {
    static void seed(WtCtaRtTicker& ticker, WTSSessionInfo* session, uint32_t date,
                     uint32_t time, bool ended = false) {
        ticker._s_info = session; ticker._store = nullptr;
        ticker._date = date; ticker._time = time * 100000;
        ticker._cur_pos = ended ? session->getTradingMins() : session->timeToMinutes(time);
        ticker._last_emit_pos = ticker._cur_pos;
        // Non-null but non-running thread selects the real live on_tick branch.
        ticker._thrd = std::make_shared<StdThread>();
        assert(!ticker._replay);
    }
    static uint32_t cursor(const WtCtaRtTicker& ticker) { return ticker._cur_pos; }
};
}

struct Engine : WtCtaEngine, IEngineEvtListener {
    std::vector<uint32_t> begins, ends;
    std::vector<std::pair<uint32_t,uint32_t>> schedules;
    unsigned ticks = 0;
    Engine() { regEventListener(this); }
    void on_tick(const char*, WTSTickData*) override { ++ticks; }
    void on_session_begin() override { begins.push_back(getTradingDate()); WtCtaEngine::on_session_begin(); }
    void on_session_end() override { ends.push_back(getTradingDate()); WtCtaEngine::on_session_end(); }
    void on_schedule_event(uint32_t date,uint32_t time) override { schedules.emplace_back(date,time); }
};

struct Fixture {
    WTSBaseDataMgr data;
    WtDtMgr history;
    TraderAdapterMgr adapters;
    Engine engine;
    WtCtaRtTicker ticker{&engine};
    WTSSessionInfo* session = WTSSessionInfo::create("TRADING","Synthetic night/day",300);
    WTSContractInfo* contract = WTSContractInfo::create("rb2701","Synthetic","SHFE","rb");
    Fixture(uint32_t date, uint32_t time, uint32_t trading_date, bool ended = false) {
        auto* config = WTSCfgLoader::load_from_content("{}");
        engine.init(config,&data,&history,nullptr,nullptr); config->release();
        engine.set_adapter_mgr(&adapters);
        session->addTradingSection(2100,230);
        session->addTradingSection(900,1015);
        session->addTradingSection(1030,1130);
        session->addTradingSection(1330,1515);
        engine.set_date_time(date,time); engine.set_trading_date(trading_date);
        NativeLiveTickerTest::seed(ticker,session,date,time,ended);
    }
    ~Fixture() { ticker.stop(); contract->release(); session->release(); }
    void tick(uint32_t date,uint32_t time,uint32_t trading_date) {
        auto* quote = WTSTickData::create("rb2701");
        auto& raw = quote->getTickStruct(); raw.action_date=date; raw.action_time=time*100000+10000;
        raw.trading_date=trading_date; raw.price=3300;
        quote->setContractInfo(contract);
        ticker.on_tick(quote); quote->release();
    }
};

int main(int argc,char** argv) {
    assert(argc==2 || argc==3); std::filesystem::create_directories(argv[1]); std::filesystem::current_path(argv[1]);
    WtHelper::setGenerateDir("generated");
    WTSLogger::init(R"({"root":{"level":"error","sinks":[{"type":"ostream_sink","pattern":"%v"}]}})",false);
    {
        Fixture value(20261027,1505,20261027);
        const auto daytime = NativeLiveTickerTest::cursor(value.ticker);
        value.tick(20261027,2100,20261028);
        if (argc==3) {
            value.tick(20261027,2101,20261028);
            assert(value.engine.getTradingDate()==20261027 && value.engine.schedules.empty());
            assert(value.engine.begins.empty() && value.engine.ends.empty());
            assert(NativeLiveTickerTest::cursor(value.ticker)==daytime);
            std::cout<<"LIVE_TICKER_BASELINE_REPRODUCED old_td=retained night_minutes=not_scheduled\n";
            return 0;
        }
        assert(value.engine.getTradingDate()==20261028 && value.engine.ticks==1);
        assert(NativeLiveTickerTest::cursor(value.ticker)<daytime);
        assert(value.engine.ends==std::vector<uint32_t>{20261027});
        assert(value.engine.begins==std::vector<uint32_t>{20261028});
        value.tick(20261027,2101,20261028);
        assert((value.engine.schedules==std::vector<std::pair<uint32_t,uint32_t>>{{20261027,2101}}));
        value.tick(20261027,2102,20261027); // Even a newer action timestamp cannot resurrect the old TD.
        assert(value.engine.getTradingDate()==20261028 && value.engine.ticks==2 && value.engine.schedules.size()==1);
    }
    {
        Fixture value(20261027,1515,20261027,true);
        value.engine.on_session_end();
        value.tick(20261027,2100,20261028);
        assert(value.engine.ends==std::vector<uint32_t>{20261027});
        assert(value.engine.begins==std::vector<uint32_t>{20261028});
    }
    {
        Fixture value(20261028,1130,20261028);
        value.tick(20261028,1330,20261028); value.tick(20261028,1331,20261028);
        assert(value.engine.ends.empty() && value.engine.begins.empty());
        assert(value.engine.getTradingDate()==20261028 && !value.engine.schedules.empty());
    }
    {
        Fixture value(20261027,2359,20261028);
        value.tick(20261028,0,20261028); value.tick(20261028,1,20261028);
        assert(value.engine.ends.empty() && value.engine.begins.empty());
        assert(value.engine.getTradingDate()==20261028 && value.engine.get_date()==20261028);
        assert(!value.engine.schedules.empty());
    }
    std::cout<<"LIVE_TICKER_PASS new_td=scheduled old_td=dropped lunch=continuous midnight=continuous settlement=once first_tick=delivered\n";
}
