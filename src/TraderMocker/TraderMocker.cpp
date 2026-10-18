#include "TraderMocker.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSError.hpp"

#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../Share/StrUtil.hpp"

#include <boost/bind.hpp>
#include <boost/filesystem.hpp>

#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/writer.h>
namespace rj = rapidjson;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(ITraderSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	const char* buffer = fmtutil::format(format, args...);

	sink->handleTraderLog(ll, buffer);
}

namespace {
std::map<uint32_t, TraderMocker*> replayMockers;
std::mutex replayRegistryMutex;
}
extern "C"
{
	EXPORT_FLAG int plus_mocker_step(uint32_t id, const WTSTickStruct* tick)
	{
		TraderMocker* mocker;
		{ std::lock_guard<std::mutex> guard(replayRegistryMutex);
		  auto it = replayMockers.find(id); mocker = it == replayMockers.end() ? nullptr : it->second; }
		return mocker ? mocker->replayStep(tick) : -1;
	}
	EXPORT_FLAG bool plus_mocker_barrier(uint32_t id)
	{
		TraderMocker* mocker;
		{ std::lock_guard<std::mutex> guard(replayRegistryMutex);
		  auto it = replayMockers.find(id); mocker = it == replayMockers.end() ? nullptr : it->second; }
		return mocker && mocker->replayBarrier();
	}
	EXPORT_FLAG uint64_t plus_mocker_clock(uint32_t id)
	{
		std::lock_guard<std::mutex> guard(replayRegistryMutex);
		auto it = replayMockers.find(id);
		if (it == replayMockers.end()) return 0;
		uint32_t date, time;
		it->second->replayDateTime(date, time);
		return TimeUtils::makeTime(date, time);
	}
	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderMocker *instance = new TraderMocker();
		return instance;
	}

	EXPORT_FLAG void deleteTrader(ITraderApi* &trader)
	{
		if (NULL != trader)
		{
			delete trader;
			trader = NULL;
		}
	}
}

std::vector<uint32_t> splitVolume(uint32_t vol, uint32_t minQty = 1, uint32_t maxQty = 100)
{
	uint32_t length = maxQty - minQty + 1;
	std::vector<uint32_t> ret;
	if (vol <= minQty)
	{
		ret.emplace_back(vol);
	}
	else
	{
		uint32_t left = vol;
		srand((uint32_t)time(NULL));
		while (left > 0)
		{
			uint32_t curVol = minQty + (uint32_t)rand() % length;

			if (curVol >= left)
				curVol = left;

			if (curVol == 0)
				continue;

			ret.emplace_back(curVol);
			left -= curVol;
		}
	}

	return ret;
}
TraderMocker::TraderMocker()
	: _terminated(false)
	, _listener(NULL)
	, _ticks(NULL)
	, _orders(NULL)
	, _awaits(NULL)
	, _trades(NULL)
	, _b_socket(NULL)
	, _max_tick_time(0)
	, _last_match_time(0)
{
	_auto_order_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 100);
	_auto_trade_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 300);
	_auto_entrust_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 100);
}


TraderMocker::~TraderMocker()
{
	release();
	if (_replay)
	{
		std::lock_guard<std::mutex> guard(replayRegistryMutex);
		auto it = replayMockers.find(_mocker_id);
		if (it != replayMockers.end() && it->second == this) replayMockers.erase(it);
	}
	if (_awaits)
		_awaits->release();
	if (_orders)
		_orders->release();

	if (_trades)
		_trades->release();

	if (_ticks)
		_ticks->release();
}

uint32_t TraderMocker::makeTradeID()
{
	return ++_auto_trade_id;
}

uint32_t TraderMocker::makeOrderID()
{
	return ++_auto_order_id;
}

bool TraderMocker::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length == 0)
		return false;

	try
	{
		fmtutil::format_to(buffer, "me.{}.{}.{}", (_replay ? _replay_date.load() : TimeUtils::getCurDate()), _mocker_id, _auto_entrust_id++);
		return true;
	}
	catch (...)
	{

	}

	return false;
}

