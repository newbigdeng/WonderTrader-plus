// Real adapter, local fake broker. No external connection or CTP order API.
#include <boost/noncopyable.hpp>
#include "WtCore/TraderAdapter.h"
#include "WtCore/WtHelper.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSError.hpp"
#include "WTSTools/WTSLogger.h"
#include <rapidjson/document.h>
#include <filesystem>
#include <fstream>
#include <cassert>
#include <iostream>
#include <vector>
using namespace wtp;

struct Data : IBaseDataMgr {
 WTSCommodityInfo* comm=WTSCommodityInfo::create("rb","steel","SHFE","NIGHT","CHINA");
 WTSContractInfo* contract=WTSContractInfo::create("rb2701","steel","SHFE","rb");
 Data(){comm->setVolScale(10);comm->setCategory(CC_Future);comm->setTradingMode(TM_Both);
  comm->setCoverMode(CM_CoverToday);contract->setCommInfo(comm);}
 ~Data(){contract->release();comm->release();}
 WTSCommodityInfo* getCommodity(const char*)override{return comm;}
 WTSCommodityInfo* getCommodity(const char*,const char*)override{return comm;}
 WTSContractInfo* getContract(const char*,const char* = "",uint32_t=0)override{return contract;}
 WTSArray* getContracts(const char* = "",uint32_t=0)override{return nullptr;}
 WTSSessionInfo* getSession(const char*)override{return nullptr;}
 WTSSessionInfo* getSessionByCode(const char*,const char* = "")override{return nullptr;}
 WTSArray* getAllSessions()override{return nullptr;}
 bool isHoliday(const char*,uint32_t,bool=false)override{return false;}
 uint32_t calcTradingDate(const char*,uint32_t date,uint32_t,bool=false)override{return date;}
 uint64_t getBoundaryTime(const char*,uint32_t,bool=false,bool=true)override{return 0;}
};

struct Broker : ITraderApi {
 std::vector<WTSEntrust*> inserted;
 int sequence=0,cancels=0;bool rejectSynchronously=false;
 ~Broker(){for(auto order:inserted)order->release();}
 bool init(WTSVariant*)override{return true;}
 int queryOrders()override{return 0;}
 int queryPositions()override{return 0;}
 int queryAccount()override{return 0;}
 bool makeEntrustID(char* buffer,int)override{
  const auto id="fixture-session."+std::to_string(++sequence);strcpy(buffer,id.c_str());return true;
 }
 int orderInsert(WTSEntrust* order)override{
  order->retain();inserted.push_back(order);return rejectSynchronously?-1:0;
 }
 int orderAction(WTSEntrustAction* action)override{
  assert(std::string(action->getOrderID())=="order-1");++cancels;return 0;
 }
};

static std::vector<rapidjson::Document> records(){
 std::ifstream stream("generated/traders/quality/execution-quality.jsonl");assert(stream.good());
 std::vector<rapidjson::Document> rows;std::string line;
 while(std::getline(stream,line)){
  rapidjson::Document value;value.Parse(line.c_str());assert(!value.HasParseError());
  rows.emplace_back(std::move(value));
 }
 return rows;
}

