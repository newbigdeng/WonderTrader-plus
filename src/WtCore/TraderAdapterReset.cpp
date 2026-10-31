#include "TraderAdapter.h"
#include "../Includes/ITraderApi.h"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSError.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/CodeHelper.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstdio>
#include "WtHelper.h"
#include "../Share/TimeUtils.hpp"
#include <filesystem>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

USING_NS_WTP;

static std::string reset_code(WTSContractInfo* contract)
{
    return CodeHelper::rawMonthCodeToStdCode(contract->getCode(), contract->getExchg());
}

bool TraderAdapter::begin_strategy_reset(const std::string& request,
    const std::vector<std::string>& contracts, std::string& error, bool query_only)
{
    std::lock_guard<std::recursive_mutex> lock(_reset_gate);
    if (!_trader_api || _rt_data_file.empty() || _reset_active) {
        error = "trading channel is not ready or another reset is running"; return false;
    }
    if (query_only && !isReady()) { error = "trading channel is not ready"; return false; }
    _reconcile_proof.clear();
    _reset_previous_blocked = _reset_blocked;
    _reset_query_only = query_only;
    if (!query_only) {
        _reset_blocked = true;
        std::ofstream marker(_rt_data_file + ".reset-block", std::ios::trunc);
        marker << request; marker.close();
        if (!marker) { error = "cannot persist trading freeze"; return false; }
    }
    _reset_blocked = _reset_active = true;
    _reset_contracts = contracts; _reset_stage = 0; _reset_delay = 0;
    _reset_drain_orders = _reset_wait_orders;
    _reset_drain_positions = _reset_wait_positions;
    _reset_orders_ready = _reset_positions_ready = false;
    _reset_error.clear(); _reset_orders.clear(); _reset_positions.clear();
    return true;
}

void TraderAdapter::reset_trade_event()
{
    std::lock_guard<std::recursive_mutex> lock(_reset_gate);
    ++_reset_epoch;
}

bool TraderAdapter::reset_order_response(const WTSArray* orders)
{
    std::lock_guard<std::recursive_mutex> lock(_reset_gate);
    if (!_reset_wait_orders) return false;
    _reset_wait_orders = false;
    if (!_reset_active || _reset_drain_orders) {
        _reset_drain_orders = false;
        return isReady(); // Drain an abandoned query; do not rebuild live statistics.
    }
    _reset_orders.clear();
    if (!orders) _reset_error = "broker order query returned no response";
    else for (auto it = orders->begin(); it != orders->end(); ++it) {
        auto* order = static_cast<WTSOrderInfo*>(*it);
        if (!order || !order->isAlive()) continue;
        if (_reset_query_only && (!std::isfinite(order->getVolLeft()) || order->getVolLeft() < 0 || !order->getOrderID()[0])) {
            _reset_error = "broker order query contains unknown order data"; break;
        }
        const std::string prefix = _order_pattern + ".";
        if (!order->getContractInfo() || std::string(order->getUserTag()).find(prefix) != 0) {
            _reset_error = "active order is not owned by this trading channel"; break;
        }
        const auto code = reset_code(order->getContractInfo());
        if (std::find(_reset_contracts.begin(), _reset_contracts.end(), code) == _reset_contracts.end()) {
            _reset_error = "active order is outside reset contracts"; break;
        }
        order->retain();
        _reset_orders.emplace_back(order, [](WTSOrderInfo* value) { value->release(); });
    }
    _reset_orders_ready = true;
    return true; // Do not rebuild normal order statistics during an administrative query.
}

void TraderAdapter::reset_position_response(const WTSArray* positions)
{
    std::lock_guard<std::recursive_mutex> lock(_reset_gate);
    if (!_reset_wait_positions) return;
    _reset_wait_positions = false;
    if (!_reset_active || _reset_drain_positions) {
        _reset_drain_positions = false;
        return; // A failed request's receipt cannot confirm a later request.
    }
    _reset_positions.clear();
    if (!positions) _reset_error = "broker position query returned no response";
    else if (_reset_epoch != _reset_query_epoch) {
        // A fill/order changed while the query was in flight; request a new snapshot.
        if (_reset_query_only) _reset_error = "trade event changed during reconciliation";
        else _reset_stage = 0;
        return;
    } else for (auto it = positions->begin(); it != positions->end(); ++it) {
        auto* position = static_cast<WTSPositionItem*>(*it);
        if (!position || !position->getContractInfo()) {
            _reset_error = "position query contains an unknown contract"; break;
        }
        if (position->getTotalPosition() == 0) continue;
        const auto code = reset_code(position->getContractInfo());
        if (std::find(_reset_contracts.begin(), _reset_contracts.end(), code) == _reset_contracts.end()) {
            _reset_error = "broker holds a position outside reset contracts"; break;
        }
        ResetPosition item{code, position->getDirection() == WDT_LONG,
            position->getPrePosition(), position->getNewPosition(),
            position->getAvailPrePos(), position->getAvailNewPos()};
        if (!std::isfinite(item.previous) || !std::isfinite(item.today) ||
            item.previous < 0 || item.today < 0 ||
            item.previous_available < item.previous || item.today_available < item.today) {
            _reset_error = "position is invalid or still frozen by an order"; break;
        }
        _reset_positions.push_back(item);
    }
    _reset_positions_ready = true;
}