int TraderMocker::orderInsert(WTSEntrust* entrust)
{	
	if (entrust == NULL)
	{
		return 0;
	}

	entrust->retain();
	boost::asio::post(_io_service, [this, entrust](){
		StdUniqueLock lock(_mutex_api);

		WTSContractInfo* ct = entrust->getContractInfo();
		if(ct == NULL) 
			ct = _bd_mgr->getContract(entrust->getCode(), entrust->getExchg());

		/*
		 *	1、开仓无需检查
		 *	2、平仓要先检查可平
		 *	3、检查通过了,平仓还要冻结持仓
		 *	4、还要考虑国际期货的问题
		 */

		bool bPass = false;
		std::string msg;
		Reservation reserved;
		do 
		{
			if (ct == NULL)
			{
				bPass = false;
				msg = "品种不存在";
				break;
			}
			WTSCommodityInfo* commInfo = ct->getCommInfo();

			//检查价格类型的合法性
			if (entrust->getPriceType() == WPT_ANYPRICE && commInfo->getPriceMode() == PM_Limit)
			{
				bPass = false;
				msg = "价格类型不合法";
				break;
			}

			//检查数量的合法性
			if ((commInfo->getCategoty() == CC_Stock) && (entrust->getOffsetType() == WOT_OPEN) && !decimal::eq(decimal::mod(entrust->getVolume(), 100), 0))
			{
				bPass = false;
				msg = "股票买入数量必须为100的整数倍";
				break;
			}

			//检查方向的合法性
			if(!commInfo->canShort() && entrust->getDirection() == WDT_SHORT)
			{
				bPass = false;
				msg = "股票不能做空";
				break;
			}

			//检查价格的合法性
			if(!decimal::eq(entrust->getPrice(), 0))
			{
				double pricetick = commInfo->getPriceTick();
				double v = entrust->getPrice() / pricetick;

				if (!decimal::eq(decimal::mod(entrust->getPrice(), pricetick), 0))	//整除的检查方式,先小数相除得到商,然后商取整以后,再跟原来的商相减,如果等于0,则是整除,否则是
				{
					bPass = false;
					msg = "委托价格不合法";
					break;
				}
			}


            if (_capital > 0) {
                bPass = reserve(entrust, ct, reserved, msg);
                break;
            }
			//开仓直接通过,不检查资金
			if (entrust->getOffsetType() == WOT_OPEN)
			{
				bPass = true;
				break;
			}

			//如果不需要开平,则直接通过,主要针对国际期货
			if (commInfo->getCoverMode() == CM_None)
			{
				bPass = true;
				break;
			}
			
			//如果区分平昨平今,而委托的是平昨,则直接拒绝,因为mocker为了简化处理,不考虑昨仓
			if (commInfo->getCoverMode() == CM_CoverToday && (entrust->getOffsetType() == WOT_CLOSE || entrust->getOffsetType() == WOT_CLOSEYESTERDAY))
			{
				bPass = false;
				msg = "没有足够的可平仓位";
				break;
			}

			//如果没有持仓或者持仓不够,也要
			auto it = _positions.find(ct->getFullCode());
			if(it == _positions.end())
			{
				bPass = false;
				msg = "没有足够的可平仓位";
				break;
			}

			PosItem& pItem = (PosItem&)it->second;
			bool isLong = entrust->getDirection() == WDT_LONG;

			double validQty = isLong ? (pItem._long._volume - pItem._long._frozen) : (pItem._short._volume - pItem._short._frozen);
			if(decimal::lt(validQty, entrust->getVolume()))
			{
				bPass = false;
				msg = "没有足够的可平仓位";
				break;
			}

			//冻结持仓
			if(isLong)
			{
				pItem._long._frozen += entrust->getVolume();
			}
			else
			{
				pItem._short._frozen += entrust->getVolume();
			}

			bPass = true;
			msg = "下单成功";

		} while (false);
		
		if(bPass)
		{
			WTSOrderInfo* ordInfo = WTSOrderInfo::create();
			ordInfo->setContractInfo(ct);
			ordInfo->setCode(entrust->getCode());
			ordInfo->setExchange(entrust->getExchg());
			ordInfo->setDirection(entrust->getDirection());
			ordInfo->setOffsetType(entrust->getOffsetType());
			ordInfo->setUserTag(entrust->getUserTag());
			ordInfo->setPrice(entrust->getPrice());
			thread_local static char str[64];
			fmtutil::format_to(str, "mo.{}.{}", _mocker_id, makeOrderID());
			ordInfo->setOrderID(str);
			ordInfo->setStateMsg(msg.c_str());
			ordInfo->setOrderState(WOS_NotTraded_Queuing);
			// Date is the local calendar date of the order timestamp, not trading day.
			uint32_t orderDate, orderTime;
			replayDateTime(orderDate, orderTime);
			ordInfo->setOrderDate(orderDate);
			ordInfo->setOrderTime(TimeUtils::makeTime(orderDate, orderTime));
			ordInfo->setVolume(entrust->getVolume());
			ordInfo->setVolLeft(entrust->getVolume());
			ordInfo->setPriceType(entrust->getPriceType());
			ordInfo->setOrderFlag(entrust->getOrderFlag());

			if (_capital > 0) _reservations[ordInfo->getOrderID()] = std::move(reserved);
			replayAudit("order", ordInfo);
			if (_listener != NULL)
			{
				_listener->onRspEntrust(entrust, NULL);
				_listener->onPushOrder(ordInfo);
			}

			_codes.insert(ct->getFullCode());

			if(_listener)
			{
				write_log(_listener,LL_INFO, "共有{}个品种有待撮合订单", _codes.size());
			}

			if (_orders == NULL)
				_orders = WTSArray::create();
			_orders->append(ordInfo, false);

			if (_awaits == NULL)
				_awaits = OrderCache::create();

			_awaits->add(ordInfo->getOrderID(), ordInfo, true);

			save_positions();
		}
		else
		{
			auditReject(entrust, msg.c_str(), msg.find("资金") != std::string::npos ? "INSUFFICIENT_FUNDS" : "ORDER_REJECTED");
			WTSError* err = WTSError::create(WEC_ORDERINSERT, msg.c_str());
			if (_listener != NULL)
			{
				_listener->onRspEntrust(entrust, err);
			}
			err->release();
		}
		entrust->release();
	});

	return 0;
}

