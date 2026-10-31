// Synthetic broker callbacks and executor targets; no CTP or socket connections.
#include "WtCore/TraderAdapter.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/CtaStraBaseCtx.h"
#include "WtCore/WtHelper.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "WTSUtils/WTSCfgLoader.h"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSVariant.hpp"
#include <rapidjson/document.h>
#include <cassert>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>
using namespace wtp;

namespace wtp {
struct NativePortfolioTestAccessor {
    static void target(WtLocalExecuter& executor, const char* code, double target) {
        std::lock_guard<std::recursive_mutex> lock(executor._reset_dispatch);
        executor._target_pos[code] = target; ++executor._portfolio_revision;
    }
    static void busy(TraderAdapter& adapter, std::promise<void>& held, std::future<void> release) {
        std::lock_guard<std::mutex> lock(adapter._portfolio_state._mutex);
        held.set_value(); release.wait();
    }
    static void event_busy(WtCtaEngine& engine, std::promise<void>& held, std::future<void> release) {
        std::lock_guard<std::recursive_mutex> lock(engine._reset_events);
        held.set_value(); release.wait();
    }
};
}

struct Broker : ITraderApi {
    int positions=0, orders=0, trades=0, accounts=0, inserts=0, cancels=0;
    std::string tag, entrust;
    bool init(WTSVariant*) override { return true; }
    int queryPositions() override { ++positions; return 0; }
    int queryOrders() override { ++orders; return 0; }
    int queryTrades() override { ++trades; return 0; }
    int queryAccount() override { ++accounts; return 0; }
    bool makeEntrustID(char* out, int) override { strcpy(out, "synthetic-entrust"); return true; }
    int orderInsert(WTSEntrust* value) override {
        ++inserts; tag=value->getUserTag(); entrust=value->getEntrustID(); return 0;
    }
    int orderAction(WTSEntrustAction*) override { ++cancels; return 0; }
    void release() override {}
};
struct Context : CtaStraBaseCtx {
    explicit Context(WtCtaEngine* engine):CtaStraBaseCtx(engine,"portfolio-test",0){}
    void on_bar_close(const char*,const char*,WTSBarStruct*) override {}
    void on_calculate(uint32_t,uint32_t) override {}
};
struct Fixture {
    WTSBaseDataMgr data;
    Broker* broker = new Broker; // initExt transfers API ownership to TraderAdapter.
    TraderAdapter adapter;
    TraderAdapterMgr manager;
    WtCtaEngine engine;
    std::shared_ptr<WtLocalExecuter> executor;
    WTSCommodityInfo* commodity=WTSCommodityInfo::create("rb","Synthetic","SHFE","DAY","CHINA");
    WTSContractInfo* contract=WTSContractInfo::create("rb2701","Synthetic","SHFE","rb");
    WTSArray* empty=WTSArray::create();
    Fixture() {
        commodity->setVolScale(10); commodity->setCategory(CC_Future); contract->setCommInfo(commodity);
        assert(adapter.initExt("synthetic", broker, &data, nullptr)); assert(adapter.run());
        TraderAdapterPtr owned(&adapter, [](TraderAdapter*){}); assert(manager.addAdapter("synthetic", owned));
        engine.set_adapter_mgr(&manager);
        executor=std::make_shared<WtLocalExecuter>(nullptr,"local",nullptr);
        auto* config=WTSCfgLoader::load_from_content(R"({"scale":1,"poolsize":0,"policy":{"default":{"name":"test"}}})");
        assert(executor->init(config)); config->release(); executor->setTrader(&adapter); engine.addExecuter(executor);
        engine.addContext(std::make_shared<Context>(&engine));
        adapter.onLoginResult(true,"ok",20261027); adapter.onRspPosition(empty); adapter.onRspOrders(empty);
        adapter.onRspTrades(empty); adapter.onRspAccount(empty);
        executor->on_channel_ready();
    }
    ~Fixture(){ adapter.release(); empty->release(); contract->release(); commodity->release(); }
    rapidjson::Document snapshot() {
        rapidjson::Document result; result.Parse(engine.query_portfolio_state("synthetic","portfolio-test"));
        assert(!result.HasParseError()); return result;
    }
    void target(double value) { NativePortfolioTestAccessor::target(*executor,"SHFE.rb.2701",value); }
    WTSOrderInfo* order(const char* id, bool alive, WTSOffsetType offset=WOT_OPEN) {
        auto* value=WTSOrderInfo::create(); value->setContractInfo(contract);
        value->setCode("rb2701"); value->setExchange("SHFE"); value->setOrderID(id);
        value->setEntrustID(broker->entrust.empty() ? id : broker->entrust.c_str());
        value->setUserTag(broker->tag.empty() ? "external-broker-order" : broker->tag.c_str());
        value->setDirection(WDT_LONG); value->setOffsetType(offset); value->setVolume(1);
        value->setVolLeft(alive ? 1 : 0); value->setPrice(3300);
        value->setOrderState(alive ? WOS_NotTraded_Queuing : WOS_Canceled); return value;
    }
    void fill(bool open, const char* id) {
        auto* value=WTSTradeInfo::create("rb2701","SHFE"); value->setContractInfo(contract);
        value->setDirection(WDT_LONG); value->setOffsetType(open ? WOT_OPEN : WOT_CLOSE);
        value->setVolume(1); value->setPrice(3300); value->setTradeID(id); value->setRefOrder(id);
        adapter.onPushTrade(value); value->release();
    }
};