std::string TraderAdapter::advance_strategy_reset(uint64_t now,
    const std::function<double(const std::string&, bool)>& quote, std::string& error)
{
    std::lock_guard<std::recursive_mutex> lock(_reset_gate);
    if (!_reset_active || !isReady()) { error = "trading channel disconnected"; return "failed"; }
    if (!_reset_error.empty()) { error = _reset_error; return "failed"; }
    if (_metrics_query_pending || _reset_drain_orders || _reset_drain_positions || now < _reset_delay) return "confirming";
    if (_reset_stage == 0) {
        _reset_query_epoch = _reset_epoch;
        _reset_stage = 1; _reset_orders_ready = false; _reset_wait_orders = true;
        if (_trader_api->queryOrders() < 0) { _reset_wait_orders = false; error = "broker refused order query"; return "failed"; }
        return _reset_query_only ? "querying_orders" : "cancelling";
    }
    if (_reset_stage == 1) {
        if (!_reset_orders_ready) return _reset_query_only ? "querying_orders" : "cancelling";
        if (!_reset_query_only && !_reset_orders.empty()) {
            for (const auto& order : _reset_orders)
                if (!doCancel(order.get())) { error = "broker cancellation was rejected"; return "failed"; }
            _reset_stage = 0; _reset_delay = now + 1500;
            return "cancelling";
        }
        _reset_stage = 2; _reset_wait_positions = true;
        _reset_positions_ready = false;
        if (!_reset_query_only) _reset_query_epoch = _reset_epoch;
        if (_trader_api->queryPositions() < 0) { _reset_wait_positions = false; error = "broker refused position query"; return "failed"; }
        return "confirming";
    }
    if (!_reset_positions_ready) return "confirming";
    if (_reset_query_epoch != _reset_epoch) {
        if (_reset_query_only) { error = "trade event changed during reconciliation"; return "failed"; }
        _reset_stage = 0; return "confirming";
    }
    if (_reset_query_only) return "ready"; // Never cancel, flatten, archive or clear targets.
    if (_reset_positions.empty()) return "ready";
    // Only explicit CLOSE/CLOSETODAY offsets are permitted; never route through buy/sell.
    _reset_sending = true;
    bool submitted = true;
    for (const auto& position : _reset_positions) {
        const double price = quote(position.code, !position.isLong);
        if (!std::isfinite(price) || price <= 0) { submitted = false; error = "no fresh executable bid/ask"; break; }
        for (bool today : {false, true}) {
            const double quantity = today ? position.today : position.previous;
            if (quantity <= 0) continue;
            if (!checkOrderLimits(position.code.c_str())) { submitted = false; error = "close order rate limit reached"; break; }
            const double signedQuantity = position.isLong ? -quantity : quantity;
            updateUndone(position.code.c_str(), signedQuantity);
            const auto id = position.isLong ? closeLong(position.code.c_str(), price, quantity, today, 0)
                                           : closeShort(position.code.c_str(), price, quantity, today, 0);
            if (id == UINT_MAX) {
                updateUndone(position.code.c_str(), -signedQuantity);
                submitted = false; error = "reduce-only close was rejected"; break;
            }
        }
        if (!submitted) break;
    }
    _reset_sending = false;
    if (!submitted) return "failed";
    _reset_stage = 0; _reset_positions_ready = false; _reset_delay = now + 5000;
    return "flattening";
}

bool TraderAdapter::finish_strategy_reset(std::string* error)
{
    std::lock_guard<std::recursive_mutex> lock(_reset_gate);
    portfolio_reset_complete(_reset_error.empty() && _reset_orders_ready && _reset_positions_ready &&
        _reset_query_epoch == _reset_epoch);
    if (_reset_query_only) {
        _reset_active = false; _reset_blocked = _reset_previous_blocked;
        return true;
    }
    // Failure keeps this flag and its on-disk marker. Success is called only after archival.
    if (std::remove((_rt_data_file + ".reset-block").c_str()) != 0) {
        if (error) *error = "cannot remove persisted reset freeze";
        return false;
    }
    _reset_active = false; _reset_blocked = false;
    _reset_orders.clear(); _reset_positions.clear();
    return true;
}

