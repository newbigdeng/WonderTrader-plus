// Exercise native reset against a local broker; no external network or trading session.
#include <boost/noncopyable.hpp>
#include "WtCore/TraderAdapter.h"
#include "WtCore/CtaStraBaseCtx.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtHelper.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSCollection.hpp"
#include "WTSTools/WTSLogger.h"
#include "WTSUtils/WTSCfgLoader.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSRiskDef.hpp"
#include <rapidjson/document.h>
#include <thread>
#include <future>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <cassert>
#include <iostream>
#include <chrono>
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
 int inserted=0,cancelled=0,positionQueries=0; bool reject=false;
 std::vector<WTSOffsetType> offsets; std::vector<double> quantities;
 bool init(WTSVariant*)override{return true;}
 int queryOrders()override{return 0;}
 int queryPositions()override{++positionQueries;return 0;}
 int queryAccount()override{return 0;}
 bool makeEntrustID(char* out,int)override{strcpy(out,"fake");return true;}
 int orderInsert(WTSEntrust* order)override{
  ++inserted; offsets.push_back(order->getOffsetType());quantities.push_back(order->getVolume());
  assert(order->getOffsetType()!=WOT_OPEN);return reject?-1:0;
 }
 int orderAction(WTSEntrustAction*)override{++cancelled;return 0;}
};
struct Context : CtaStraBaseCtx {
 Context(WtCtaEngine* engine):CtaStraBaseCtx(engine,"private-test",0){}
 void on_bar_close(const char*,const char*,WTSBarStruct*)override{}
 int resetCallbacks=0;std::function<void()> callback;
 void on_calculate(uint32_t,uint32_t)override{++resetCallbacks;if(callback)callback();}
 uint32_t barNumber()const{return _last_barno;}
 void seed(){
  auto& position=_pos_map["SHFE.rb.2701"];position._volume=1;position._closeprofit=17;
  position._details.emplace_back();position._details.back()._volume=1;position._details.back()._price=3033;_sig_map["SHFE.rb.2701"]._volume=-1;
  _condtions["SHFE.rb.2701"].emplace_back();_user_datas["volume_trend"]="private-risk";
  _fund_info._total_profit=17;_fund_info._total_fees=4;
 }
 void check(){assert(_pos_map["SHFE.rb.2701"]._volume==0);assert(_pos_map["SHFE.rb.2701"]._closeprofit==17);
  assert(_sig_map.empty()&&_condtions.empty()&&_user_datas.empty());
  assert(_fund_info._total_profit==17&&_fund_info._total_fees==4);}
};