int main(int argc,char** argv) {
    assert(argc==2); std::filesystem::create_directories(argv[1]); std::filesystem::current_path(argv[1]);
    WtHelper::setGenerateDir("generated");
    WTSLogger::init(R"({"root":{"level":"error","sinks":[{"type":"ostream_sink","pattern":"%v"}]}})",false);
    {
        Fixture value;
        auto before=value.snapshot(); assert(before["valid"].GetBool() && before["execution_idle"].GetBool());
        const auto queries=value.broker->positions+value.broker->orders+value.broker->trades+value.broker->accounts;
        auto repeated=value.snapshot(); assert(repeated["event_seq"]==before["event_seq"]);
        assert(queries==value.broker->positions+value.broker->orders+value.broker->trades+value.broker->accounts);
        value.target(1);
        auto id=value.adapter.openLong("SHFE.rb.2701",3300,1,0,value.contract); assert(id!=UINT_MAX);
        auto pending=value.snapshot(); assert(pending["pending_submissions"].GetUint64()==1);
        assert(pending["target_pending"].GetBool() && !pending["execution_idle"].GetBool());
        auto* order=value.order("open",true); value.adapter.onPushOrder(order);
        auto working=value.snapshot(); assert(working["working_orders"]["SHFE.rb.2701"].GetUint64()==1);
        assert(working["pending_submissions"].GetUint64()==0);
        value.target(0); order->setOrderState(WOS_Canceled); order->setVolLeft(0); value.adapter.onPushOrder(order); order->release();
        assert(value.snapshot()["execution_idle"].GetBool());
    }
    {
        Fixture value; value.fill(true,"actual-open"); value.target(0);
        auto* order=value.order("close",true,WOT_CLOSE); value.adapter.onPushOrder(order);
        auto pending=value.snapshot(); assert(!pending["execution_idle"].GetBool());
        assert(pending["actual_positions"][0]["long"].GetDouble()==1);
        value.fill(false,"actual-close"); assert(!value.snapshot()["execution_idle"].GetBool());
        order->setOrderState(WOS_AllTraded); order->setVolLeft(0); value.adapter.onPushOrder(order); order->release();
        assert(value.snapshot()["execution_idle"].GetBool());
    }
    {
        Fixture value;
        auto* c= WTSCommodityInfo::create("c","Synthetic","DCE","DAY","CHINA"); c->setCategory(CC_Future);
        auto* contract=WTSContractInfo::create("c2701","Synthetic","DCE","c"); contract->setCommInfo(c);
        auto* positions=WTSArray::create();
        for (auto direction : {WDT_LONG,WDT_SHORT}) {
            auto* p=WTSPositionItem::create("c2701","CNY","DCE"); p->setContractInfo(contract);
            p->setDirection(direction); p->setNewPosition(1); p->setAvailNewPos(1); positions->append(p,false);
        }
        auto* rb=WTSPositionItem::create("rb2701","CNY","SHFE"); rb->setContractInfo(value.contract);
        rb->setDirection(WDT_LONG); rb->setNewPosition(1); rb->setAvailNewPos(1); positions->append(rb,false);
        value.adapter.onLoginResult(true,"fresh",20261027); value.adapter.onRspPosition(positions);
        value.adapter.onRspOrders(value.empty); value.adapter.onRspTrades(value.empty); value.adapter.onRspAccount(value.empty);
        auto hedged=value.snapshot(); assert(hedged["actual_positions"][0]["code"]=="DCE.c.2701");
        assert(hedged["actual_positions"].Size()==2);
        assert(hedged["actual_positions"][0]["long"].GetDouble()==1 && hedged["actual_positions"][0]["short"].GetDouble()==1);
        assert(!hedged["execution_idle"].GetBool());
        auto* external=WTSOrderInfo::create(); external->setContractInfo(contract); external->setCode("c2701");
        external->setExchange("DCE"); external->setOrderID("outside-rb"); external->setEntrustID("outside-rb");
        external->setUserTag("external"); external->setDirection(WDT_LONG); external->setOffsetType(WOT_OPEN);
        external->setVolume(1); external->setVolLeft(1); external->setOrderState(WOS_NotTraded_Queuing);
        value.adapter.onPushOrder(external); external->release();
        assert(value.snapshot()["working_orders"]["DCE.c.2701"].GetUint64()==1);
        positions->release(); contract->release(); c->release();
    }
    {
        Fixture value;
        std::promise<void> held, release; auto available=held.get_future();
        std::thread busy([&](){NativePortfolioTestAccessor::busy(value.adapter,held,release.get_future());});
        available.wait(); assert(!value.snapshot()["valid"].GetBool()); release.set_value(); busy.join();
        std::promise<void> event_held, event_release; auto event_available=event_held.get_future();
        std::thread event_busy([&](){NativePortfolioTestAccessor::event_busy(value.engine,event_held,event_release.get_future());});
        event_available.wait(); assert(!value.snapshot()["valid"].GetBool()); event_release.set_value(); event_busy.join();
        assert(value.snapshot()["valid"].GetBool());
    }
    std::cout<<"PORTFOLIO_STATE_PASS pending_open=reserved pending_close=reserved hedged=gross multi_code=preserved busy=nonblocking getter_queries=0\n";
}