void TraderAdapter::fail_strategy_reset()
{
    std::lock_guard<std::recursive_mutex> lock(_reset_gate);
    portfolio_reset_complete(false);
    _reset_active = false;
    if (_reset_query_only) _reset_blocked = _reset_previous_blocked;
    // Both memory and durable submission gate remain frozen.
}

std::string TraderAdapter::reconcile_snapshot(bool execution_idle)
{
    std::lock_guard<std::recursive_mutex> lock(_reset_gate);
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    const bool complete = _reset_orders_ready && _reset_positions_ready && _reset_error.empty() &&
        _reset_query_epoch == _reset_epoch && isReady() && _trading_day != 0;
    out.StartObject();
    auto flag = [&](const char* name, bool value) { out.Key(name); out.Bool(value); };
    flag("query_complete", complete); flag("orders_complete", complete); flag("positions_complete", complete);
    flag("connected", isReady()); flag("ready", isReady());
    flag("execution_idle", execution_idle && !_reset_previous_blocked);
    out.Key("event_seq"); out.Uint64(_reset_epoch);
    out.Key("pending_submissions"); out.Uint64(_pending_submissions.size());
    out.Key("trading_date"); out.Uint(_trading_day);
    out.Key("positions"); out.StartArray();
    for (const auto& item : _reset_positions) {
        out.StartObject(); out.Key("code"); out.String(item.code.c_str());
        out.Key("is_long"); out.Bool(item.isLong);
        out.Key("quantity"); out.Double(item.previous + item.today); out.EndObject();
    }
    out.EndArray(); out.Key("alive_orders"); out.StartArray();
    for (const auto& item : _reset_orders) {
        out.StartObject(); out.Key("orderid"); out.String(item->getOrderID());
        out.Key("quantity_left"); out.Double(item->getVolLeft()); out.EndObject();
    }
    out.EndArray(); out.EndObject(); return buffer.GetString();
}

void TraderAdapter::retain_reconcile_proof(const std::string& json)
{
    std::lock_guard<std::recursive_mutex> lock(_reset_gate);
    rapidjson::Document proof; proof.Parse(json.c_str());
    if (proof.HasParseError() || !proof.IsObject() || !proof.HasMember("event_seq") ||
        !proof["event_seq"].IsUint64() || proof["event_seq"].GetUint64() != _reset_epoch ||
        !proof["query_complete"].GetBool() || !proof["execution_idle"].GetBool() ||
        !proof["positions"].Empty() || !proof["alive_orders"].Empty() ||
        !_pending_submissions.empty() || !isReady()) { _reconcile_proof.clear(); return; }
    _reconcile_proof = json; _reconcile_proof_epoch = _reset_epoch;
    _reconcile_connected = isReady();
}

void TraderAdapter::finish_reconcile_release(bool previously_ready)
{
    std::lock_guard<std::recursive_mutex> lock(_reset_gate);
    if (_reconcile_proof.empty()) return;
    rapidjson::Document proof; proof.Parse(_reconcile_proof.c_str());
    if (proof.HasParseError() || !proof.IsObject()) return;
    auto& allocator = proof.GetAllocator();
    proof.AddMember("terminal", true, allocator);
    proof.AddMember("valid", previously_ready && _reconcile_connected &&
        _reset_epoch == _reconcile_proof_epoch && _pending_submissions.empty(), allocator);
    proof["event_seq"].SetUint64(_reset_epoch);
    proof.AddMember("sealed_at_ms", static_cast<uint64_t>(TimeUtils::getLocalTimeNow()), allocator);
    const auto path = std::filesystem::path(WtHelper::getBaseDir()) / "strategy-control/reconcile-seal.json";
    std::error_code error; std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer); proof.Accept(writer);
    const auto temporary = path.string() + ".tmp";
    std::ofstream file(temporary, std::ios::binary); file << buffer.GetString(); file.close();
    if (file) std::rename(temporary.c_str(), path.string().c_str());
}

void TraderAdapter::reset_entrust_error(WTSError* error)
{
    std::lock_guard<std::recursive_mutex> lock(_reset_gate);
    if (_reset_active && error && error->getErrorCode() != WEC_NONE)
        _reset_error = std::string("broker rejected reset order: ") + error->getMessage();
}
