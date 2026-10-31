/*!
 * \file TraderAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include <mutex>
#include <boost/noncopyable.hpp>
#include <memory>
#include <functional>
#include <vector>
#include <set>
#include <atomic>

#include "../Includes/ExecuteDefs.h"
#include "../Includes/FasterDefs.h"
#include "../Includes/ITraderApi.h"
#include "../Share/BoostFile.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/SpinMutex.hpp"
#include "ExecutionRecorder.hpp"
#include "PortfolioState.hpp"

NS_WTP_BEGIN
class WTSVariant;
class ActionPolicyMgr;
class WTSContractInfo;
class WTSCommodityInfo;
class WtLocalExecuter;
class EventNotifier;

class ITrdNotifySink;

typedef std::function<void(const char*, bool, double, double, double, double)> FuncEnumChnlPosCallBack;

class TraderAdapter : public ITraderSpi
{
public:
	TraderAdapter(EventNotifier* caster = NULL);
	~TraderAdapter();

	typedef enum tagAdapterState
	{
		AS_NOTLOGIN,		//未登录
		AS_LOGINING,		//正在登录
		AS_LOGINED,			//已登录
		AS_LOGINFAILED,		//登录失败
		AS_POSITION_QRYED,	//仓位已查
		AS_ORDERS_QRYED,	//订单已查
		AS_TRADES_QRYED,	//成交已查
		AS_ALLREADY			//全部就绪
	} AdapterState;

	struct PositionMetrics
	{
		double avg_price, position_cost, dynprofit, open_price, margin;
		uint64_t updated_at;
		bool valid;
	};

	typedef struct _PosItem
	{
		//多仓数据
		double	l_newvol;
		double	l_newavail;
		double	l_prevol;
		double	l_preavail;

		//空仓数据
		double	s_newvol;
		double	s_newavail;
		double	s_prevol;
		double	s_preavail;
		PositionMetrics l_metrics, s_metrics;

		_PosItem()
		{
			memset(this, 0, sizeof(_PosItem));
		}

		double total_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newvol + l_prevol;
			else
				return s_newvol + s_prevol;
		}

		double avail_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newavail + l_preavail;
			else
				return s_newavail + s_preavail;
		}

	} PosItem;

	typedef struct _RiskParams
	{
		uint32_t	_order_times_boundary;
		uint32_t	_order_stat_timespan;
		uint32_t	_order_total_limits;

		uint32_t	_cancel_times_boundary;
		uint32_t	_cancel_stat_timespan;
		uint32_t	_cancel_total_limits;

		_RiskParams()
		{
			memset(this, 0, sizeof(_RiskParams));
		}
	} RiskParams;

public:
	bool init(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);
	bool initExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

	void release();

	bool run();

	inline const char* id() const{ return _id.c_str(); }

	AdapterState state() const{ return _state; }

	void addSink(ITrdNotifySink* sink)
	{
		_sinks.insert(sink);
	}

	inline bool isReady() const { return _state == AS_ALLREADY; }
	void setExecutionQuoteReader(const ExecutionRecorder::QuoteReader& reader) { _execution.setQuoteReader(reader); }

	void queryFund();
    bool portfolio_snapshot(PortfolioView& result);
    bool begin_strategy_reset(const std::string& request, const std::vector<std::string>& contracts, std::string& error, bool query_only = false);
    std::string advance_strategy_reset(uint64_t now, const std::function<double(const std::string&, bool)>& quote, std::string& error);
    bool finish_strategy_reset(std::string* error = nullptr);
    void fail_strategy_reset();
    void reset_position_response(const WTSArray* positions);
    bool reset_order_response(const WTSArray* orders);
    void reset_trade_event();
    void reset_entrust_error(WTSError* error);
    std::string reconcile_snapshot(bool execution_idle);
    void retain_reconcile_proof(const std::string& json);
    void finish_reconcile_release(bool previously_ready);
    uint64_t reconcile_event_seq() { std::lock_guard<std::recursive_mutex> lock(_reset_gate); return _reset_epoch; }
    void startup_guard(bool active) { std::lock_guard<std::recursive_mutex> lock(_reset_gate); _startup_guard = active; }
    bool startup_release_ready(uint64_t epoch) {
        std::lock_guard<std::recursive_mutex> lock(_reset_gate);
        return _startup_guard && !_reset_blocked && !_reset_active && isReady() &&
            _pending_submissions.empty() && _reset_epoch == epoch;
    }
    bool exclusive_reset_sink(ITrdNotifySink* sink) const { return _sinks.size() == 1 && _sinks.count(sink); }


private:
    friend struct NativePortfolioTestAccessor;
    PortfolioState _portfolio_state;
    void portfolio_status(bool connected, uint32_t trading_date = 0);
    void portfolio_positions(const WTSArray* positions);
    void portfolio_orders(const WTSArray* orders);
    void portfolio_order(WTSOrderInfo* order);
    void portfolio_reset_complete(bool complete);
	uint64_t replayTime() const;
	uint64_t (*_replay_clock)(uint32_t) = nullptr;
	uint32_t _replay_clock_id = 0;
	uint32_t doEntrust(WTSEntrust* entrust);
	bool	doCancel(WTSOrderInfo* ordInfo);

	inline void	printPosition(const char* stdCode, const PosItem& pItem);

	inline WTSContractInfo* getContract(const char* stdCode);
	inline WTSCommodityInfo* getCommodify(const char* stdCommID);

	const RiskParams* getRiskParams(const char* stdCode);

	void initSaveData();

	inline void	logTrade(uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo);
	inline void	logOrder(uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo);

	void	saveData(WTSArray* ayFunds = NULL);

	void updateUndone(const char* stdCode, double qty, bool bOuput = true);

public:
	double getPosition(const char* stdCode, bool bValidOnly, int32_t flag = 3);
	OrderMap* getOrders(const char* stdCode);
	double getUndoneQty(const char* stdCode)
	{
		auto it = _undone_qty.find(stdCode);
		if (it != _undone_qty.end())
			return it->second;

		return 0;
	}

	void enumPosition(FuncEnumChnlPosCallBack cb);

	uint32_t openLong(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo = NULL);
	uint32_t openShort(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo = NULL);
	uint32_t closeLong(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo = NULL);
	uint32_t closeShort(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo = NULL);
	
	OrderIDs buy(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);
	OrderIDs sell(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);
	bool	cancel(uint32_t localid);
	OrderIDs cancel(const char* stdCode, bool isBuy, double qty = 0);

	inline bool	isTradeEnabled(const char* stdCode) const;

	bool	checkCancelLimits(const char* stdCode);
	bool	checkOrderLimits(const char* stdCode);

	bool	checkSelfMatch(const char* stdCode, WTSTradeInfo* tInfo);

	inline	bool isSelfMatched(const char* stdCode)
	{
		//如果忽略自成交，则直接返回false
		if (_ignore_sefmatch)
			return false;

		auto it = _self_matches.find(stdCode);
		return it != _self_matches.end();
	}

public:
	//////////////////////////////////////////////////////////////////////////
	//ITraderSpi接口
	virtual void handleEvent(WTSTraderEvent e, int32_t ec) override;

	virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override;

	virtual void onLogout() override;

	virtual void onRspEntrust(WTSEntrust* entrust, WTSError *err) override;

	virtual void onRspAccount(WTSArray* ayAccounts) override;

	virtual void onRspPosition(const WTSArray* ayPositions) override;

	virtual void onRspOrders(const WTSArray* ayOrders) override;

	virtual void onRspTrades(const WTSArray* ayTrades) override;

	virtual void onPushOrder(WTSOrderInfo* orderInfo) override;

	virtual void onPushTrade(WTSTradeInfo* tradeRecord) override;

	virtual void onTraderError(WTSError* err, void* pData = NULL) override;

	virtual IBaseDataMgr* getBaseDataMgr() override;

	virtual void handleTraderLog(WTSLogLevel ll, const char* message) override;

private:
	WTSVariant*			_cfg;
	std::string			_id;
	std::string			_order_pattern;

	uint32_t			_trading_day;

	ITraderApi*			_trader_api;
    std::recursive_mutex _reset_gate;
    bool _reset_blocked = false, _reset_sending = false, _reset_active = false;
    bool _reset_query_only = false, _reset_previous_blocked = false;
    bool _startup_guard = false;
    bool _reconcile_releasing = false, _reconcile_connected = false;
    std::string _reconcile_proof;
    uint64_t _reconcile_proof_epoch = 0;
    std::set<uint32_t> _pending_submissions;
    bool _reset_orders_ready = false, _reset_positions_ready = false, _reset_wait_positions = false;
    bool _reset_wait_orders = false, _reset_drain_orders = false, _reset_drain_positions = false;
    int _reset_stage = 0;
    uint64_t _reset_epoch = 0, _reset_query_epoch = 0, _reset_delay = 0;
    std::vector<std::string> _reset_contracts;
    std::vector<std::shared_ptr<WTSOrderInfo>> _reset_orders;
    struct ResetPosition { std::string code; bool isLong; double previous, today, previous_available, today_available; };
    std::vector<ResetPosition> _reset_positions;
    std::string _reset_error;

	FuncDeleteTrader	_remover;
	std::atomic<AdapterState> _state;

	EventNotifier*		_notifier;

	wt_hashset<ITrdNotifySink*>	_sinks;

	IBaseDataMgr*		_bd_mgr;
	ActionPolicyMgr*	_policy_mgr;

	wt_hashmap<std::string, PosItem> _positions;
	uint64_t _position_revision = 0, _metrics_query_revision = 0;
	bool _metrics_query_pending = false;

	SpinMutex	_mtx_orders;
	OrderMap*	_orders;
	wt_hashset<std::string> _orderids;	//主要用于标记有没有处理过该订单

	wt_hashmap<std::string, std::string>		_trade_refs;	//用于记录成交单和订单的匹配
	wt_hashset<std::string>					_self_matches;	//自成交的合约

	/*
	 *	By Wesley @ 2023.03.16
	 *	加一个控制，这样自成交发生以后，还可以恢复交易
	 */
	bool			_ignore_sefmatch;		//忽略自成交限制

	wt_hashmap<std::string, double> _undone_qty;	//未完成数量

	typedef WTSHashMap<std::string>	TradeStatMap;
	TradeStatMap*	_stat_map;	//统计数据

	//这两个缓存时间内的容器,主要是为了控制瞬间流量而设置的
	typedef std::vector<uint64_t> TimeCacheList;
	typedef wt_hashmap<std::string, TimeCacheList> CodeTimeCacheMap;
	CodeTimeCacheMap	_order_time_cache;	//下单时间缓存
	CodeTimeCacheMap	_cancel_time_cache;	//撤单时间缓存

	//如果被风控了,就会进入到排除队列
	wt_hashset<std::string>	_exclude_codes;
	
	typedef wt_hashmap<std::string, RiskParams>	RiskParamsMap;
	RiskParamsMap	_risk_params_map;
	bool			_risk_mon_enabled;

	bool			_save_data;	//是否保存交易日志
	BoostFilePtr	_trades_log;		//交易数据日志
	BoostFilePtr	_orders_log;		//订单数据日志
	std::string		_rt_data_file;		//实时数据文件
	ExecutionRecorder _execution;
};

typedef std::shared_ptr<TraderAdapter>				TraderAdapterPtr;
typedef wt_hashmap<std::string, TraderAdapterPtr>	TraderAdapterMap;


//////////////////////////////////////////////////////////////////////////
//TraderAdapterMgr
class TraderAdapterMgr : private boost::noncopyable
{
public:
	void	release();

	void	run();

	const TraderAdapterMap& getAdapters() const { return _adapters; }

	TraderAdapterPtr getAdapter(const char* tname);

	bool	addAdapter(const char* tname, TraderAdapterPtr& adapter);

	void	refresh_funds();

private:
	TraderAdapterMap	_adapters;
};

NS_WTP_END
