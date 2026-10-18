#include "Includes/ITraderApi.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSError.hpp"
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cassert>
#include <cmath>
#include <rapidjson/document.h>
using namespace wtp;
std::string native_library;
void near(double a,double b){if(std::abs(a-b)>1e-7){std::cerr<<a<<" != "<<b<<'\n';std::abort();}}
struct Base : IBaseDataMgr {
 WTSCommodityInfo* comm;WTSContractInfo* ct;
 Base() {
  comm=WTSCommodityInfo::create("rb","steel","SHFE","ALLDAY","CHINA");
  comm->setVolScale(10);comm->setPriceTick(1);comm->setCategory(CC_Future);
  comm->setTradingMode(TM_Both);comm->setPriceMode(PM_Both);
  comm->setCoverMode(CM_CoverToday);comm->setMarginRate(0);
  ct=WTSContractInfo::create("rb2701","steel","SHFE","rb");ct->setCommInfo(comm);
  ct->setMarginRatios(.1,.12);
 }
 ~Base(){ct->release();comm->release();}
 WTSCommodityInfo* getCommodity(const char*)override{return comm;}
 WTSCommodityInfo* getCommodity(const char*,const char*)override{return comm;}
 WTSContractInfo*getContract(const char*,const char* = "",uint32_t=0)override{return ct;}
 WTSArray*getContracts(const char* = "",uint32_t=0)override{return nullptr;}
 WTSSessionInfo*getSession(const char*)override{return nullptr;}
 WTSSessionInfo*getSessionByCode(const char*,const char* = "")override{return nullptr;}
 WTSArray*getAllSessions()override{return nullptr;}
 bool isHoliday(const char*,uint32_t,bool=false)override{return false;}
 uint32_t calcTradingDate(const char*,uint32_t d,uint32_t,bool=false)override{return d;}
 uint64_t getBoundaryTime(const char*,uint32_t,bool=false,bool=true)override{return 0;}
};
struct Spi:ITraderSpi {
 Base base;int rejects=0;std::vector<std::string> orders;std::vector<double> prices,volumes;
 double balance=0,margin=0,available=0,commission=0,frozen=0,frozen_fee=0,unrealized=0;
 IBaseDataMgr*getBaseDataMgr()override{return &base;}
 void handleEvent(WTSTraderEvent,int32_t)override{}
 void onLoginResult(bool,const char*,uint32_t)override{}
 void onRspEntrust(WTSEntrust*,WTSError*error)override{if(error)rejects++;}
 void onPushOrder(WTSOrderInfo*o)override{
  if(orders.empty()||orders.back()!=o->getOrderID())orders.push_back(o->getOrderID());
 }
 void onPushTrade(WTSTradeInfo*t)override{prices.push_back(t->getPrice());volumes.push_back(t->getVolume());}
 void onRspAccount(WTSArray*a)override{
  auto v=(WTSAccountInfo*)a->at(0);balance=v->getBalance();margin=v->getMargin();
  available=v->getAvailable();commission=v->getCommission();frozen=v->getFrozenMargin();
  frozen_fee=v->getFrozenCommission();unrealized=v->getDynProfit();
 }
};
struct Run {
 Spi spi;ITraderApi*api;uint32_t id;void*h;
 int(*step)(uint32_t,const WTSTickStruct*);bool(*barrier)(uint32_t);
 uint32_t tickCount=0;
 Run(const std::string&name,double capital,uint32_t slip=0,const std::string&fees="fees.json",bool overrideMargin=false) {
  std::filesystem::create_directories(name);std::filesystem::current_path(name);
  std::ofstream f("fees.json");
  f<<(fees=="amount"?R"({"SHFE.rb":{"byvolume":false,"open":0.001,"close":0.002,"closetoday":0.003,"minfee":5}})":R"({"SHFE.rb":{"byvolume":true,"open":2,"close":3,"closetoday":4}})");f.close();
  h=dlopen(native_library.c_str(),RTLD_NOW|RTLD_LOCAL);
  if(!h){std::cerr<<dlerror();std::abort();}
  auto create=(FuncCreateTrader)dlsym(h,"createTrader");api=create();
  step=(decltype(step))dlsym(h,"plus_mocker_step");barrier=(decltype(barrier))dlsym(h,"plus_mocker_barrier");
  static uint32_t next=0;id=++next;
  auto params=WTSVariant::createObject();params->append("replay",true);
  params->append("mockerid",id);params->append("capital",capital);params->append("fees","fees.json");
  params->append("slippage_ticks",slip);
  params->append("replay_date",(uint32_t)20261012);params->append("replay_tradingdate",(uint32_t)20261012);
  if(overrideMargin)params->append("marginrate",.2);
  assert(api->init(params));params->release();api->registerSpi(&spi);
  api->connect();api->login("","","");assert(barrier(id));tick();
 }
 ~Run(){api->release();delete api;dlclose(h);std::filesystem::current_path("..");}
 void tick(double mark=100,double bid=99,double ask=101,double vol=2,uint32_t date=20261012,double upper=0,double lower=0){
  WTSTickStruct t={};strcpy(t.exchg,"SHFE");strcpy(t.code,"rb2701");
  t.action_date=t.trading_date=date;t.action_time=90000000+(++tickCount)*1000;
  t.upper_limit=upper;t.lower_limit=lower;
  t.price=mark;t.ask_prices[0]=ask;t.bid_prices[0]=bid;
  t.ask_qty[0]=t.bid_qty[0]=vol;assert(step(id,&t)>=0);assert(barrier(id));
 }
 void order(double qty,double price,WTSOffsetType offset=WOT_OPEN,WTSDirectionType direction=WDT_LONG,WTSPriceType type=WPT_LIMITPRICE){
  auto e=WTSEntrust::create("rb2701",qty,price,"SHFE");
  e->setDirection(direction);e->setOffsetType(offset);e->setPriceType(type);
  e->setContractInfo(spi.base.ct);char tag[64];api->makeEntrustID(tag,sizeof(tag));e->setEntrustID(tag);
  api->orderInsert(e);e->release();assert(barrier(id));
 }
 void cancel(){auto a=WTSEntrustAction::create("rb2701","SHFE");a->setOrderID(spi.orders.back().c_str());api->orderAction(a);a->release();assert(barrier(id));}
 void query(){api->queryAccount();assert(barrier(id));}
};
int main(int argc, char** argv){
 if(argc != 3) { std::cerr << "Usage: trader_mocker_finance <TraderMocker library> <temporary output directory>\\n"; return 2; }
 native_library=std::filesystem::absolute(argv[1]).string();
 std::filesystem::create_directories(argv[2]);
 std::filesystem::current_path(argv[2]);
 {
  Run r("partial_cancel_close",100000,2);r.order(3,110);r.query();
  near(r.spi.frozen,330);near(r.spi.frozen_fee,6);near(r.spi.available,99664);
  r.tick();r.query();near(r.spi.commission,4);near(r.spi.frozen,110);near(r.spi.frozen_fee,2);
  near(r.spi.prices[0],103);near(r.spi.balance,99996);near(r.spi.unrealized,-60);near(r.spi.margin,200);
  r.cancel();r.query();near(r.spi.frozen,0);near(r.spi.frozen_fee,0);near(r.spi.available,99736);
  r.order(2,95,WOT_CLOSETODAY);r.tick();r.query();
  near(r.spi.prices.back(),97);near(r.spi.commission,12);near(r.spi.balance,99868);
  near(r.spi.margin,0);near(r.spi.available,99868);
 }
 {
  Run r("freeze_reject_release",220);r.order(1,110);r.order(1,110);r.query();
  assert(r.spi.rejects==1);near(r.spi.frozen,110);near(r.spi.frozen_fee,2);
  r.cancel();r.order(1,110);r.query();assert(r.spi.rejects==1);near(r.spi.frozen,110);
 }
 {
  Run r("loss_allows_close",120);r.order(1,101);r.tick();r.tick(80,79,81);r.query();
  assert(r.spi.available<0);r.order(1,81);assert(r.spi.rejects==1);
  r.order(1,79,WOT_CLOSETODAY);r.tick(80,79,81);r.query();
  near(r.spi.margin,0);near(r.spi.commission,6);near(r.spi.balance,-106);
 }
 {
  Run r("limit_and_empty_book",100000,3);r.order(1,101);r.tick(100,99,101,0);
  assert(r.spi.prices.empty());r.tick();near(r.spi.prices[0],101);
  r.order(1,99,WOT_CLOSETODAY);r.tick();near(r.spi.prices.back(),99);
 }
 {
  Run r("amount_minimum_partial",100000,0,"amount");r.order(3,110);r.tick();r.tick();r.query();
  near(r.spi.commission,10);assert(r.spi.prices.size()==2);
  r.order(3,95,WOT_CLOSETODAY);r.tick();r.tick();r.query();
  // Two fills: 99*2*10*.003=5.94, 99*1*10*.003 floored to minfee5.
  near(r.spi.commission,20.94);near(r.spi.margin,0);
 }
 {
  Run r("yesterday_and_today",100000);r.order(2,101);r.tick();r.tick(100,99,101,2,20261013);
  r.order(1,101);r.tick(100,99,101,2,20261013);
  r.order(2,99,WOT_CLOSE);r.tick(100,99,101,2,20261013);
  r.order(1,99,WOT_CLOSETODAY);r.tick(100,99,101,2,20261013);r.query();
  near(r.spi.commission,16);near(r.spi.margin,0);
 }
 {
  Run r("short_and_margin_override",100000,1,"fees.json",true);
  r.order(2,90,WOT_OPEN,WDT_SHORT);r.tick();r.query();
  near(r.spi.margin,400);near(r.spi.prices.front(),98);near(r.spi.unrealized,-40);
  r.order(2,110,WOT_CLOSETODAY,WDT_SHORT);r.tick();r.query();
  near(r.spi.prices.back(),102);near(r.spi.balance,99908);near(r.spi.margin,0);
 }
 {
  Run r("market_reprice_reject",105);r.tick(100,99,100);r.order(1,0,WOT_OPEN,WDT_LONG,WPT_ANYPRICE);
  r.tick(130,129,130);r.query();assert(r.spi.prices.empty());
  near(r.spi.frozen,0);near(r.spi.frozen_fee,0);near(r.spi.balance,105);
 }
 {
  Run r("price_limit_caps",100000,5);
  r.order(1,110);r.tick(100,99,101,2,20261012,103,95);
  near(r.spi.prices.front(),103);
  r.order(1,90,WOT_CLOSETODAY);r.tick(100,99,101,2,20261012,103,97);
  near(r.spi.prices.back(),97);
 }
 {
  Run r("mixed_yesterday_today_minimum_once",100000,0,"amount");
  r.spi.base.comm->setCoverMode(CM_OpenCover);
  r.order(1,101);r.tick();
  r.tick(100,99,101,2,20261013);
  r.order(1,101);r.tick(100,99,101,2,20261013);
  r.order(2,99,WOT_CLOSE);r.tick(100,99,101,2,20261013);r.query();
  // One native fill closes both dates: 1.98 + 2.97 = 4.95, minimum = 5 once.
  assert(r.spi.volumes.size()==3);near(r.spi.volumes.back(),2);
  near(r.spi.commission,15);near(r.spi.balance,99945);
  near(r.spi.margin,0);near(r.spi.available,99945);
 }
 // Margin rate must remain contract-specific after another product has been queried.
 auto a=WTSCommodityInfo::create("a","a","DCE","ALLDAY","CHINA");
 auto b=WTSCommodityInfo::create("b","b","DCE","ALLDAY","CHINA");
 a->setMarginRate(.2);b->setMarginRate(0);
 auto ca=WTSContractInfo::create("a1","a","DCE","a");ca->setCommInfo(a);ca->setMarginRatios(.1,.1);
 auto cb=WTSContractInfo::create("b1","b","DCE","b");cb->setCommInfo(b);cb->setMarginRatios(.12,.15);
 near(ca->getLongMarginRatio(),.2);near(cb->getLongMarginRatio(),.12);near(cb->getShortMarginRatio(),.15);
 ca->release();cb->release();a->release();b->release();
 std::cout<<"NATIVE_FINANCE_11_CASES_PASSED\n";
}