int32_t TraderMocker::match_once()
{
	if (_terminated || _orders == NULL || _orders->size() == 0 || _ticks == NULL)
		return 0;
	
	int32_t count = 0;
	for (const std::string& fullcode : _codes)
	{
		if (_terminated)
			return count;

		std::string code, exchg;
		auto pos = fullcode.find(".");
		exchg = fullcode.substr(0, pos);
		code = fullcode.substr(pos + 1);

		WTSContractInfo* ct = _bd_mgr->getContract(code.c_str(), exchg.c_str());
		if (ct == NULL)
			continue;

		WTSCommodityInfo* commInfo = ct->getCommInfo();

		WTSTickData* curTick = (WTSTickData*)_ticks->grab(fullcode);
		if (curTick && strcmp(curTick->code(), ct->getCode())==0)
		{
			StdUniqueLock lock(_mtx_awaits);
			uint64_t tickTime = (uint64_t)curTick->actiondate() * 1000000000 + curTick->actiontime();
			if (decimal::gt(curTick->price(), 0) /*&& tickTime >= _last_match_time*/)
			{
				//开始处理订单
				//处理记录
				std::vector<std::string> to_erase;

				std::vector<WTSOrderInfo*> pending;
				for (auto it = _awaits->begin(); it != _awaits->end(); ++it)
					pending.push_back((WTSOrderInfo*)it->second);
				if (_replay) std::sort(pending.begin(), pending.end(), [](WTSOrderInfo* a, WTSOrderInfo* b){
					return strtoull(strrchr(a->getOrderID(), '.')+1, nullptr, 10) < strtoull(strrchr(b->getOrderID(), '.')+1, nullptr, 10);
				});
				double bidLeft = curTick->bidqty(0), askLeft = curTick->askqty(0);
				for (WTSOrderInfo* ordInfo : pending)
				{
					if (ordInfo->getVolLeft() == 0 || strcmp(ordInfo->getCode(), curTick->code()) != 0)
						continue;

					bool isBuy = (ordInfo->getDirection() == WDT_LONG && ordInfo->getOffsetType() == WOT_OPEN) || (ordInfo->getDirection() != WDT_LONG && ordInfo->getOffsetType() != WOT_OPEN);

					double uPrice, uVolume;
					if (isBuy)
					{
						uPrice = curTick->askprice(0);
						uVolume = _replay ? askLeft : curTick->askqty(0);
					}
					else
					{
						uPrice = curTick->bidprice(0);
						uVolume = _replay ? bidLeft : curTick->bidqty(0);
					}

					if (decimal::eq(uVolume, 0))
						continue;					

					if (_use_newpx)
					{
						uPrice = curTick->price();
					}

					if (decimal::eq(uPrice, 0))
						continue;

					double target = ordInfo->getPrice();
					//买入的时候,委托价格小于最新价则不成交,卖出的时候,委托价大于最新价则不成交
					if (ordInfo->getPriceType() == WPT_LIMITPRICE && ((isBuy && decimal::lt(target, uPrice)) || (!isBuy && decimal::gt(target, uPrice))))
						continue;

                    double quotePrice = uPrice;
                    if ((curTick->upperlimit()>0 && quotePrice>curTick->upperlimit()) ||
                        (curTick->lowerlimit()>0 && quotePrice<curTick->lowerlimit())) continue;
                    double slip = _slippage_ticks * commInfo->getPriceTick();
                    uPrice += isBuy ? slip : -slip;
                    if (ordInfo->getPriceType() == WPT_LIMITPRICE)
                        uPrice = isBuy ? std::min(uPrice, target) : std::max(uPrice, target);
                    if (curTick->upperlimit() > 0) uPrice = std::min(uPrice, curTick->upperlimit());
                    if (curTick->lowerlimit() > 0) uPrice = std::max(uPrice, curTick->lowerlimit());
                    if (uPrice <= 0 || (ordInfo->getPriceType() == WPT_LIMITPRICE &&
                        ((isBuy && uPrice > target) || (!isBuy && uPrice < target)))) continue;
					count++;

					double maxVolume = min(uVolume, ordInfo->getVolLeft());
					std::vector<uint32_t> ayVol = _replay ? std::vector<uint32_t>{(uint32_t)maxVolume} : splitVolume((uint32_t)maxVolume, (uint32_t)_min_qty, (uint32_t)_max_qty);

                    for (uint32_t curVol : ayVol)
                    {
                        double feePaid = 0, profit = 0;
                        if (_capital > 0 && !settle(ordInfo, uPrice, curVol, feePaid, profit)) {
                            unfreeze(ordInfo);
                            ordInfo->setOrderState(WOS_Canceled);
                            ordInfo->setStateMsg("资金不足，剩余开仓已拒绝");
                            replayAudit("reject", ordInfo, nullptr, "INSUFFICIENT_FUNDS: 价格或权益变化后资金不足");
                            if (_listener) {
                                _listener->onPushOrder(ordInfo);
                                auto err = WTSError::create(WEC_ORDERINSERT, "INSUFFICIENT_FUNDS: 资金不足");
                                _listener->onTraderError(err); err->release();
                            }
                            to_erase.emplace_back(ordInfo->getOrderID());
                            break;
                        }
                        _audit_fee = feePaid; _audit_profit = profit; _audit_quote = quotePrice;
                        if (_replay) { if (isBuy) askLeft -= curVol; else bidLeft -= curVol; }

						WTSTradeInfo* trade = WTSTradeInfo::create(curTick->code(), curTick->exchg());
						trade->setDirection(ordInfo->getDirection());
						trade->setOffsetType(ordInfo->getOffsetType());
						trade->setContractInfo(ct);

						trade->setPrice(uPrice);
						trade->setVolume(curVol);

						trade->setRefOrder(ordInfo->getOrderID());

						char str[64];
						fmtutil::format_to(str, "mt.{}.{}", _mocker_id, makeTradeID());
						trade->setTradeID(str);

						uint32_t tradeDate, tradeTime;
						replayDateTime(tradeDate, tradeTime);
						trade->setTradeDate(tradeDate);
						trade->setTradeTime(TimeUtils::makeTime(tradeDate, tradeTime));
						trade->setUserTag(ordInfo->getUserTag());

						//更新订单数据
						ordInfo->setVolLeft(ordInfo->getVolLeft() - curVol);
						ordInfo->setVolTraded(ordInfo->getVolTraded() + curVol);
						if (decimal::eq(ordInfo->getVolLeft(), 0))
						{
							ordInfo->setOrderState(WOS_AllTraded);
							ordInfo->setStateMsg("AllTrd");
							to_erase.emplace_back(ordInfo->getOrderID());
						}
						else
						{
							ordInfo->setOrderState(WOS_PartTraded_Queuing);
							ordInfo->setStateMsg("PartTrd");
						}

						PosItem& pItem = _positions[ct->getFullCode()];
						//第一次的话要给代码和交易所赋值
						if(strlen(pItem._code) == 0)
						{
							strcpy(pItem._code, ct->getCode());
							strcpy(pItem._exchg, ct->getExchg());
						}

						if (_capital > 0) {}
						else if(commInfo->getCoverMode() == CM_None)
						{

						}
						else
						{
							if (ordInfo->getDirection() == WDT_LONG)
							{
								if (ordInfo->getOffsetType() == WOT_OPEN)
								{
									pItem._long._volume += curVol;
								}
								else
								{
									pItem._long._volume -= curVol;
									pItem._long._frozen -= curVol;
								}
							}
							else
							{
								if (ordInfo->getOffsetType() == WOT_OPEN)
								{
									pItem._short._volume += curVol;
								}
								else
								{
									pItem._short._volume -= curVol;
									pItem._short._frozen -= curVol;
								}
							}
						}
						

						replayAudit("trade", ordInfo, trade);
						if (_listener)
						{
							StdUniqueLock lock(_mutex_api);
							_listener->onPushOrder(ordInfo);
							_listener->onPushTrade(trade);
						}

						if (_trades == NULL)
							_trades = WTSArray::create();

						_trades->append(trade, false);
					}
				}

				if (count > 0)
				{
					//write_log(_listener,LL_INFO, "[TraderMocker]触发 %s.%s 开多 %u 条,价格:%u", tick->exchg(), tick->code(), iCount, uPrice);
					for (const std::string& oid : to_erase)
					{
						_awaits->remove(oid);
					}
				}
			}
		}

		if(curTick)
			curTick->release();
	}

	_last_match_time = _max_tick_time;

	_ticks->clear();

	if(count > 0)
		save_positions();

	return count;
}

