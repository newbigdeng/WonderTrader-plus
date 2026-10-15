// Read-only audit using the official CTP SDK. There is no order insertion API call.
#include "ThostFtdcTraderApi.h"
#include <dlfcn.h>
#include <map>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <cstring>
#include <cmath>
#include <vector>

template<class T> void setstr(T& target,const std::string& value) {
    std::memset(target,0,sizeof(target));std::strncpy(target,value.c_str(),sizeof(target)-1);
}
std::string trim(std::string s) {
    auto a=s.find_first_not_of(" \t\r\n");
    return a==std::string::npos?"":s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
std::map<std::string,std::string> config(const char* path) {
    std::ifstream f(path);std::string line,section;std::map<std::string,std::string> out;
    while(std::getline(f,line)) {
        line=trim(line);if(line.empty()||line[0]==';'||line[0]=='#')continue;
        if(line[0]=='['){section=line;continue;}
        auto n=line.find('=');if(section=="[ctp]"&&n!=std::string::npos)out[trim(line.substr(0,n))]=trim(line.substr(n+1));
    }return out;
}
class Audit:public CThostFtdcTraderSpi {
public:
    CThostFtdcTraderApi* api=nullptr;
    std::map<std::string,std::string> cfg;
    std::mutex mutex;std::condition_variable cv;
    bool finished=false,ok=false,fund_seen=false;int req=0,error=0,active=0,orders=0,position_total=0;
    double available=0,balance=0,margin=0,commission=0;
    std::string stage="connecting",day;
    std::vector<std::string> posrows,orderrows;
    void done(bool success){std::lock_guard<std::mutex> lock(mutex);ok=success;finished=true;cv.notify_all();}
    bool bad(CThostFtdcRspInfoField* rsp){if(rsp&&rsp->ErrorID){error=rsp->ErrorID;done(false);return true;}return false;}
    void sent(int result){if(result){error=result;done(false);}}
    void OnFrontConnected() override {
        stage="authentication";CThostFtdcReqAuthenticateField x{};
        setstr(x.BrokerID,cfg["broker"]);setstr(x.UserID,cfg["user"]);setstr(x.AppID,cfg["appid"]);setstr(x.AuthCode,cfg["authcode"]);
        sent(api->ReqAuthenticate(&x,++req));
    }
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField*,CThostFtdcRspInfoField* rsp,int,bool last) override {
        if(bad(rsp)||!last)return;stage="login";CThostFtdcReqUserLoginField x{};
        setstr(x.BrokerID,cfg["broker"]);setstr(x.UserID,cfg["user"]);setstr(x.Password,cfg["pass"]);
        sent(api->ReqUserLogin(&x,++req));
    }
    void OnRspUserLogin(CThostFtdcRspUserLoginField* x,CThostFtdcRspInfoField* rsp,int,bool last) override {
        if(bad(rsp)||!last||!x)return;day=x->TradingDay;stage="settlement_confirmation";
        CThostFtdcSettlementInfoConfirmField q{};setstr(q.BrokerID,cfg["broker"]);setstr(q.InvestorID,cfg["user"]);
        sent(api->ReqSettlementInfoConfirm(&q,++req));
    }
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*,CThostFtdcRspInfoField* rsp,int,bool last) override {
        if(bad(rsp)||!last)return;stage="positions";CThostFtdcQryInvestorPositionField q{};
        setstr(q.BrokerID,cfg["broker"]);setstr(q.InvestorID,cfg["user"]);sent(api->ReqQryInvestorPosition(&q,++req));
    }
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* x,CThostFtdcRspInfoField* rsp,int,bool last) override {
        if(bad(rsp))return;
        if(x&&x->Position>0){position_total+=x->Position;std::ostringstream a;a<<"{\"instrument\":\""<<x->InstrumentID<<"\",\"exchange\":\""<<x->ExchangeID<<"\",\"direction\":\""<<x->PosiDirection<<"\",\"position_date\":\""<<x->PositionDate<<"\",\"quantity\":"<<x->Position<<"}";posrows.push_back(a.str());}
        if(!last)return;std::this_thread::sleep_for(std::chrono::seconds(1));stage="orders";
        CThostFtdcQryOrderField q{};setstr(q.BrokerID,cfg["broker"]);setstr(q.InvestorID,cfg["user"]);sent(api->ReqQryOrder(&q,++req));
    }
    void OnRspQryOrder(CThostFtdcOrderField* x,CThostFtdcRspInfoField* rsp,int,bool last) override {
        if(bad(rsp))return;
        if(x){++orders;char s=x->OrderStatus;bool terminal=s=='0'||s=='2'||s=='4'||s=='5';if(!terminal)++active;
          std::ostringstream a;a<<"{\"instrument\":\""<<x->InstrumentID<<"\",\"exchange\":\""<<x->ExchangeID<<"\",\"status\":\""<<s<<"\",\"original_volume\":"<<x->VolumeTotalOriginal<<",\"traded\":"<<x->VolumeTraded<<",\"remaining\":"<<x->VolumeTotal<<",\"active_or_uncertain\":"<<(!terminal?"true":"false")<<"}";orderrows.push_back(a.str());}
        if(!last)return;std::this_thread::sleep_for(std::chrono::seconds(1));stage="funds";
        CThostFtdcQryTradingAccountField q{};setstr(q.BrokerID,cfg["broker"]);setstr(q.InvestorID,cfg["user"]);sent(api->ReqQryTradingAccount(&q,++req));
    }
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* x,CThostFtdcRspInfoField* rsp,int,bool last) override {
        if(bad(rsp))return;
        if(x&&std::string(x->CurrencyID)=="CNY"){fund_seen=true;available=x->Available;balance=x->Balance;margin=x->CurrMargin;commission=x->Commission;}
        if(last){stage="complete";done(fund_seen&&std::isfinite(available));}
    }
    void output(const char* path){std::ofstream f(path);f.precision(17);
      f<<"{\"result\":\""<<(ok?"passed":"failed")<<"\",\"stage\":\""<<stage<<"\",\"error_id\":"<<error<<",\"trading_day\":\""<<day<<"\",\"positions_total\":"<<position_total<<",\"active_or_uncertain_orders\":"<<active<<",\"order_count\":"<<orders<<",\"available\":"<<available<<",\"balance\":"<<balance<<",\"margin\":"<<margin<<",\"commission\":"<<commission<<",\"positions\":[";
      for(size_t i=0;i<posrows.size();++i)f<<(i?",":"")<<posrows[i];f<<"],\"orders\":[";
      for(size_t i=0;i<orderrows.size();++i)f<<(i?",":"")<<orderrows[i];f<<"]}\n";
    }
};
int main(int argc,char** argv){
    if(argc!=5)return 2;Audit a;a.cfg=config(argv[1]);
    if(a.cfg["front"]!="tcp://182.254.243.31:30003"||a.cfg["broker"]!="9999")return 3;
    void* library=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);if(!library){std::cerr<<dlerror()<<"\n";return 4;}
    using Creator=CThostFtdcTraderApi*(*)(const char*,bool);
    auto create=(Creator)dlsym(library,"_ZN19CThostFtdcTraderApi19CreateFtdcTraderApiEPKcb");if(!create)return 5;
    a.api=create(argv[3],true);if(!a.api)return 6;
    a.api->RegisterSpi(&a);a.api->SubscribePrivateTopic(THOST_TERT_QUICK);a.api->SubscribePublicTopic(THOST_TERT_QUICK);
    a.api->RegisterFront(const_cast<char*>(a.cfg["front"].c_str()));a.api->Init();
    {std::unique_lock<std::mutex> lock(a.mutex);if(!a.cv.wait_for(lock,std::chrono::seconds(50),[&]{return a.finished;})){a.stage="timeout";a.error=-999;}}
    a.api->RegisterSpi(nullptr);a.api->Release();a.output(argv[4]);
    std::cout<<"AUDIT "<<(a.ok?"passed":"failed")<<" trading_day="<<a.day<<" positions="<<a.position_total<<" active_or_uncertain_orders="<<a.active<<"\n";
    return a.ok?0:1;
}
