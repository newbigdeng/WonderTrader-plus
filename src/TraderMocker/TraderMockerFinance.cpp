#include "TraderMocker.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/IBaseDataMgr.h"
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <cmath>
#include <iomanip>
#include <algorithm>
namespace rj = rapidjson;

// Fee formulas and cent rounding stay in WT's contract/commodity metadata.
const TraderMocker::FeeSpec* TraderMocker::feeSpec(WTSContractInfo* ct) const
{
    auto found = _fees.find(ct->getFullCode());
    if (found == _fees.end()) found = _fees.find(ct->getCommInfo()->getFullPid());
    return found == _fees.end() ? nullptr : &found->second;
}
double TraderMocker::fee(WTSContractInfo* ct, double price, double qty, uint32_t offset, bool applyMinimum)
{
    const auto* spec = feeSpec(ct);
    double minimum = 0, rate = 0;
    if (spec) {
        ct->setFeeRates(spec->open, spec->close, spec->today, spec->byvolume);
        minimum = spec->minimum;
        rate = offset == 0 ? spec->open : offset == 2 ? spec->today : spec->close;
    }
    double result = ct->calcFee(price, qty, offset);
    if (applyMinimum && qty > 0 && rate > 0 && minimum > result) result = minimum;
    return std::round(result * 100) / 100;
}
double TraderMocker::marginRate(WTSContractInfo* ct, bool isLong) const
{
    return _margin_rate >= 0 ? _margin_rate :
        (isLong ? ct->getLongMarginRatio() : ct->getShortMarginRatio());
}
double TraderMocker::markPrice(WTSContractInfo* ct, double fallback) const
{
    auto found = _marks.find(ct->getFullCode());
    return found != _marks.end() && found->second.price > 0 ? found->second.price : fallback;
}
TraderMocker::Account TraderMocker::account() const
{
    Account result;
    result.realized = _realized; result.commission = _commission;
    result.balance = _capital + _realized - _commission;
    for (const auto& pair : _positions) {
        auto ct = _bd_mgr->getContract(pair.second._code, pair.second._exchg);
        if (!ct) continue;
        double multiplier = ct->getCommInfo()->getVolScale();
        for (int side = 0; side < 2; ++side) {
            const auto& unit = side == 0 ? pair.second._long : pair.second._short;
            for (const auto& lot : unit.lots) {
                double mark = markPrice(ct, lot.price);
                result.unrealized += (mark - lot.price) * (side == 0 ? 1 : -1) * lot.volume * multiplier;
                result.margin += mark * lot.volume * multiplier * marginRate(ct, side == 0);
            }
        }
    }
    for (const auto& pair : _reservations) {
        result.frozen_margin += pair.second.margin;
        result.frozen_commission += pair.second.commission;
    }
    result.equity = result.balance + result.unrealized;
    result.available = result.equity - result.margin - result.frozen_margin - result.frozen_commission;
    return result;
}
bool TraderMocker::reserve(WTSEntrust* entrust, WTSContractInfo* ct, Reservation& value, std::string& message)
{
    const double qty = entrust->getVolume();
    const bool isLong = entrust->getDirection() == WDT_LONG;
    if (!(qty > 0) || !std::isfinite(qty)) { message = "委托数量不合法"; return false; }
    const bool opening = entrust->getOffsetType() == WOT_OPEN;
    double reference = entrust->getPrice();
    if (reference <= 0) {
        auto tick = _marks.find(ct->getFullCode());
        bool buy = opening ? isLong : !isLong;
        if (tick != _marks.end())
            reference = buy ? tick->second.ask_prices[0] : tick->second.bid_prices[0];
        if (reference > 0) reference += (buy ? 1 : -1) * _slippage_ticks * ct->getCommInfo()->getPriceTick();
    }
    if (!(reference > 0)) { message = "没有可用报价，无法检查资金"; return false; }
    value.volume = qty;
    if (opening) {
        const double ratio = marginRate(ct, isLong);
        if (!(ratio > 0) || !std::isfinite(ratio)) { message = "缺少有效保证金率"; return false; }
        value.margin = reference * qty * ct->getCommInfo()->getVolScale() * ratio;
        value.commission = std::max(fee(ct, reference, qty, 0), qty * fee(ct, reference, 1, 0));
        if (account().available + 1e-8 < value.margin + value.commission) {
            message = "INSUFFICIENT_FUNDS: 可用资金不足，无法冻结保证金和手续费"; return false;
        }
        return true;
    }
    auto found = _positions.find(ct->getFullCode());
    if (found == _positions.end()) { message = "没有足够的可平仓位"; return false; }
    auto& unit = isLong ? found->second._long : found->second._short;
    const auto offset = entrust->getOffsetType();
    bool onlyToday = offset == WOT_CLOSETODAY;
    bool onlyPrevious = offset == WOT_CLOSEYESTERDAY ||
        (offset == WOT_CLOSE && ct->getCommInfo()->getCoverMode() == CM_CoverToday);
    double left = qty, todayQty = 0, previousQty = 0;
    // Yesterday first for generic close. Explicit close-today never consumes yesterday.
    for (int todayPass = 0; todayPass < 2 && left > 1e-8; ++todayPass) {
        for (size_t index = 0; index < unit.lots.size() && left > 1e-8; ++index) {
            const auto& lot = unit.lots[index];
            bool today = lot.date == _replay_tdate;
            if (today != (todayPass == 1) || (onlyToday && !today) || (onlyPrevious && today)) continue;
            double take = std::min(left, lot.volume - lot.frozen);
            if (take <= 0) continue;
            value.closes.push_back({index, take, today});
            if (today) todayQty += take; else previousQty += take;
            left -= take;
        }
    }
    if (left > 1e-8) { message = "没有足够的可平仓位"; return false; }
    for (const auto& slice : value.closes) unit.lots[slice.lot].frozen += slice.volume;
    unit._frozen += qty;
    value.commission = std::max(fee(ct, reference, todayQty, 2), todayQty * fee(ct, reference, 1, 2)) +
        std::max(fee(ct, reference, previousQty, 1), previousQty * fee(ct, reference, 1, 1));
    // Closing is allowed even after losses leave negative available capital.
    return true;
}
void TraderMocker::unfreeze(WTSOrderInfo* order)
{
    auto found = _reservations.find(order->getOrderID());
    if (found == _reservations.end()) return;
    if (order->getOffsetType() != WOT_OPEN) {
        auto& position = _positions[order->getContractInfo()->getFullCode()];
        auto& unit = order->getDirection() == WDT_LONG ? position._long : position._short;
        for (const auto& slice : found->second.closes) {
            unit.lots[slice.lot].frozen -= slice.volume;
            unit._frozen -= slice.volume;
        }
        if (unit._frozen < 1e-8)
            unit.lots.erase(std::remove_if(unit.lots.begin(),unit.lots.end(),[](const Lot& lot){return lot.volume<1e-8;}),unit.lots.end());
    }
    _reservations.erase(found);
}
bool TraderMocker::settle(WTSOrderInfo* order, double price, double qty, double& feePaid, double& profit)
{
    auto found = _reservations.find(order->getOrderID());
    if (found == _reservations.end()) return false;
    auto& reservation = found->second;
    auto ct = order->getContractInfo();
    auto& position = _positions[ct->getFullCode()];
    strcpy(position._code, ct->getCode()); strcpy(position._exchg, ct->getExchg());
    bool isLong = order->getDirection() == WDT_LONG;
    auto& unit = isLong ? position._long : position._short;
    double multiplier = ct->getCommInfo()->getVolScale();
    double fraction = qty / reservation.volume;
    double releaseMargin = reservation.margin * fraction;
    double releaseCommission = reservation.commission * fraction;
    if (order->getOffsetType() == WOT_OPEN) {
        feePaid = fee(ct, price, qty, 0);
        double mark = markPrice(ct, price);
        double requiredMargin = mark * qty * multiplier * marginRate(ct, isLong);
        double newUnrealized = (mark - price) * (isLong ? 1 : -1) * qty * multiplier;
        if (account().available + releaseMargin + releaseCommission + newUnrealized + 1e-8 < requiredMargin + feePaid)
            return false;
        unit.lots.push_back({qty, 0, price, _replay_tdate});
        unit._volume += qty;
    } else {
        double left = qty, todayQty = 0, previousQty = 0;
        for (auto& slice : reservation.closes) {
            double take = std::min(left, slice.volume);
            if (take <= 0) continue;
            auto& lot = unit.lots[slice.lot];
            bool today = lot.date == _replay_tdate;
            profit += (price - lot.price) * (isLong ? 1 : -1) * take * multiplier;
            if (today) todayQty += take; else previousQty += take;
            lot.volume -= take; lot.frozen -= take;
            unit._volume -= take; unit._frozen -= take;
            slice.volume -= take; left -= take;
        }
        // A generic close can consume yesterday and today in one native fill.
        // Apply their distinct rates, then charge the minimum once for this fill.
        feePaid = fee(ct, price, todayQty, 2, false) + fee(ct, price, previousQty, 1, false);
        const auto* spec = feeSpec(ct);
        if (spec && ((todayQty > 0 && spec->today > 0) || (previousQty > 0 && spec->close > 0)))
            feePaid = std::max(feePaid, spec->minimum);
        feePaid = std::round(feePaid * 100) / 100;
    }
    _commission += feePaid; _realized += profit;
    reservation.margin -= releaseMargin; reservation.commission -= releaseCommission;
    reservation.volume -= qty;
    if (reservation.volume < 1e-8) _reservations.erase(found);
    if (unit._frozen < 1e-8)
        unit.lots.erase(std::remove_if(unit.lots.begin(),unit.lots.end(),[](const Lot& lot){return lot.volume<1e-8;}),unit.lots.end());
    return true;
}
void TraderMocker::writeAccount(bool checkpoint)
{
    if (_capital <= 0) return;
    const auto snapshot = account();
    {
        std::lock_guard<std::mutex> guard(_account_snapshot_mutex);
        _account_snapshot = snapshot;
        _account_date = _replay_date.load(); _account_time = _replay_time.load();
        _account_tdate = _replay_tdate;
        _account_valid = _account_tdate != 0;
    }
    if (!_funds.is_open()) {
        _funds.open("replay_funds.csv", std::ios::out);
        _funds << "date,time,trading_date,balance,equity,realized,unrealized,commission,margin,frozen_margin,frozen_commission,available\n";
    }
    _funds << std::setprecision(15) << _replay_date << ',' << _replay_time << ',' << _replay_tdate << ','
        << snapshot.balance << ',' << snapshot.equity << ',' << snapshot.realized << ',' << snapshot.unrealized << ','
        << snapshot.commission << ',' << snapshot.margin << ',' << snapshot.frozen_margin << ','
        << snapshot.frozen_commission << ',' << snapshot.available << '\n';
    if (!checkpoint) return;
    rj::Document row(rj::kObjectType); auto& a = row.GetAllocator();
    row.AddMember("date",_replay_date.load(),a); row.AddMember("time",_replay_time.load(),a);
    row.AddMember("trading_date",_replay_tdate,a); row.AddMember("capital",_capital,a);
#define FIELD(name) row.AddMember(#name,snapshot.name,a)
    FIELD(balance);FIELD(equity);FIELD(realized);FIELD(unrealized);FIELD(commission);
    FIELD(margin);FIELD(frozen_margin);FIELD(frozen_commission);FIELD(available);
#undef FIELD
    rj::Value positions(rj::kArrayType);
    for (const auto& pair : _positions) {
        auto ct = _bd_mgr->getContract(pair.second._code,pair.second._exchg);
        if (!ct) continue;
        for (int side=0;side<2;++side) {
            const auto& unit=side==0?pair.second._long:pair.second._short;
            if (unit._volume<=1e-8) continue;
            double today=0,cost=0,margin=0,unrealized=0;
            double multiplier=ct->getCommInfo()->getVolScale();
            for (const auto& lot:unit.lots) {
                if(lot.date==_replay_tdate)today+=lot.volume;
                cost+=lot.volume*lot.price;
                double mark=markPrice(ct,lot.price);
                margin+=mark*lot.volume*multiplier*marginRate(ct,side==0);
                unrealized+=(mark-lot.price)*(side==0?1:-1)*lot.volume*multiplier;
            }
            rj::Value item(rj::kObjectType);
            item.AddMember("code",rj::Value(ct->getFullCode(),a),a);
            item.AddMember("direction",side,a); item.AddMember("volume",unit._volume,a);
            item.AddMember("today_volume",today,a);item.AddMember("yesterday_volume",unit._volume-today,a);
            item.AddMember("frozen",unit._frozen,a);item.AddMember("average_price",cost/unit._volume,a);
            item.AddMember("margin",margin,a);item.AddMember("unrealized",unrealized,a);
            positions.PushBack(item,a);
        }
    }
    row.AddMember("positions",positions,a);
    rj::StringBuffer buffer; rj::Writer<rj::StringBuffer> writer(buffer);row.Accept(writer);
    std::ofstream output("replay_account.json");output<<buffer.GetString()<<'\n';
    _funds.flush();
}
void TraderMocker::auditReject(WTSEntrust* entrust,const char* error,const char* code)
{
    if (!_replay) return;
    rj::Document row(rj::kObjectType);auto& a=row.GetAllocator();
    row.AddMember("type","reject",a);row.AddMember("date",_replay_date.load(),a);
    row.AddMember("time",_replay_time.load(),a);
    row.AddMember("entrustid",rj::Value(entrust->getEntrustID(),a),a);
    row.AddMember("code",rj::Value(entrust->getCode(),a),a);
    row.AddMember("direction",(uint32_t)entrust->getDirection(),a);
    row.AddMember("offset",(uint32_t)entrust->getOffsetType(),a);
    row.AddMember("price",entrust->getPrice(),a);row.AddMember("volume",entrust->getVolume(),a);
    row.AddMember("error",rj::Value(error,a),a);row.AddMember("error_code",rj::Value(code,a),a);
    if(_capital>0) {
        writeAccount();
        std::ifstream input("replay_account.json");
        std::string text((std::istreambuf_iterator<char>(input)),{});
        rj::Document snapshot;snapshot.Parse(text.c_str());
        rj::Value item;item.CopyFrom(snapshot,a);row.AddMember("account",item,a);
    }
    rj::StringBuffer buffer;rj::Writer<rj::StringBuffer> writer(buffer);row.Accept(writer);
    std::ofstream output("replay_events.jsonl",std::ios::app);output<<buffer.GetString()<<'\n';
}