//////////////////////////////////////////////////////////////////////////

bool TraderMocker::init(WTSVariant *params)
{
	_capital = params->getDouble("capital");
    _margin_rate = params->has("marginrate") ? params->getDouble("marginrate") : -1;
    double slippage = params->getDouble("slippage_ticks");
    if (!std::isfinite(_capital) || _capital < 0 || !std::isfinite(_margin_rate) ||
        (params->has("marginrate") && (_margin_rate <= 0 || _margin_rate > 1)) ||
        !std::isfinite(slippage) || slippage < 0 || slippage != std::floor(slippage) || slippage > UINT32_MAX) return false;
    _slippage_ticks = (uint32_t)slippage;
    const char* feeFile = params->getCString("fees");
    if (feeFile && *feeFile) {
        std::string text; StdFile::read_file_content(feeFile, text);
        rj::Document fees; fees.Parse(text.c_str());
        if (fees.HasParseError() || !fees.IsObject()) return false;
        for (auto it = fees.MemberBegin(); it != fees.MemberEnd(); ++it) {
            const auto& row = it->value;
            if (!row.IsObject() || !row.HasMember("byvolume") || !row["byvolume"].IsBool()) return false;
            for (const char* key : {"open","close","closetoday"})
                if (!row.HasMember(key) || !row[key].IsNumber() || row[key].GetDouble() < 0) return false;
            if (row.HasMember("minfee") && (!row["minfee"].IsNumber() || row["minfee"].GetDouble() < 0)) return false;
            double minimum = row.HasMember("minfee") ? row["minfee"].GetDouble() : 0;
            _fees[it->name.GetString()] = {row["byvolume"].GetBool(),row["open"].GetDouble(),row["close"].GetDouble(),row["closetoday"].GetDouble(),minimum};
        }
    }
	_replay = params->getBoolean("replay");
    if (_capital > 0 && !_replay) return false; // Financial accounting requires the serialized Plus replay clock.
	_replay_date = params->getUInt32("replay_date");
	_replay_time = params->getUInt32("replay_time");
	_replay_tdate = params->getUInt32("replay_tradingdate");
	_millisecs = params->getUInt32("span");
	_use_newpx = params->getBoolean("newpx");
	_mocker_id = params->getUInt32("mockerid");
	_max_qty = params->getDouble("maxqty");
	_min_qty = params->getDouble("minqty");

	if (_replay)
	{
		_auto_order_id = 0; _auto_trade_id = 0; _auto_entrust_id = 0;
		std::lock_guard<std::mutex> guard(replayRegistryMutex);
		if (replayMockers.count(_mocker_id)) return false;
		replayMockers[_mocker_id] = this;
	}
	_udp_port = params->getInt32("udp_port");

	boost::asio::ip::address addr = boost::asio::ip::make_address("0.0.0.0");
	_broad_ep = boost::asio::ip::udp::endpoint(addr, _udp_port);

	if (decimal::eq(_max_qty, 0))
		_max_qty = 100;

	if (decimal::eq(_min_qty, 0))
		_min_qty = 1;

	//加载持仓数据
	std::stringstream ss;
	ss << "./mocker_" << _mocker_id << "/";
	std::string path = ss.str();
	boost::filesystem::create_directories(path.c_str());

	_pos_file = path;
	_pos_file += "positions.json";

	return true;
}

