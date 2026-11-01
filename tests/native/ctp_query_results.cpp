// Drive CTP callbacks directly: no SDK instance, credentials or network.
#include "TraderCTP/TraderCTP.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace wtp;
struct MissingContracts : IBaseDataMgr {
 WTSContractInfo* known=nullptr;
 WTSCommodityInfo* getCommodity(const char*)override{return nullptr;}
 WTSCommodityInfo* getCommodity(const char*,const char*)override{return nullptr;}
 WTSContractInfo* getContract(const char*,const char* = "",uint32_t=0)override{return known;}
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
 double today=0,prior=0,freeToday=0,freePrior=0;
 void onRspPosition(const WTSArray* result)override{
  receive(result);
  if(rows){auto* p=static_cast<WTSPositionItem*>(*result->begin());
   today=p->getNewPosition();prior=p->getPrePosition();
   freeToday=p->getAvailNewPos();freePrior=p->getAvailPrePos();}
 }
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
 // Multiple hedge buckets describe real exposure but cannot all fund speculative closes.
 for (bool split:{false,true}) {
  Results result;TraderCTP trader;trader.registerSpi(&result);
  auto* product=WTSCommodityInfo::create("test","fixture","DCE","DAY","CHINA");
  product->setVolScale(10);product->setCoverMode(split?CM_CoverToday:CM_OpenCover);
  product->setCategory(split?CC_Future:CC_FutOption);
  auto* contract=WTSContractInfo::create("fixture","fixture","DCE","test");
  contract->setCommInfo(product);result.base.known=contract;
  CThostFtdcInvestorPositionField row{};strcpy(row.InstrumentID,"fixture");
  row.PosiDirection=THOST_FTDC_PD_Long;row.HedgeFlag=THOST_FTDC_HF_Speculation;
  row.PositionDate=THOST_FTDC_PSD_Today;
  row.Position=split?3:8;row.TodayPosition=3;row.ShortFrozen=1;
  if(!split){row.StrikeFrozen=2;row.YdStrikeFrozen=1;row.AbandonFrozen=1;}
  trader.OnRspQryInvestorPosition(&row,nullptr,1,false);
  if(split){row.PositionDate=THOST_FTDC_PSD_History;row.Position=5;row.TodayPosition=0;
   row.ShortFrozen=2;trader.OnRspQryInvestorPosition(&row,nullptr,1,false);}
  row.HedgeFlag=THOST_FTDC_HF_Hedge;row.PositionDate=THOST_FTDC_PSD_Today;
  row.Position=row.TodayPosition=2;trader.OnRspQryInvestorPosition(&row,nullptr,1,true);
  assert(result.valid && result.rows==1 && result.today==5 && result.prior==5);
  assert(result.freeToday==(split?2:0) && result.freePrior==(split?3:4));
  contract->release();product->release();
 }
 std::cout<<"CTP queries: successful empty arrays and final/fragment errors distinguished\n";
}