int main(int argc,char** argv){
 assert(argc==2);
 const auto run=std::filesystem::path(argv[1])/std::to_string(ExecutionStamp::now().wall);
 std::filesystem::create_directories(run);std::filesystem::current_path(run);
 WtHelper::setGenerateDir("generated");
 WTSLogger::init(R"({"root":{"level":"error","sinks":[{"type":"ostream_sink","pattern":"%v"}]}})",false);
 Data data;auto broker=new Broker;TraderAdapter adapter;
 assert(adapter.initExt("quality",broker,&data,nullptr));assert(adapter.run());
 adapter.onLoginResult(true,"ok",20261026);
 auto empty=WTSArray::create();adapter.onRspPosition(empty);adapter.onRspOrders(empty);
 adapter.onRspTrades(empty);adapter.onRspAccount(empty);empty->release();assert(adapter.isReady());
 ExecutionQuote quote{2999,3000,2999,1792980000123};
 adapter.setExecutionQuoteReader([&](const char* code){assert(std::string(code)=="SHFE.rb.2701");return quote;});
 // openLong drives exactly the same doEntrust send path as a buy instruction.
 const auto local=adapter.openLong("SHFE.rb.2701",3001,3,0,data.contract);assert(local!=UINT_MAX);
 auto entrust=broker->inserted.back();const std::string entrustId=entrust->getEntrustID();
 assert(std::string(entrust->getUserTag())=="otp.quality."+std::to_string(local));
 auto order=WTSOrderInfo::create(entrust);order->setContractInfo(data.contract);
 order->setOrderID("order-1");order->setOrderDate(20261026);order->setOrderTime(1792980000000);
 order->setOrderState(WOS_NotTraded_Queuing);order->setVolTraded(0);order->setVolLeft(3);
 adapter.onPushOrder(order);
 // Later quotes must not rewrite the frozen submission benchmark.
 quote.ask=3100;quote.bid=3099;quote.updated_at+=1000;
 auto fill=WTSTradeInfo::create("rb2701","SHFE");fill->setContractInfo(data.contract);
 fill->setDirection(WDT_LONG);fill->setOffsetType(WOT_OPEN);fill->setVolume(1);fill->setPrice(3001);
 fill->setTradeID("trade-1");fill->setRefOrder("order-1");fill->setUserTag(entrust->getUserTag());
 fill->setTradeDate(20261026);fill->setTradeTime(1792980001000);adapter.onPushTrade(fill);fill->release();
 order->setOrderState(WOS_PartTraded_Queuing);order->setVolTraded(1);order->setVolLeft(2);
 adapter.onPushOrder(order);assert(adapter.cancel(local));assert(broker->cancels==1);
 order->setOrderState(WOS_Canceled);adapter.onPushOrder(order);order->release();
 // Broker rejection and synchronous API failure retain their submission IDs.
 const auto rejected=adapter.openLong("SHFE.rb.2701",3001,1,0,data.contract);assert(rejected!=UINT_MAX);
 auto error=WTSError::create(WEC_ORDERINSERT,"synthetic rejection");
 adapter.onRspEntrust(broker->inserted.back(),error);error->release();
 quote={}; // A missing quote must stay missing in the final rejected submission.
 broker->rejectSynchronously=true;
 assert(adapter.openLong("SHFE.rb.2701",3001,1,0,data.contract)==UINT_MAX);
 const auto rows=records();assert(rows.size()>=8);
 int submissions=0,rejections=0,fills=0,cancelRequests=0,partial=0,canceled=0;
 std::string clock;uint64_t sendMono=0,fillMono=0;
 for(const auto& row:rows){
  assert(row["schema"].GetInt()==1);assert(std::string(row["channel"].GetString())=="quality");
  assert(row["trading_date"].GetUint()==20261026);
  assert(row["wall_time_ns"].IsUint64()&&row["wall_time_ns"].GetUint64()>1000000000000000000ULL);
  assert(row["mono_time_ns"].IsUint64()&&row["mono_time_ns"].GetUint64()>0);
  if(clock.empty())clock=row["clock_id"].GetString();assert(!clock.empty()&&clock==row["clock_id"].GetString());
  const std::string kind=row["event"].GetString();
  assert(std::string(row["code"].GetString())=="SHFE.rb.2701");
  if(kind=="submission"){
   ++submissions;assert(std::string(row["side"].GetString())=="buy");
   if(submissions==3){assert(!row.HasMember("quote"));assert(!row.HasMember("benchmark_kind"));}
   if(row["localid"].GetUint()==local){
    assert(std::string(row["entrust_id"].GetString())==entrustId);assert(row["qty"].GetDouble()==3);
    assert(row["quote"]["ask"].GetDouble()==3000);assert(row["quote"]["bid"].GetDouble()==2999);
    assert(row["quote"]["updated_at"].GetUint64()==1792980000123);
    assert(std::string(row["benchmark_kind"].GetString())=="opposite_touch");sendMono=row["mono_time_ns"].GetUint64();
   }
  }else if(kind=="fill"){
   ++fills;assert(row["localid"].GetUint()==local);assert(std::string(row["orderid"].GetString())=="order-1");
   assert(std::string(row["tradeid"].GetString())=="trade-1");assert(row["qty"].GetDouble()==1);
   assert(row["price"].GetDouble()==3001);fillMono=row["mono_time_ns"].GetUint64();
  }else if(kind=="order_update"){
   assert(row["localid"].GetUint()==local);assert(std::string(row["entrust_id"].GetString())==entrustId);
   assert(std::string(row["orderid"].GetString())=="order-1");
   if(row["filled_qty"].GetDouble()==1&&row["left_qty"].GetDouble()==2)++partial;
   if(row["canceled"].GetBool())++canceled;
  }else if(kind=="cancel_request"){
   ++cancelRequests;assert(row["localid"].GetUint()==local);assert(std::string(row["orderid"].GetString())=="order-1");
  }else if(kind=="reject"){
   ++rejections;assert(row["localid"].GetUint()!=local);assert(!std::string(row["entrust_id"].GetString()).empty());
  }
 }
 assert(submissions==3&&rejections==2&&fills==1&&cancelRequests==1&&partial==2&&canceled==1);
 assert(fillMono>=sendMono&&sendMono>0);
 adapter.release();WTSLogger::stop();
 std::cout<<"native submission benchmark, partial fill, cancel, rejected orders and clocks passed\n";
}