void TraderMocker::load_positions()
{
	if (_capital > 0) return;
	if (!boost::filesystem::exists(_pos_file.c_str()))
		return;

	std::string json;
	StdFile::read_file_content(_pos_file.c_str(), json);

	rj::Document root;
	root.Parse(json.c_str());
	if (root.HasParseError())
		return;

	if(root.HasMember("positions"))
	{//读取仓位
		double total_profit = 0;
		double total_dynprofit = 0;
		const rj::Value& jPos = root["positions"];
		if (!jPos.IsNull() && jPos.IsArray())
		{
			for (const rj::Value& pItem : jPos.GetArray())
			{
				const char* exchg = pItem["exchg"].GetString();
				const char* code = pItem["code"].GetString();
				WTSContractInfo* ct = _bd_mgr->getContract(code, exchg);
				if (ct == NULL)
					continue;

				PosItem& pInfo = _positions[ct->getFullCode()];
				strcpy(pInfo._code, ct->getCode());
				strcpy(pInfo._exchg, ct->getExchg());
				
				pInfo._long._volume = pItem["long"]["volume"].GetDouble();

				pInfo._short._volume = pItem["short"]["volume"].GetDouble();
			}
		}
	}

	if (_listener)
		write_log(_listener, LL_INFO, "[TraderMocker]共加载{}条持仓数据", _positions.size());
}

void TraderMocker::save_positions()
{
	rj::Document root(rj::kObjectType);

	{//持仓数据保存
		rj::Value jPos(rj::kArrayType);

		rj::Document::AllocatorType &allocator = root.GetAllocator();

		for (auto& v : _positions)
		{
			const char* fullcode = v.first.c_str();
			const PosItem& pInfo = v.second;

			rj::Value pItem(rj::kObjectType);
			pItem.AddMember("exchg", rj::Value(pInfo._exchg, allocator), allocator);
			pItem.AddMember("code", rj::Value(pInfo._code, allocator), allocator);

			{
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("volume", pInfo._long._volume, allocator);

				pItem.AddMember("long", dItem, allocator);
			}

			{
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("volume", pInfo._short._volume, allocator);

				pItem.AddMember("short", dItem, allocator);
			}


			jPos.PushBack(pItem, allocator);
		}

		root.AddMember("positions", jPos, allocator);
	}

	{
        rj::StringBuffer sb;
        rj::PrettyWriter<rj::StringBuffer> writer(sb);
        root.Accept(writer);
        StdFile::write_file_content(_pos_file.c_str(), sb.GetString());

	}
}

void TraderMocker::release()
{
	// Idempotent shutdown, including release() after disconnect().
	const bool saveCheckpoint = (_thrd_worker != NULL);
	_terminated = true;
	if (_thrd_match && _thrd_match->joinable())
		_thrd_match->join();
	_thrd_match.reset();
	if (_thrd_worker && _thrd_worker->joinable())
	{
		// Finish API work already queued before closing the receiving service.
		boost::asio::post(_io_service, [this]{ _io_service.stop(); });
		_thrd_worker->join();
	}
	else
		_io_service.stop();
	_thrd_worker.reset();
	if (_b_socket)
	{
		boost::system::error_code ec;
		_b_socket->close(ec);
		delete _b_socket;
		_b_socket = NULL;
	}
	// No thread may mutate positions while the final checkpoint is written.
	if (saveCheckpoint && !_pos_file.empty()) {
		save_positions();
        if (_capital > 0) { writeAccount(); _funds.close(); }
    }
}

