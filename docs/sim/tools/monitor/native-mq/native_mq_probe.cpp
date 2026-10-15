// Controlled native notifier test. No parser, trader, account, or order API exists here.
#include "EventNotifier.h"
#include "WtHelper.h"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../WTSTools/WTSLogger.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
USING_NS_WTP;

int main(int argc, char** argv) {
    if (argc != 4) return 2; // IPC URL, native module directory, append-only command file
    WtHelper::setInstDir(argv[2]);
    WTSLogger::init(R"({"root":{"level":"info","sinks":[{"type":"ostream_sink","pattern":"%v"}]}})",false);
    std::vector<WTSOrderInfo*> orders;
    {
        EventNotifier notifier;
        WTSVariant* cfg=WTSVariant::createObject();
        cfg->append("active",true);cfg->append("url",argv[1]);
        if(!notifier.init(cfg)) return 3;
        cfg->release();
        std::ifstream commands(argv[3]);
        std::string line;
        bool done=false;
        for(int i=0;i<18000 && !done;++i) {
            commands.clear();
            while(std::getline(commands,line)) {
                if(line=="QUIT") {done=true;break;}
                if(line.compare(0,4,"LOG ")==0) notifier.notify_log("info",line.substr(4).c_str());
                else if(line=="ORDER" || line=="CANCEL") {
                    auto entrust=WTSEntrust::create("0001",1,100,"TEST");
                    entrust->setDirection(WDT_LONG);entrust->setOffsetType(WOT_OPEN);
                    auto order=WTSOrderInfo::create(entrust);entrust->release();
                    order->setOrderState(line=="CANCEL"?WOS_Canceled:WOS_Submitting);
                    order->setVolTraded(0);order->setVolLeft(1);
                    order->setStateMsg(line=="CANCEL"?"CONTROLLED_TEST_CANCELED":"CONTROLLED_TEST_SUBMITTED");
                    notifier.notify("mq_probe",line=="CANCEL"?990002:990001,"TEST.MQ.0001",order);
                    orders.push_back(order);
                } else if(line=="TRADE") {
                    auto trade=WTSTradeInfo::create("0001","TEST");
                    trade->setDirection(WDT_LONG);trade->setOffsetType(WOT_OPEN);
                    trade->setVolume(1);trade->setPrice(100);
                    notifier.notify("mq_probe",990003,"TEST.MQ.0001",trade);
                    trade->release();
                }
                std::cout<<"COMMAND "<<line<<std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        for(auto order:orders) {
            std::cout<<"ORDER_REFS "<<order->retainCount()<<std::endl;
            order->release();
        }
    } // Exercise idle guarded-worker shutdown, not just process termination.
    std::cout<<"NOTIFIER_DESTROYED"<<std::endl;
    WTSLogger::stop();
    return 0;
}
