// Drive CTP callbacks directly: no SDK instance, credentials or network.
#include "TraderCTP/TraderCTP.h"
#include "Includes/IBaseDataMgr.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace wtp;
struct MissingContracts : IBaseDataMgr {
 WTSCommodityInfo* getCommodity(const char*)override{return nullptr;}
 WTSCommodityInfo* getCommodity(const char*,const char*)override{return nullptr;}
 WTSContractInfo* getContract(const char*,const char* = "",uint32_t=0)override{return nullptr;}
 WTSArray* getContracts(const char* = "",uint32_t=0)override{return nullptr;}
 WTSSessionInfo* getSession(const char*)override{return nullptr;}
 WTSSessionInfo* getSessionByCode(const char*,const char* = "")override{return nullptr;}
 WTSArray* getAllSessions()override{return nullptr;}
 bool isHoliday(const char*,uint32_t,bool=false)override{return false;}
 uint32_t calcTradingDate(const char*,uint32_t day,uint32_t,bool=false)override{return day;}
 uint64_t getBoundaryTime(const char*,uint32_t,bool=false,bool=true)override{return 0;}
};
struct Results : ITraderSpi {
 MissingContracts base;
 int count=0;bool valid=false;uint32_t rows=0;
 IBaseDataMgr* getBaseDataMgr()override{return &base;}
 void handleEvent(WTSTraderEvent,int32_t)override{}
 void onLoginResult(bool,const char*,uint32_t)override{}
 void receive(const WTSArray* result){++count;valid=result!=nullptr;rows=result?result->size():0;}
 void onRspOrders(const WTSArray* result)override{receive(result);}
 void onRspPosition(const WTSArray* result)override{receive(result);}
};
int main(){
 CThostFtdcRspInfoField error{};error.ErrorID=7;
 for(bool positions:{false,true}) for(int errorStage:{0,1,2}) {
  Results result;TraderCTP trader;trader.registerSpi(&result);
  auto callback=[&](CThostFtdcRspInfoField* info,bool last){
   if(positions)trader.OnRspQryInvestorPosition(nullptr,info,1,last);
   else trader.OnRspQryOrder(nullptr,info,1,last);
  };
  // A failure in an earlier fragment must survive a successful final fragment.
  if(errorStage==1){callback(&error,false);assert(result.count==0);}
  callback(errorStage==2?&error:nullptr,true);
  assert(result.count==1&&result.rows==0);assert(result.valid==(errorStage==0));
 }
 // An unknown zero/finished row is harmless; unknown exposure must not look flat.
 for(bool held:{false,true}) {
  Results result;TraderCTP trader;trader.registerSpi(&result);
  CThostFtdcInvestorPositionField position{};strcpy(position.InstrumentID,"unknown2701");
  strcpy(position.ExchangeID,"SHFE");position.Position=held?1:0;
  trader.OnRspQryInvestorPosition(&position,nullptr,2,false);assert(result.count==0);
  trader.OnRspQryInvestorPosition(nullptr,nullptr,2,true);
  assert(result.count==1&&result.rows==0&&result.valid==!held);
 }
 for(bool active:{false,true}) {
  Results result;TraderCTP trader;trader.registerSpi(&result);
  CThostFtdcOrderField order{};strcpy(order.InstrumentID,"unknown2701");strcpy(order.ExchangeID,"SHFE");
  order.OrderStatus=active?THOST_FTDC_OST_NoTradeQueueing:THOST_FTDC_OST_AllTraded;
  trader.OnRspQryOrder(&order,nullptr,3,false);assert(result.count==0);
  trader.OnRspQryOrder(nullptr,nullptr,3,true);
  assert(result.count==1&&result.rows==0&&result.valid==!active);
 }
 std::cout<<"CTP queries: successful empty arrays and final/fragment errors distinguished\n";
}