void TraderMocker::registerSpi(ITraderSpi *listener)
{
	_listener = listener;

	if (listener)
		_bd_mgr = listener->getBaseDataMgr();
}

void TraderMocker::reconn_udp()
{
	if (_b_socket != NULL)
	{
		_b_socket->close();
		delete _b_socket;
		_b_socket = NULL;
	}

	_b_socket = new boost::asio::ip::udp::socket(_io_service);

	_b_socket->open(_broad_ep.protocol());
	_b_socket->set_option(boost::asio::ip::udp::socket::reuse_address(true));
	_b_socket->set_option(boost::asio::ip::udp::socket::broadcast(true));
	_b_socket->bind(_broad_ep);


	_b_socket->async_receive_from(boost::asio::buffer(_b_buffer), _broad_ep,
		boost::bind(&TraderMocker::handle_read, this,
		boost::asio::placeholders::error,
		boost::asio::placeholders::bytes_transferred, true));
}

void TraderMocker::connect()
{
	if (_thrd_worker)
		return;
	_terminated = false;
	_io_service.restart();
	if (!_replay) reconn_udp();

		_thrd_worker.reset(new StdThread([this]{
		auto guard = boost::asio::make_work_guard(_io_service);
		_io_service.run();
	}));

	boost::asio::post(_io_service, [this](){
		StdUniqueLock lock(_mutex_api);

		load_positions();

		if (_listener)
			_listener->handleEvent(WTE_Connect, 0);
	});
}

void TraderMocker::disconnect()
{
	release();
}

bool TraderMocker::isConnected()
{
	return _replay ? _thrd_worker != NULL && !_terminated : _thrd_match != NULL;
}

