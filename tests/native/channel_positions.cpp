// Drives the real adapter with a local fake broker. No network or order API.
#include <boost/noncopyable.hpp>
#include "WtCore/TraderAdapter.h"
#include "WtCore/WtHelper.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSCollection.hpp"
#include "WTSTools/WTSLogger.h"
#include <rapidjson/document.h>
#include <filesystem>
#include <fstream>
#include <cassert>
#include <iostream>
using namespace wtp;

struct Base : IBaseDataMgr {
 WTSCommodityInfo* comm=WTSCommodityInfo::create("rb","steel","SHFE","NIGHT","CHINA");
 WTSContractInfo* ct=WTSContractInfo::create("rb2701","steel","SHFE","rb");
 Base(){comm->setVolScale(10);comm->setCategory(CC_Future);comm->setTradingMode(TM_Both);comm->setCoverMode(CM_CoverToday);ct->setCommInfo(comm);}
 ~Base(){ct->release();comm->release();}
 WTSCommodityInfo* getCommodity(const char*)override{return comm;}
 WTSCommodityInfo* getCommodity(const char*,const char*)override{return comm;}
 WTSContractInfo* getContract(const char*,const char* = "",uint32_t=0)override{return ct;}
 WTSArray* getContracts(const char* = "",uint32_t=0)override{return nullptr;}
 WTSSessionInfo* getSession(const char*)override{return nullptr;}
 WTSSessionInfo* getSessionByCode(const char*,const char* = "")override{return nullptr;}
 WTSArray* getAllSessions()override{return nullptr;}
 bool isHoliday(const char*,uint32_t,bool=false)override{return false;}
 uint32_t calcTradingDate(const char*,uint32_t d,uint32_t,bool=false)override{return d;}
 uint64_t getBoundaryTime(const char*,uint32_t,bool=false,bool=true)override{return 0;}
};
struct Broker : ITraderApi {
 int positionQueries=0,accountQueries=0,orderQueries=0;
 bool init(WTSVariant*)override{return true;}
 int queryPositions()override{++positionQueries;return 0;}
 int queryAccount()override{++accountQueries;return 0;}
 int queryOrders()override{++orderQueries;return 0;}
};

int main(int argc,char** argv){
 assert(argc==2);
 std::filesystem::create_directories(argv[1]);std::filesystem::current_path(argv[1]);
 WtHelper::setGenerateDir("generated");
 WTSLogger::init(R"({"root":{"level":"error","sinks":[{"type":"ostream_sink","pattern":"%v"}]}})",false);
 Base base;auto broker=new Broker;
 TraderAdapter adapter;assert(adapter.initExt("test",broker,&base,nullptr));assert(adapter.run());
 auto reply=[&](double volume,double available,double price,double profit){
  auto rows=WTSArray::create();auto position=WTSPositionItem::create("rb2701","CNY","SHFE");
  position->setContractInfo(base.ct);position->setDirection(WDT_LONG);position->setNewPosition(volume);
  position->setAvailNewPos(available);position->setAvgPrice(price);position->setPositionCost(price*volume*10);
  position->setOpenCost((price-2)*volume*10);position->setMargin(volume*4800);
  position->setDynProfit(profit);rows->append(position,false);adapter.onRspPosition(rows);rows->release();
 };
 auto funds=WTSArray::create();auto fund=WTSAccountInfo::create();fund->setCurrency("CNY");
 fund->setBalance(10000);fund->setAvailable(6000);fund->setDynProfit(20);funds->append(fund,false);
 auto snapshot=[](){
  std::ifstream file("generated/traders/test/rtdata.json");
  std::string text((std::istreambuf_iterator<char>(file)),{});rapidjson::Document data;data.Parse(text.c_str());
  assert(!data.HasParseError());return data;
 };
 adapter.onLoginResult(true,"ok",20261023);
 adapter.onRspPosition(nullptr);assert(broker->orderQueries==0);
 reply(1,1,3033,20);assert(broker->orderQueries==1);
 auto noOrders=WTSArray::create();adapter.onRspOrders(noOrders);noOrders->release();
 adapter.onRspTrades(nullptr);adapter.onRspAccount(funds);
 auto initial=snapshot();auto& before=initial["positions"][0]["long"];
 assert(before["metrics_valid"].GetBool());assert(before["avg_price"].GetDouble()==3033);
 assert(before["position_cost"].GetDouble()==30330);assert(before["dynprofit"].GetDouble()==20);
 assert(before["open_price"].GetDouble()==3031);assert(before["margin"].GetDouble()==4800);
 assert(before["metrics_updated_at"].GetUint64()>0);

 // A fill while a position query is outstanding invalidates that whole reply.
 adapter.onRspAccount(funds);int queries=broker->positionQueries;
 adapter.onRspAccount(funds);assert(broker->positionQueries==queries);
 auto trade=WTSTradeInfo::create("rb2701","SHFE");trade->setContractInfo(base.ct);
 trade->setDirection(WDT_LONG);trade->setOffsetType(WOT_OPEN);trade->setVolume(1);trade->setPrice(3031);
 adapter.onPushTrade(trade);trade->release();
 auto filled=snapshot();assert(!filled["positions"][0]["long"]["metrics_valid"].GetBool());
 assert(filled["positions"][0]["long"]["newvol"].GetDouble()==2);
 reply(2,0,1000,99999);assert(!snapshot()["positions"][0]["long"]["metrics_valid"].GetBool());
 assert(adapter.getPosition("SHFE.rb.2701",false)==2);

 // A later receipt may refresh display metrics, never execution quantities.
 adapter.onRspAccount(funds);auto account=snapshot();
 auto accountTime=account["funds"]["CNY"]["updated_at"].GetUint64();reply(2,0,3032,50);
 auto current=snapshot();auto& pos=current["positions"][0]["long"];
 assert(pos["metrics_valid"].GetBool());assert(pos["avg_price"].GetDouble()==3032);
 assert(pos["dynprofit"].GetDouble()==50);assert(pos["newavail"].GetDouble()==2);
 assert(adapter.getPosition("SHFE.rb.2701",true)==2);
 assert(current["funds"]["CNY"]["updated_at"].GetUint64()==accountTime);
 assert(current["funds"]["CNY"]["balance"].GetDouble()==10000);
 adapter.onRspAccount(funds);reply(3,0,3032,100);
 assert(!snapshot()["positions"][0]["long"]["metrics_valid"].GetBool());
 assert(adapter.getPosition("SHFE.rb.2701",false)==2);
 adapter.onRspAccount(funds);auto empty=WTSArray::create();adapter.onRspPosition(empty);empty->release();
 assert(!snapshot()["positions"][0]["long"]["metrics_valid"].GetBool());
 funds->release();adapter.release();WTSLogger::stop();
 std::cout<<"position receipt, fill invalidation, stale-query discard and account preservation passed\n";
}
