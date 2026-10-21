#pragma once
#include <atomic>
#include <vector>
#include <map>
#include <fstream>
#include <mutex>

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/asio/io_context.hpp>

#include "../Includes/FasterDefs.h"
#include "../Includes/ITraderApi.h"
#include "../Share/StdUtils.hpp"
#include "../Includes/WTSCollection.hpp"
#include "../Includes/WTSStruct.h"


NS_WTP_BEGIN
	class WTSTickData;
	struct WTSTickStruct;
	class WTSOrderInfo;
	class WTSTradeInfo;
NS_WTP_END

USING_NS_WTP;

/*
 *	仿真交易器
 */
class TraderMocker : public ITraderApi
{
public:
	TraderMocker();
	~TraderMocker();

private:
	/*
	*	撮合 定时器
	*/
	int32_t		match_once();

	uint32_t	makeTradeID();
	uint32_t	makeOrderID();

	void		load_positions();
	void		save_positions();


private:
	StdThreadPtr	_thrd_match;
	std::atomic<bool>	_terminated;

	StdUniqueMutex		_mutex_api;

	std::atomic<uint32_t>	_auto_trade_id;
	std::atomic<uint32_t>	_auto_order_id;
	std::atomic<uint32_t>	_auto_entrust_id;

	ITraderSpi* _listener;
	IBaseDataMgr*		_bd_mgr;

	StdThreadPtr		_thrd_worker;

	uint32_t		_millisecs;
	uint32_t		_mocker_id;
	bool			_use_newpx;
	double			_max_qty;
	double			_min_qty;

	WTSArray*		_orders;
	WTSArray*		_trades;
	typedef WTSHashMap<std::string> TickCache;
	TickCache*		_ticks;

	typedef WTSHashMap<std::string> OrderCache;
	OrderCache*			_awaits;
	StdUniqueMutex	_mtx_awaits;

	wt_hashset<std::string>	_codes;

	uint64_t		_max_tick_time;
	uint64_t		_last_match_time;

struct Lot { double volume = 0, frozen = 0, price = 0; uint32_t date = 0; };
	struct PosUnit {
		double _volume = 0, _frozen = 0;
		std::vector<Lot> lots;
	};

	typedef struct _PosItem
	{
		char	_exchg[MAX_INSTRUMENT_LENGTH];
		char	_code[MAX_INSTRUMENT_LENGTH];

		PosUnit		_long;
		PosUnit		_short;

		_PosItem()
		{
			memset(_exchg, 0, sizeof(_exchg));
			memset(_code, 0, sizeof(_code));
		}
	} PosItem;

	wt_hashmap<std::string, PosItem> _positions;
	std::string		_pos_file;
	struct FeeSpec { bool byvolume; double open, close, today, minimum; };
	struct CloseSlice { size_t lot; double volume; bool today; };
	struct Reservation {
		double margin = 0, commission = 0, volume = 0;
		std::vector<CloseSlice> closes;
	};
	struct Account {
		double balance = 0, equity = 0, realized = 0, unrealized = 0;
		double commission = 0, margin = 0, frozen_margin = 0, frozen_commission = 0, available = 0;
	};
	// A small native snapshot is published before callbacks and after each mark.
	// Readers never wait on the API worker (which may be calling Python).
	Account _account_snapshot;
	std::mutex _account_snapshot_mutex;
	uint32_t _account_date = 0, _account_time = 0, _account_tdate = 0;
	bool _account_valid = false;
	double _capital = 0, _realized = 0, _commission = 0, _margin_rate = -1;
	uint32_t _slippage_ticks = 0;
	std::map<std::string, FeeSpec> _fees;
	std::map<std::string, Reservation> _reservations;
	std::map<std::string, WTSTickStruct> _marks;
	std::ofstream _funds;
	const FeeSpec* feeSpec(WTSContractInfo* ct) const;
	double fee(WTSContractInfo* ct, double price, double qty, uint32_t offset, bool applyMinimum = true);
	double marginRate(WTSContractInfo* ct, bool isLong) const;
	double markPrice(WTSContractInfo* ct, double fallback = 0) const;
	Account account() const;
	bool reserve(WTSEntrust* entrust, WTSContractInfo* ct, Reservation& value, std::string& message);
	void unfreeze(WTSOrderInfo* order);
	bool settle(WTSOrderInfo* order, double price, double qty, double& feePaid, double& profit);
	void writeAccount(bool checkpoint = true);
	void auditReject(WTSEntrust* entrust, const char* error, const char* code);
	double _audit_fee = 0, _audit_profit = 0, _audit_quote = 0;


private:
	int			_udp_port;

	boost::asio::ip::udp::endpoint	_broad_ep;
	boost::asio::io_context			_io_service;

	boost::asio::ip::udp::socket*	_b_socket;

	boost::array<char, 1024> _b_buffer;

	void	handle_read(const boost::system::error_code& e, std::size_t bytes_transferred, bool isBroad);

	void	extract_buffer(uint32_t length, bool isBroad);

	void	reconn_udp();

//////////////////////////////////////////////////////////////////////////
//ITraderApi
public:
	virtual bool init(WTSVariant *params) override;
	// Plus replay uses the same matcher/API worker, with no UDP socket or wall timer.
	void replayAudit(const char* type, WTSOrderInfo* order = nullptr, WTSTradeInfo* trade = nullptr, const char* error = "");
	int replayStep(const WTSTickStruct* tick);
	bool replayBarrier();
	bool replayAccount(double* values, uint32_t* clock);
	void replayDateTime(uint32_t& date, uint32_t& time) const;
	bool _replay = false;
	std::atomic<uint32_t> _replay_date{0}, _replay_time{0};
	uint32_t _replay_tdate = 0;


	virtual void release() override;

	virtual void registerSpi(ITraderSpi *listener) override;

	virtual void connect() override;

	virtual void disconnect() override;

	virtual bool isConnected() override;

	virtual bool makeEntrustID(char* buffer, int length) override;

	virtual int login(const char* user, const char* pass, const char* productInfo) override;

	virtual int logout() override;

	virtual int orderInsert(WTSEntrust* eutrust) override;

	virtual int orderAction(WTSEntrustAction* action) override;

	virtual int queryAccount() override;

	virtual int queryPositions() override;

	virtual int queryOrders() override;

	virtual int queryTrades() override;
};