int TraderMocker::login(const char* user, const char* pass, const char* productInfo)
{
	if (_thrd_match)
		return 0;
	if (!_replay) _thrd_match.reset(new StdThread([this]() {
		while (!_terminated)
		{
			match_once();

			//等待5毫秒
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}));

	boost::asio::post(_io_service, [this](){
		StdUniqueLock lock(_mutex_api);

		if (_listener)
			_listener->onLoginResult(true, "", _replay ? _replay_tdate : TimeUtils::getCurDate());
	});

	return 0;
}

int TraderMocker::logout()
{
	return 0;
}

int TraderMocker::orderAction(WTSEntrustAction* action)
{
	action->retain();
	
	boost::asio::post(_io_service, [this, action](){
		StdUniqueLock lck(_mtx_awaits);	//一定要把awaits锁起来,不然可能会导致一边撮合一边撤单
		WTSOrderInfo* ordInfo = _awaits ? (WTSOrderInfo*)_awaits->grab(action->getOrderID()) : nullptr;

		/*
		 *	撤单也要考虑几个问题
		 *	1、是否处于可以撤销的状态
		 *	2、如果是开仓,则直接撤销
		 *	3、如果是平仓,要释放冻结
		 */
		if(ordInfo == NULL)
		{
			write_log(_listener,LL_ERROR, "订单{}不存在或者已完成", action->getOrderID());
			WTSError* err = WTSError::create(WEC_ORDERCANCEL, "订单不存在或者处于不可撤销状态");
			if (_listener)
				_listener->onTraderError(err);
			err->release();
			action->release();
			return;
		}

		WTSContractInfo* ct = ordInfo->getContractInfo();
		WTSCommodityInfo* commInfo = ct->getCommInfo();

		bool bPass = false;
		if (_capital > 0) unfreeze(ordInfo);
		do 
		{
            if (_capital > 0) { bPass = true; break; }
			//开仓委托直接撤单
			if (ordInfo->getOffsetType() == WOT_OPEN)
			{
				bPass = true;
				break;
			}
			
			//不区分开平的,也直接撤销
			if (commInfo->getCoverMode() == CM_None)
			{
				bPass = true;
				break;
			}

			//释放冻结持仓
			PosItem& pItem = _positions[ct->getFullCode()];
			bool isLong = ordInfo->getDirection() == WDT_LONG;
			if(isLong)
			{
				pItem._long._frozen -= ordInfo->getVolLeft();
			}
			else
			{
				pItem._short._frozen -= ordInfo->getVolLeft();
			}
			bPass = true;

		} while (false);

		replayAudit("cancel", ordInfo);
		ordInfo->setStateMsg("撤单成功");
		ordInfo->setOrderState(WOS_Canceled);
		//ordInfo->setVolLeft(0);	

		if (_listener)
		{
			StdUniqueLock lock(_mutex_api);
			_listener->onPushOrder(ordInfo);
		}

		ordInfo->release();
		_awaits->remove(action->getOrderID());
		action->release();

		save_positions();
	});

	return 0;
}

int TraderMocker::queryAccount()
{
	boost::asio::post(_io_service, [this](){
		WTSArray* ay = WTSArray::create();
		WTSAccountInfo* accountInfo = WTSAccountInfo::create();
		accountInfo->setCurrency("CNY");
        const auto snapshot = account();
		accountInfo->setBalance(snapshot.balance);
		accountInfo->setPreBalance(_capital);
		accountInfo->setCloseProfit(snapshot.realized);
		accountInfo->setMargin(snapshot.margin);
		accountInfo->setAvailable(snapshot.available);
		accountInfo->setCommission(snapshot.commission);
		accountInfo->setFrozenMargin(snapshot.frozen_margin);
		accountInfo->setFrozenCommission(snapshot.frozen_commission);
		accountInfo->setDeposit(0);
		accountInfo->setWithdraw(0);
		accountInfo->setDynProfit(snapshot.unrealized);

		ay->append(accountInfo, false);

		if (_listener)
		{
			StdUniqueLock lock(_mutex_api);
			_listener->onRspAccount(ay);
		}

		ay->release();

	});
	return 0;
}

int TraderMocker::queryPositions()
{
	boost::asio::post(_io_service, [this](){
		WTSArray* ayPos = WTSArray::create();

		for(auto& v : _positions)
		{
			const PosItem& pItem = v.second;

			WTSContractInfo* ct = _bd_mgr->getContract(pItem._code, pItem._exchg);
			if(ct == NULL)
				continue;

            WTSCommodityInfo* commInfo = ct->getCommInfo();
            auto fillPosition = [this,ct,commInfo](WTSPositionItem* target,const PosUnit& unit,bool isLong) {
                if (_capital <= 0) return;
                double today = 0, previous = 0, availToday = 0, availPrevious = 0, cost = 0, margin = 0, pnl = 0;
                for (const auto& lot : unit.lots) {
                    if (lot.date == _replay_tdate) { today += lot.volume; availToday += lot.volume-lot.frozen; }
                    else { previous += lot.volume; availPrevious += lot.volume-lot.frozen; }
                    cost += lot.price*lot.volume;
                    double mark=markPrice(ct,lot.price);
                    margin += mark*lot.volume*commInfo->getVolScale()*marginRate(ct,isLong);
                    pnl += (mark-lot.price)*(isLong?1:-1)*lot.volume*commInfo->getVolScale();
                }
                target->setNewPosition(today);target->setPrePosition(previous);
                target->setAvailNewPos(availToday);target->setAvailPrePos(availPrevious);
                target->setAvgPrice(unit._volume>0?cost/unit._volume:0);
                target->setPositionCost(cost*commInfo->getVolScale());target->setMargin(margin);target->setDynProfit(pnl);
            };

			if(pItem._long._volume > 0)
			{
				WTSPositionItem* pInfo = WTSPositionItem::create(pItem._code, commInfo->getCurrency(), pItem._exchg);
				pInfo->setContractInfo(ct);
				pInfo->setDirection(WDT_LONG);
				pInfo->setNewPosition(pItem._long._volume);
				pInfo->setAvailNewPos(pItem._long._volume - pItem._long._frozen);
                fillPosition(pInfo,pItem._long,true);

				ayPos->append(pInfo, false);
			}

			if (pItem._short._volume > 0)
			{
				WTSPositionItem* pInfo = WTSPositionItem::create(pItem._code, commInfo->getCurrency(), pItem._exchg);
				pInfo->setContractInfo(ct);
				pInfo->setDirection(WDT_SHORT);
				pInfo->setNewPosition(pItem._short._volume);
				pInfo->setAvailNewPos(pItem._short._volume - pItem._short._frozen);
                fillPosition(pInfo,pItem._short,false);

				ayPos->append(pInfo, false);
			}
		}

		if (_listener)
		{
			StdUniqueLock lock(_mutex_api);
			_listener->onRspPosition(ayPos);
		}
		ayPos->release();
	});

	return 0;
}

int TraderMocker::queryOrders()
{
	boost::asio::post(_io_service, [this](){
		StdUniqueLock lock(_mutex_api);

		if (_listener)
			_listener->onRspOrders(_orders);
	});

	return 0;
}

int TraderMocker::queryTrades()
{
	boost::asio::post(_io_service, [this](){
		StdUniqueLock lock(_mutex_api);

		if (_listener)
			_listener->onRspTrades(_trades);
	});

	return 0;
}

void TraderMocker::handle_read(const boost::system::error_code& e, std::size_t bytes_transferred, bool isBroad /* = true */)
{
	if (e)
	{
		if (_listener)
			write_log(_listener,LL_ERROR, "[TraderMocker]UDP行情接收出错:{}({})", e.message().c_str(), e.value());

		if (!_terminated)
		{
			std::this_thread::sleep_for(std::chrono::seconds(2));
			reconn_udp();
			return;
		}
	}

	if (_terminated || bytes_transferred <= 0)
		return;

	extract_buffer(bytes_transferred, isBroad);

	if (isBroad && _b_socket)
	{
		_b_socket->async_receive_from(boost::asio::buffer(_b_buffer), _broad_ep,
			boost::bind(&TraderMocker::handle_read, this,
			boost::asio::placeholders::error,
			boost::asio::placeholders::bytes_transferred, true));
	}
}

#define UDP_MSG_PUSHTICK	0x200
#pragma pack(push,1)

typedef struct UDPPacketHead
{
	uint32_t		_type;
} UDPPacketHead;
//UDP请求包
typedef struct _UDPReqPacket : UDPPacketHead
{
	char			_data[1020];
} UDPReqPacket;

//UDPTick数据包
template <typename T>
struct UDPDataPacket : UDPPacketHead
{
	T			_data;
};
#pragma pack(pop)
typedef UDPDataPacket<WTSTickStruct>	UDPTickPacket;
void TraderMocker::extract_buffer(uint32_t length, bool isBroad /* = true */)
{
	UDPPacketHead* header = (UDPTickPacket*)_b_buffer.data();

	if (header->_type == UDP_MSG_PUSHTICK)
	{
		UDPTickPacket* packet = (UDPTickPacket*)header;
		thread_local static char fullcode[64] = { 0 };
		fmtutil::format_to(fullcode, "{}.{}", packet->_data.exchg, packet->_data.code);
		auto it = _codes.find(fullcode);
		if (it == _codes.end())
			return;

		WTSTickData* curTick = WTSTickData::create(packet->_data);
		
		if (_ticks == NULL)
			_ticks = TickCache::create();

		_ticks->add(fullcode, curTick, false);

		_max_tick_time = max(_max_tick_time, (uint64_t)curTick->actiondate() * 1000000000 + curTick->actiontime());
	}
}

// Replay operations are serialized with order insertion, cancellation and callbacks.
// A replay tick matches only orders queued before that tick; new decisions wait for the next tick.
void TraderMocker::replayDateTime(uint32_t& date, uint32_t& time) const
{
	if (_replay) { date = _replay_date.load(); time = _replay_time.load(); }
	else TimeUtils::getDateTime(date, time);
}
bool TraderMocker::replayBarrier()
{
	if (!_replay || !_thrd_worker || _terminated) return false;
	auto promise = std::make_shared<std::promise<void>>();
	auto future = promise->get_future();
	boost::asio::post(_io_service, [promise]{ promise->set_value(); });
	return future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
}
int TraderMocker::replayStep(const WTSTickStruct* tick)
{
	if (!_replay || !_thrd_worker || _terminated || !tick) return -1;
	WTSTickStruct copy = *tick;
	auto promise = std::make_shared<std::promise<int>>();
	auto future = promise->get_future();
	boost::asio::post(_io_service, [this, copy, promise]() mutable {
		_replay_date = copy.action_date; _replay_time = copy.action_time;
        _replay_tdate = copy.trading_date;
        _marks[std::string(copy.exchg) + "." + copy.code] = copy;
		if (_ticks == nullptr) _ticks = TickCache::create();
		auto curTick = WTSTickData::create(copy);
		std::string key = std::string(copy.exchg) + "." + copy.code;
		_ticks->add(key, curTick, false);
		int matched = match_once();
        if (_capital > 0) writeAccount(false);
        promise->set_value(matched);
	});
	if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return -2;
	return future.get();
}

void TraderMocker::replayAudit(const char* type, WTSOrderInfo* order, WTSTradeInfo* trade, const char* error)
{
	if (!_replay) return;
	rj::Document row(rj::kObjectType);
	auto& a = row.GetAllocator();
	row.AddMember("type", rj::Value(type, a), a);
	row.AddMember("date", _replay_date.load(), a);
	row.AddMember("time", _replay_time.load(), a);
	if (order)
	{
		row.AddMember("orderid", rj::Value(order->getOrderID(), a), a);
		row.AddMember("code", rj::Value(order->getCode(), a), a);
		row.AddMember("direction", (uint32_t)order->getDirection(), a);
		row.AddMember("offset", (uint32_t)order->getOffsetType(), a);
		row.AddMember("price", order->getPrice(), a);
		row.AddMember("volume", order->getVolume(), a);
		row.AddMember("left", order->getVolLeft(), a);
	}
	if (trade)
	{
		row.AddMember("tradeid", rj::Value(trade->getTradeID(), a), a);
		row["price"] = trade->getPrice();
		row["volume"] = trade->getVolume();
	}
    if (trade) {
        row.AddMember("fee", _audit_fee, a); row.AddMember("realized", _audit_profit, a);
        row.AddMember("quote_price", _audit_quote, a);
        row.AddMember("slippage", std::abs(trade->getPrice() - _audit_quote), a);
        row.AddMember("slippage_ticks", _slippage_ticks, a);
    }
    if (error && *error) {
        row.AddMember("error", rj::Value(error, a), a);
        row.AddMember("error_code", rj::Value(strstr(error,"INSUFFICIENT_FUNDS") ? "INSUFFICIENT_FUNDS" : "ORDER_REJECTED", a), a);
    }
    if (_capital > 0) {
        writeAccount();
        std::ifstream input("replay_account.json");
        std::string raw((std::istreambuf_iterator<char>(input)),{});
        rj::Document snapshot; snapshot.Parse(raw.c_str());
        if (!snapshot.HasParseError()) {
            rj::Value item; item.CopyFrom(snapshot,a); row.AddMember("account",item,a);
        }
    }
	rj::StringBuffer buffer;
	rj::Writer<rj::StringBuffer> writer(buffer);
	row.Accept(writer);
	std::ofstream out("replay_events.jsonl", std::ios::app);
	out << buffer.GetString() << "\n";
}