struct Engine : WtCtaEngine {
 void seedPortfolio(TraderAdapterMgr* mgr){
  _adapter_mgr=mgr;_port_fund=WTSPortFundInfo::create();auto& fund=_port_fund->fundInfo();
  fund._dynprofit=123;fund._profit=17;fund._balance=10017;fund._fees=4;
  auto& position=_pos_map["SHFE.rb.2701"];position.reset(new PosInfo);
  position->_volume=1;position->_dynprofit=123;position->_closeprofit=17;
 }
 void checkPortfolio(){
  assert(_pos_map["SHFE.rb.2701"]->_volume==0);assert(_pos_map["SHFE.rb.2701"]->_closeprofit==17);
  const auto& fund=_port_fund->fundInfo();assert(fund._dynprofit==0&&fund._profit==17&&fund._fees==4&&fund._balance==10017);
  std::ifstream file("generated/portfolio/datas.json");std::string text((std::istreambuf_iterator<char>(file)),{});
  rapidjson::Document data;data.Parse(text.c_str());assert(!data.HasParseError());
  assert(data["fund"]["dynprofit"].GetDouble()==0);assert(data["fund"]["profit"].GetDouble()==17);
 }
};
struct Unit : ExecuteUnit {
 std::atomic<bool>& finished;std::atomic<int>& late;
 Unit(std::atomic<bool>& f,std::atomic<int>& l):finished(f),late(l){}
 const char* getName()override{return "test";}const char* getFactName()override{return "test";}
 void set_position(const char*,double)override{}void on_tick(WTSTickData*)override{}
 void on_trade(uint32_t,const char*,bool,double,double)override{if(finished)++late;}
 void on_order(uint32_t,const char*,bool,double,double,bool)override{}
 void on_entrust(uint32_t,const char*,bool,const char*)override{}
 void on_channel_ready()override{}void on_channel_lost()override{}
};
struct Factory : IExecuterFact {
 const char* getName()override{return "test";}void enumExeUnit(FuncEnumUnitCallback)override{}
 ExecuteUnit* createExeUnit(const char*)override{return nullptr;}
 ExecuteUnit* createDiffExeUnit(const char*)override{return nullptr;}
 ExecuteUnit* createArbiExeUnit(const char*)override{return nullptr;}
 bool deleteExeUnit(ExecuteUnit* unit)override{delete unit;return true;}
};
namespace wtp {
struct NativeResetTestAccessor {
 static void enqueue_barrier(WtLocalExecuter& executor){
  static Factory factory;std::atomic<bool> finished{false};std::atomic<int> late{0};
  executor._unit_map["SHFE.rb.2701"]=std::make_shared<ExeUnitWrapper>(new Unit(finished,late),&factory);
  executor._target_pos["SHFE.rb.2701"]=1;
  executor._pool.reset(new boost::threadpool::pool(1));
  std::promise<void> passed,release;auto released=release.get_future();
  std::thread callback([&]{
   // Pause at the same dispatch gate held between admission and pool scheduling.
   std::lock_guard<std::recursive_mutex> lock(executor._reset_dispatch);
   passed.set_value();released.wait();executor.on_trade(1,"SHFE.rb.2701",true,1,3033);
  });
  passed.get_future().wait();
  auto reset=std::async(std::launch::async,[&]{executor.freeze_for_reset(true);executor.clear_reset_targets();
   executor.freeze_for_reset(false);finished=true;});
  assert(reset.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
  release.set_value();callback.join();reset.get();executor._pool->wait();
  assert(late==0);assert(executor._target_pos.empty()&&executor._unit_map.empty());
 }
};
}

int main(int argc,char** argv){
 assert(argc==2);auto run=std::filesystem::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
 std::filesystem::create_directories(run);std::filesystem::current_path(run);
 WtHelper::setGenerateDir("generated");
 WTSLogger::init(R"({"root":{"level":"error","sinks":[{"type":"ostream_sink","pattern":"%v"}]}})",false);
 std::cerr<<"reset-test: adapter init"<<std::endl;
 Data data;auto broker=new Broker;TraderAdapter adapter;assert(adapter.initExt("reset-test",broker,&data,nullptr));
 assert(adapter.run());adapter.onLoginResult(true,"ok",20261023);
 auto empty=WTSArray::create();adapter.onRspPosition(empty);adapter.onRspOrders(empty);adapter.onRspTrades(empty);
 adapter.onRspAccount(empty);assert(adapter.isReady());
 std::string error;
 auto quote=[](const std::string&,bool){return 3034.;};
 auto step=[&](uint64_t now){return adapter.advance_strategy_reset(now,quote,error);};
 auto positions=[&](double quantity){
  auto rows=WTSArray::create();
  if(quantity){auto p=WTSPositionItem::create("rb2701","CNY","SHFE");p->setContractInfo(data.contract);
   p->setDirection(WDT_SHORT);p->setNewPosition(quantity);p->setAvailNewPos(quantity);rows->append(p,false);}
  adapter.onRspPosition(rows);rows->release();
 };
 auto flat=[&](uint64_t now){assert(step(now)=="cancelling");adapter.onRspOrders(empty);
  assert(step(now+1)=="confirming");positions(0);assert(step(now+2)=="ready");};
 std::cerr<<"reset-test: flat broker"<<std::endl;
 // Broker flat / theory one: no offset order may be invented to repair the theory.
 assert(adapter.begin_strategy_reset("flat",{"SHFE.rb.2701"},error));flat(100);
 assert(broker->inserted==0);adapter.finish_strategy_reset();
 std::cerr<<"reset-test: context archive"<<std::endl;
 auto engine=std::make_unique<Engine>();Context context(engine.get());context.seed();
 std::filesystem::create_directories("generated/outputs/private-test");
 {std::ofstream log("generated/outputs/private-test/signals.csv");log<<"old-signal\n";}
 context.freeze_for_reset(true);assert(context.archive_and_reset("archive-flat",error));context.check();
 assert(std::filesystem::exists("archive-flat/userdata.json"));assert(std::filesystem::exists("archive-flat/signals.csv"));
 {std::ifstream log("generated/outputs/private-test/signals.csv");std::string line;std::getline(log,line);
  assert(line=="code,target,sigprice,gentime,usertag");assert(!std::getline(log,line));}
 WtLocalExecuter executor(nullptr,"managed-private",nullptr);
 auto executorConfig=WTSCfgLoader::load_from_content(R"({"scale":1,"policy":{"default":{"name":"test"}}})");
 assert(executor.init(executorConfig));executorConfig->release();executor.setTrader(&adapter);
 executor.freeze_for_reset(true);assert(executor.buy("SHFE.rb.2701",3034,1).empty());
 executor.clear_reset_targets();executor.freeze_for_reset(false);
 std::cerr<<"reset-test: executor barrier"<<std::endl;
 NativeResetTestAccessor::enqueue_barrier(executor);
 std::cerr<<"reset-test: executor barrier complete"<<std::endl;

 std::cerr<<"reset-test: owned cancellation"<<std::endl;
 assert(adapter.begin_strategy_reset("cancel",{"SHFE.rb.2701"},error));assert(step(300)=="cancelling");
 auto open=WTSEntrust::create("rb2701",2,3033);open->setContractInfo(data.contract);
 open->setUserTag("otp.reset-test.12");open->setDirection(WDT_SHORT);open->setOffsetType(WOT_OPEN);
 auto order=WTSOrderInfo::create(open);order->setContractInfo(data.contract);order->setOrderState(WOS_PartTraded_Queuing);
 order->setVolTraded(1);order->setVolLeft(1);auto orders=WTSArray::create();orders->append(order,false);
 adapter.onRspOrders(orders);orders->release();open->release();
 assert(step(301)=="cancelling");assert(broker->cancelled==1&&broker->inserted==0);flat(2000);adapter.finish_strategy_reset();
 std::cerr<<"reset-test: metrics drain and partial fill"<<std::endl;
 // A partial fill invalidates the in-flight position snapshot before any close.
 auto accounts=WTSArray::create();auto account=WTSAccountInfo::create();account->setCurrency("CNY");
 accounts->append(account,false);adapter.onRspAccount(accounts);const int queries=broker->positionQueries;
 adapter.reset_trade_event();assert(adapter.begin_strategy_reset("partial",{"SHFE.rb.2701"},error));
 assert(step(3000)=="confirming");adapter.onRspAccount(accounts);assert(broker->positionQueries==queries);
 positions(0); // Old normal query A: cannot be accepted as reset query B.
 assert(step(3001)=="cancelling");adapter.onRspOrders(empty);assert(step(3002)=="confirming");
 adapter.reset_trade_event();positions(2);assert(step(3003)=="cancelling");
 adapter.onRspOrders(empty);assert(step(3004)=="confirming");positions(1);
 assert(step(3005)=="flattening");accounts->release();assert(broker->inserted==1);assert(broker->quantities.back()==1);
 assert(broker->offsets.back()==WOT_CLOSETODAY);flat(9000);adapter.finish_strategy_reset();
 std::cerr<<"reset-test: wrong contract"<<std::endl;
 // Unknown/shared contract is refused without a single close submission.
 assert(adapter.begin_strategy_reset("shared",{"SHFE.rb.2705"},error));assert(step(8000)=="cancelling");
 adapter.onRspOrders(empty);assert(step(8001)=="confirming");positions(1);assert(step(8002)=="failed");
 assert(broker->inserted==1);adapter.fail_strategy_reset();
 std::cerr<<"reset-test: rejected close"<<std::endl;
 // Explicit retry can take over a failed freeze, and rejected closes remain frozen.
 assert(adapter.begin_strategy_reset("reject",{"SHFE.rb.2701"},error));assert(step(9000)=="cancelling");
 adapter.onRspOrders(empty);assert(step(9001)=="confirming");positions(1);broker->reject=true;
 assert(step(9002)=="failed");adapter.fail_strategy_reset();const int count=broker->inserted;
 assert(adapter.closeShort("SHFE.rb.2701",3034,1,true,0)==UINT_MAX);assert(broker->inserted==count);
 assert(std::filesystem::exists("generated/traders/reset-test/rtdata.json.reset-block"));

 std::cerr<<"reset-test: engine archive and portfolio"<<std::endl;
 broker->reject=false;TraderAdapterMgr manager;TraderAdapterPtr owned(&adapter,[](TraderAdapter*){});
 assert(manager.addAdapter("reset-test",owned));engine->seedPortfolio(&manager);context.seed();
 engine->addContext(CtaContextPtr(&context,[](ICtaStraCtx*){}));
 auto local=std::make_shared<WtLocalExecuter>(nullptr,"managed-private",nullptr);
 auto setup=WTSCfgLoader::load_from_content(R"({"scale":1,"policy":{"default":{"name":"test"}},"routers":[{"strategy":"private-test","executer":"managed-private"}]})");
 assert(setup&&local->init(setup));local->setTrader(&adapter);
 engine->addExecuter(local);adapter.addSink(local.get());
 assert(engine->loadRouterRules(setup->get("routers")));setup->release();
 assert(engine->request_strategy_reset("private-test","reset-test","engine-flat",R"(["SHFE.rb.2701"])"));
 engine->process_strategy_resets();adapter.onRspOrders(empty);engine->process_strategy_resets();positions(0);engine->process_strategy_resets();
 assert(std::string(engine->query_strategy_reset("engine-flat")).find("completed")!=std::string::npos);
 context.check();engine->checkPortfolio();
 std::cerr<<"reset-test: idle initialization wakeup"<<std::endl;
 const auto barNumber=context.barNumber();const auto inserted=broker->inserted;
 context.callback=[&]{assert(std::string(engine->query_strategy_reset("engine-flat")).find("completed")!=std::string::npos);};
 assert(!engine->wakeup_strategy_reset("missing"));
 assert(engine->wakeup_strategy_reset("engine-flat"));assert(engine->wakeup_strategy_reset("engine-flat"));
 engine->process_strategy_resets();engine->process_strategy_resets();
 assert(engine->wakeup_strategy_reset("engine-flat"));engine->process_strategy_resets();
 assert(context.resetCallbacks==1&&context.barNumber()==barNumber&&broker->inserted==inserted);
 context.callback={};
 std::cerr<<"reset-test: ownership rejection"<<std::endl;
 // Ownership / not-ready failure is reported before an unconfigured engine can mutate state.
 assert(engine->request_strategy_reset("unknown","reset-test","ownership",R"(["SHFE.rb.2701"] )"));
 engine->process_strategy_resets();assert(std::string(engine->query_strategy_reset("ownership")).find("failed")!=std::string::npos);


 std::cerr<<"reset-test: abandoned reset query drain"<<std::endl;
 // Explicit retries must drain old reset order/position queries as well as normal metrics.
 assert(adapter.begin_strategy_reset("old-order",{"SHFE.rb.2701"},error));assert(step(15000)=="cancelling");
 adapter.fail_strategy_reset();assert(adapter.begin_strategy_reset("retry-order",{"SHFE.rb.2701"},error));
 assert(step(15001)=="confirming");adapter.onRspOrders(empty); // Abandoned order response.
 assert(step(15002)=="cancelling");adapter.onRspOrders(empty);assert(step(15003)=="confirming");
 adapter.fail_strategy_reset();assert(adapter.begin_strategy_reset("retry-position",{"SHFE.rb.2701"},error));
 assert(step(15004)=="confirming");positions(0); // Abandoned B reports flat, but C has not been queried.
 assert(step(15005)=="cancelling");adapter.onRspOrders(empty);assert(step(15006)=="confirming");
 positions(1);assert(step(15007)=="flattening");assert(broker->quantities.back()==1);
 flat(22000);assert(adapter.finish_strategy_reset());
 std::cerr<<"reset-test: restart persistence"<<std::endl;
 // A valid request freezes first even when the broker becomes unavailable.
 adapter.onLogout();assert(adapter.begin_strategy_reset("not-ready",{"SHFE.rb.2701"},error));
 assert(step(12000)=="failed");adapter.fail_strategy_reset();
 auto restartedBroker=new Broker;TraderAdapter restarted;
 assert(restarted.initExt("reset-test",restartedBroker,&data,nullptr));
 assert(restarted.closeShort("SHFE.rb.2701",3034,1,true,0)==UINT_MAX);
 assert(restartedBroker->inserted==0);restarted.release();
 std::cerr<<"reset-test: archive failure"<<std::endl;
 // An archive failure must leave the old active theory intact and frozen.
 context.seed();context.freeze_for_reset(true);{std::ofstream file("bad-archive");file<<"regular-file";}
 assert(!context.archive_and_reset("bad-archive/child",error));
 assert(context.stra_get_position("SHFE.rb.2701")==1);
 std::cerr<<"reset-test: cleanup"<<std::endl;
 empty->release();adapter.release();WTSLogger::stop();
 std::cout<<"native reset: flat theory, archive, gates, fresh snapshots, reduce-only and rejection passed\n";
}
