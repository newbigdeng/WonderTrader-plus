// Local FakeTrader only. Reconciliation must never insert, cancel or erase state.
#include "WtCore/TraderAdapter.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/CtaStraBaseCtx.h"
#include "WtCore/WtHelper.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"
#include "WTSTools/WTSLogger.h"
#include "Share/TimeUtils.hpp"
#include <rapidjson/document.h>
#include <filesystem>
#include <fstream>
#include <cassert>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace wtp;

struct Data : IBaseDataMgr {
 WTSCommodityInfo* comm=WTSCommodityInfo::create("rb","steel","SHFE","NIGHT","CHINA");
 WTSContractInfo* contract=WTSContractInfo::create("rb2701","steel","SHFE","rb");
 Data(){comm->setVolScale(10);comm->setCategory(CC_Future);contract->setCommInfo(comm);}
 ~Data(){contract->release();comm->release();}
 WTSCommodityInfo* getCommodity(const char*)override{return comm;}
 WTSCommodityInfo* getCommodity(const char*,const char*)override{return comm;}
 WTSContractInfo* getContract(const char*,const char* = "",uint32_t=0)override{return contract;}
 WTSArray* getContracts(const char* = "",uint32_t=0)override{return nullptr;}
 WTSSessionInfo* getSession(const char*)override{return nullptr;}
 WTSSessionInfo* getSessionByCode(const char*,const char* = "")override{return nullptr;}
 WTSArray* getAllSessions()override{return nullptr;}
 bool isHoliday(const char*,uint32_t,bool=false)override{return false;}
 uint32_t calcTradingDate(const char*,uint32_t day,uint32_t,bool=false)override{return day;}
 uint64_t getBoundaryTime(const char*,uint32_t,bool=false,bool=true)override{return 0;}
};
struct Broker : ITraderApi {
 int inserted=0,cancelled=0,orders=0,positions=0; bool reject=false;
 std::function<void()> final_callback;
 bool init(WTSVariant*)override{return true;}
 int queryOrders()override{++orders;return reject?-1:0;}
 int queryPositions()override{++positions;return reject?-1:0;}
 int queryAccount()override{return 0;}
 bool makeEntrustID(char* out,int)override{strcpy(out,"fake");return true;}
 int orderInsert(WTSEntrust*)override{++inserted;return 0;}
 int orderAction(WTSEntrustAction*)override{++cancelled;return 0;}
 void release()override{if(final_callback)final_callback();}
};
struct Context : CtaStraBaseCtx {
 Context(WtCtaEngine* engine):CtaStraBaseCtx(engine,"example",0){}
 void on_bar_close(const char*,const char*,WTSBarStruct*)override{}
 void on_calculate(uint32_t,uint32_t)override{}
 void seed(){_pos_map["SHFE.rb.2701"]._volume=1;_sig_map["SHFE.rb.2701"]._volume=1;}
 void unseed(){_pos_map.clear();_sig_map.clear();}
 void signal(double qty){_sig_map["SHFE.rb.2701"]._volume=qty;}
 size_t signals()const{return _sig_map.size();}
};
struct Engine : WtCtaEngine {
 void manager(TraderAdapterMgr* mgr){_adapter_mgr=mgr;}
 void signal(double qty){_sig_map["SHFE.rb.2701"]._volume=qty;}
 size_t signals()const{return _sig_map.size();}
};
namespace wtp { struct NativeResetTestAccessor {
 static void target(WtLocalExecuter& executor,double qty){executor._target_pos["SHFE.rb.2701"]=qty;}
}; }
struct Fixture {
 Data data; Broker* broker=new Broker; TraderAdapter adapter; Engine engine; Context context{&engine};
 TraderAdapterMgr manager; std::shared_ptr<WtLocalExecuter> executor; WTSArray* empty=WTSArray::create();
 Fixture(bool guarded=false){
  assert(adapter.initExt("fake",broker,&data,nullptr));assert(adapter.run());adapter.onLoginResult(true,"ok",20261027);
  adapter.onRspPosition(empty);adapter.onRspOrders(empty);adapter.onRspTrades(empty);adapter.onRspAccount(empty);
  assert(adapter.isReady());TraderAdapterPtr owned(&adapter,[](TraderAdapter*){});assert(manager.addAdapter("fake",owned));
  engine.manager(&manager);
  executor=std::make_shared<WtLocalExecuter>(nullptr,"owned",nullptr);
  auto config=WTSCfgLoader::load_from_content(R"({"scale":1,"policy":{"default":{"name":"test"}},"routers":[{"strategy":"example","executer":"owned"}]})");
  assert(executor->init(config));executor->setTrader(&adapter);engine.addExecuter(executor);adapter.addSink(executor.get());
  assert(engine.loadRouterRules(config->get("routers")));config->release();
  assert(engine.configure_strategy_reconcile("boot-test","plan-test"));
  if(guarded)assert(engine.arm_strategy_startup_guard("startup-test"));
  engine.addContext(CtaContextPtr(&context,[](ICtaStraCtx*){}));
 }
 ~Fixture(){adapter.release();empty->release();}
 bool request(const char* id,const char* purpose="before_stop"){
  const auto json=std::string("{\"request_id\":\"")+id+"\",\"strategyid\":\"example\",\"trader\":\"fake\",\"contracts\":[\"SHFE.rb.2701\"],\"expected_pid\":"+
   std::to_string(getpid())+",\"expected_boot_id\":\"boot-test\",\"plan_revision\":\"plan-test\",\"purpose\":\""+purpose+"\",\"requested_at_ms\":"+std::to_string(TimeUtils::getLocalTimeNow())+"}";
  return engine.request_strategy_reconcile(json.c_str());
 }
 rapidjson::Document result(const char* id){rapidjson::Document value;value.Parse(engine.query_strategy_reset(id));assert(!value.HasParseError());return value;}
 void complete(){engine.process_strategy_resets();adapter.onRspOrders(empty);engine.process_strategy_resets();adapter.onRspPosition(empty);engine.process_strategy_resets();}
};
static rapidjson::Document seal(){
 std::ifstream file("generated/strategy-control/reconcile-seal.json");std::string text((std::istreambuf_iterator<char>(file)),{});
 rapidjson::Document value;value.Parse(text.c_str());assert(!value.HasParseError());return value;
}
int main(int argc,char** argv){
 assert(argc==2);std::filesystem::create_directories(argv[1]);std::filesystem::current_path(argv[1]);
 WtHelper::setGenerateDir("generated");WTSLogger::init(R"({"root":{"level":"error","sinks":[{"type":"ostream_sink","pattern":"%v"}]}})",false);
 {
  Fixture f;assert(f.request("empty"));f.engine.process_strategy_resets();
  assert(!f.result("empty")["query_complete"].GetBool());
  f.adapter.onRspOrders(f.empty);f.engine.process_strategy_resets();
  assert(!f.result("empty")["positions_complete"].GetBool());
  f.adapter.onRspPosition(f.empty);f.engine.process_strategy_resets();auto r=f.result("empty");
  assert(r["query_complete"].GetBool()&&r["positions_complete"].GetBool()&&r["orders_complete"].GetBool());
  assert(r["positions"].Empty()&&r["alive_orders"].Empty()&&r["execution_idle"].GetBool());
  assert(!f.context.reset_frozen()&&!f.executor->reset_frozen());
  assert(!std::filesystem::exists("generated/traders/fake/rtdata.json.reset-block"));
  assert(f.broker->inserted==0&&f.broker->cancelled==0);f.adapter.release();assert(seal()["valid"].GetBool());
 }
 {
  Fixture f;f.context.seed();NativeResetTestAccessor::target(*f.executor,1);
  assert(f.request("targets"));f.complete();assert(!f.result("targets")["execution_idle"].GetBool());
  assert(f.context.stra_get_position("SHFE.rb.2701")==1);assert(!f.executor->reconcile_idle());
  assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 {
  Fixture f;assert(f.request("changed"));f.engine.process_strategy_resets();f.adapter.onRspOrders(f.empty);
  f.engine.process_strategy_resets();f.adapter.reset_trade_event();f.adapter.onRspPosition(f.empty);f.engine.process_strategy_resets();
  assert(std::string(f.result("changed")["status"].GetString())=="failed");
  assert(!f.context.reset_frozen()&&!f.executor->reset_frozen());assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 {
  Fixture f;f.broker->reject=true;assert(f.request("rejected"));f.engine.process_strategy_resets();
  assert(std::string(f.result("rejected")["status"].GetString())=="failed");assert(!f.context.reset_frozen());
  assert(!f.executor->reset_frozen());assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 {
  Fixture f;assert(f.request("late-event"));f.complete();assert(f.result("late-event")["query_complete"].GetBool());
  f.broker->final_callback=[&]{f.adapter.reset_trade_event();};f.adapter.release();assert(!seal()["valid"].GetBool());
 }
 {
  Fixture f;f.context.freeze_for_reset(true);f.executor->freeze_for_reset(true);
  assert(f.request("preserved","prepare"));f.complete();assert(f.result("preserved")["query_complete"].GetBool());
  assert(f.context.reset_frozen()&&f.executor->reset_frozen());assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 {
  Fixture f(true);assert(f.context.reset_frozen()&&f.executor->reset_frozen());
  assert(f.adapter.openLong("SHFE.rb.2701",3033,1,0)==UINT_MAX);assert(f.broker->inserted==0);
  assert(!f.engine.release_strategy_startup_guard("startup-test")); // No broker query yet.
  assert(f.request("startup-empty","prepare"));f.complete();assert(f.result("startup-empty")["execution_idle"].GetBool());
  assert(!f.engine.release_strategy_startup_guard("wrong-nonce"));
  assert(f.engine.release_strategy_startup_guard("startup-test"));
  assert(!f.context.reset_frozen()&&!f.executor->reset_frozen());assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 {
  Fixture f(true);f.context.seed();assert(f.request("startup-theory","prepare"));f.complete();
  assert(!f.result("startup-theory")["execution_idle"].GetBool());assert(!f.engine.release_strategy_startup_guard("startup-test"));
  assert(f.context.reset_frozen()&&f.executor->reset_frozen());assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 {
  Fixture f;assert(f.adapter.openLong("SHFE.rb.2701",3033,1,0)!=UINT_MAX);
  const int sent=f.broker->inserted;assert(f.request("unacknowledged"));f.complete();
  assert(f.result("unacknowledged")["pending_submissions"].GetUint()==1);
  assert(f.broker->inserted==sent&&f.broker->cancelled==0);
 }
 {
  Fixture f;assert(f.request("alive"));f.engine.process_strategy_resets();
  auto entrust=WTSEntrust::create("rb2701",1,3033);entrust->setContractInfo(f.data.contract);
  entrust->setUserTag("otp.fake.123");auto order=WTSOrderInfo::create(entrust);
  order->setContractInfo(f.data.contract);order->setOrderID("working");order->setOrderState(WOS_NotTraded_Queuing);order->setVolLeft(1);
  auto orders=WTSArray::create();orders->append(order,false);f.adapter.onRspOrders(orders);orders->release();entrust->release();
  f.engine.process_strategy_resets();f.adapter.onRspPosition(f.empty);f.engine.process_strategy_resets();
  auto result=f.result("alive");assert(result["orders_complete"].GetBool()&&result["alive_orders"].Size()==1);
  assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 {
  Fixture f;assert(f.request("query-error"));f.engine.process_strategy_resets();f.adapter.onRspOrders(nullptr);f.engine.process_strategy_resets();
  assert(std::string(f.result("query-error")["status"].GetString())=="failed");
  assert(!f.context.reset_frozen()&&!f.executor->reset_frozen());assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 {
  Fixture f;assert(f.request("disconnected"));f.complete();assert(f.result("disconnected")["query_complete"].GetBool());
  f.adapter.onLogout();assert(!f.result("disconnected")["query_complete"].GetBool());
  f.adapter.release();assert(!seal()["valid"].GetBool());
 }
 {
  Fixture f;assert(f.request("timed-out"));f.engine.process_strategy_resets();
  std::this_thread::sleep_for(std::chrono::milliseconds(10100));f.engine.process_strategy_resets();
  assert(std::string(f.result("timed-out")["status"].GetString())=="failed");
  assert(!f.context.reset_frozen()&&!f.executor->reset_frozen());
  const int queries=f.broker->orders;assert(f.request("after-late"));f.engine.process_strategy_resets();
  assert(f.broker->orders==queries);f.adapter.onRspOrders(f.empty); // Drain timed-out query, never complete the new one.
  assert(!f.result("after-late")["query_complete"].GetBool());f.complete();
  assert(f.broker->orders==queries+1&&f.result("after-late")["query_complete"].GetBool());
  assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 {
  Fixture f(true);assert(f.request("metrics-recheck","prepare"));f.complete();
  const auto epoch=f.result("metrics-recheck")["event_seq"].GetUint64();
  f.adapter.onRspPosition(f.empty); // Ordinary same-flat metrics must not invalidate the proof.
  assert(f.result("metrics-recheck")["event_seq"].GetUint64()==epoch);
  assert(f.result("metrics-recheck")["query_complete"].GetBool());
  auto positions=WTSArray::create();auto position=WTSPositionItem::create("rb2701","CNY","SHFE");
  position->setContractInfo(f.data.contract);position->setDirection(WDT_LONG);position->setNewPosition(1);position->setAvailNewPos(1);
  positions->append(position,false);f.adapter.onRspPosition(positions);positions->release();
  assert(!f.result("metrics-recheck")["query_complete"].GetBool());
  assert(!f.engine.release_strategy_startup_guard("startup-test"));assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 {
  Fixture f;std::string error;assert(f.adapter.begin_strategy_reset("old-failed-reset",{"SHFE.rb.2701"},error));
  f.adapter.fail_strategy_reset();assert(f.request("inspect-blocked","prepare"));f.complete();
  assert(f.result("inspect-blocked")["query_complete"].GetBool());
  assert(!f.result("inspect-blocked")["execution_idle"].GetBool());
  assert(std::filesystem::exists("generated/traders/fake/rtdata.json.reset-block"));
  assert(f.adapter.openLong("SHFE.rb.2701",3033,1,0)==UINT_MAX);assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 std::filesystem::remove("generated/traders/fake/rtdata.json.reset-block"); // Remove only this fixture's own marker.
 {
  Fixture f(true);f.context.signal(0);f.engine.signal(0);
  assert(f.request("zero-signals","prepare"));f.complete();assert(f.result("zero-signals")["execution_idle"].GetBool());
  assert(f.engine.release_strategy_startup_guard("startup-test"));
  assert(f.context.signals()==1&&f.engine.signals()==1);assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 {
  Fixture f(true);f.context.signal(0);f.engine.signal(1);
  assert(f.request("nonzero-signal","prepare"));f.complete();assert(!f.result("nonzero-signal")["execution_idle"].GetBool());
  assert(!f.engine.release_strategy_startup_guard("startup-test"));assert(f.engine.signals()==1);
  assert(f.broker->inserted==0&&f.broker->cancelled==0);
 }
 std::cout<<"read-only reconciliation tests passed\n";
}
