/*!
 * \file WtCtaEngine.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include <mutex>
#include <vector>
#include <atomic>
#include "../Includes/ICtaStraCtx.h"
#include "../Share/threadpool.hpp"
#include "WtExecMgr.h"
#include "WtEngine.h"

NS_WTP_BEGIN
class WTSVariant;
typedef std::shared_ptr<ICtaStraCtx> CtaContextPtr;

class WtCtaRtTicker;

class WtCtaEngine : public WtEngine, public IExecuterStub
{
public:
    bool request_strategy_reset(const char* strategy, const char* trader, const char* request, const char* contracts, bool query_only = false, const char* metadata = nullptr);
    bool configure_strategy_reconcile(const char* boot, const char* revision);
    bool request_strategy_reconcile(const char* request_json);
    bool arm_strategy_startup_guard(const char* nonce);
    bool release_strategy_startup_guard(const char* nonce);
    const char* query_strategy_startup_guard();
    const char* query_strategy_reset(const char* request);
    bool wakeup_strategy_reset(const char* request);
    void process_strategy_resets();
    bool strategy_reset_pending() const { return _reset_pending; }
    const char* query_portfolio_state(const char* traderid, const char* strategyid);

	WtCtaEngine();
	virtual ~WtCtaEngine();

public:
	//////////////////////////////////////////////////////////////////////////
	//WtEngine接口
	virtual void handle_push_quote(WTSTickData* newTick) override;

	virtual void on_tick(const char* stdCode, WTSTickData* curTick) override;

	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	virtual void on_init() override;
	virtual void on_session_begin() override;
	virtual void on_session_end() override;

	virtual void run() override;
	virtual void stop() override;

	virtual void init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier) override;

	virtual bool isInTrading() override;
	virtual uint32_t transTimeToMin(uint32_t uTime) override;

	///////////////////////////////////////////////////////////////////////////
	//IExecuterStub 接口
	virtual uint64_t get_real_time() override;
	virtual WTSCommodityInfo* get_comm_info(const char* stdCode) override;
	virtual WTSSessionInfo* get_sess_info(const char* stdCode) override;
	virtual IHotMgr* get_hot_mon() { return _hot_mgr; }
	virtual uint32_t get_trading_day() { return _cur_tdate; }


public:
	void on_schedule(uint32_t curDate, uint32_t curTime);	

	void handle_pos_change(const char* straName, const char* stdCode, double diffPos);

	void addContext(CtaContextPtr ctx);
	
	CtaContextPtr	getContext(uint32_t id);

	inline void addExecuter(ExecCmdPtr executer)
	{
		_exec_mgr.add_executer(executer);
		executer->setStub(this);
	}

	inline bool loadRouterRules(WTSVariant* cfg)
	{
		return _exec_mgr.load_router_rules(cfg);
	}

public:
	void notify_chart_marker(uint64_t time, const char* straId, double price, const char* icon, const char* tag);
	void notify_chart_index(uint64_t time, const char* straId, const char* idxName, const char* lineName, double val);
	void notify_trade(const char* straId, const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, const char* userTag);

private:
    friend struct NativePortfolioTestAccessor;
    struct ResetJob {
        std::string strategy, trader, request, status = "running", step = "queued", message, error, archive;
        std::vector<std::string> contracts;
        uint64_t started = 0, completed = 0;
        CtaContextPtr context;
        std::shared_ptr<WtLocalExecuter> executor;
        bool begun = false, init_requested = false, init_dispatched = false;
        bool query_only = false, adapter_begun = false, previous_context_frozen = false, previous_executor_frozen = false;
        std::string metadata, snapshot;
    };
    std::recursive_mutex _reset_events;
    std::mutex _reset_jobs_mutex;
    std::vector<std::shared_ptr<ResetJob>> _reset_jobs;
    std::atomic<bool> _reset_pending{false};
    std::string _reconcile_boot, _reconcile_revision;
    std::string _startup_nonce;
    bool _startup_guard = false;

	typedef wt_hashmap<uint32_t, CtaContextPtr> ContextMap;
	ContextMap		_ctx_map;

	WtCtaRtTicker*	_tm_ticker;

	WtExecuterMgr	_exec_mgr;

	WTSVariant*		_cfg;

	typedef std::shared_ptr<boost::threadpool::pool> ThreadPoolPtr;
	ThreadPoolPtr		_pool;
};

NS_WTP_END

