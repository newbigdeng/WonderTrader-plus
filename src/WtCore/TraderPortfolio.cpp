#include "TraderAdapter.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Share/CodeHelper.hpp"
#include <cmath>

USING_NS_WTP;

namespace {
std::string portfolio_code(WTSContractInfo* contract) {
    if (!contract || !contract->getCommInfo()) return {};
    auto* commodity = contract->getCommInfo();
    if (commodity->getCategoty() == CC_FutOption || commodity->getCategoty() == CC_SpotOption)
        return CodeHelper::rawFutOptCodeToStdCode(contract->getCode(), contract->getExchg());
    if (CodeHelper::isMonthlyCode(contract->getCode()))
        return CodeHelper::rawMonthCodeToStdCode(contract->getCode(), contract->getExchg());
    return CodeHelper::rawFlatCodeToStdCode(contract->getCode(), contract->getExchg(), contract->getProduct());
}
bool set_order(PortfolioView& view, WTSOrderInfo* order) {
    if (!order) return false;
    const auto code = portfolio_code(order->getContractInfo());
    const std::string order_id = order->getOrderID(), entrust_id = order->getEntrustID();
    if (code.empty() || (order_id.empty() && entrust_id.empty()) ||
        !std::isfinite(order->getVolLeft()) || order->getVolLeft() < 0) return false;
    for (auto it = view.orders.begin(); it != view.orders.end();) {
        if (it->second.code == code && ((!order_id.empty() && it->second.order_id == order_id) ||
            (!entrust_id.empty() && it->second.entrust_id == entrust_id))) it = view.orders.erase(it);
        else ++it;
    }
    if (order->isAlive()) view.orders[code + '#' + (entrust_id.empty() ? order_id : entrust_id)] =
        PortfolioOrder{code, order_id, entrust_id};
    return true;
}
}

void TraderAdapter::portfolio_status(bool connected, uint32_t day) {
    _portfolio_state.update([&](PortfolioView& view) {
        view.connected = connected;
        if (day) view.trading_date = day;
        if (!connected) view.positions_complete = view.orders_complete = false;
    });
}

void TraderAdapter::portfolio_positions(const WTSArray* positions) {
    _portfolio_state.update([&](PortfolioView& view) {
        view.positions.clear(); view.positions_complete = positions != nullptr;
        if (!positions) return;
        for (auto it = positions->begin(); it != positions->end(); ++it) {
            auto* position = static_cast<WTSPositionItem*>(*it);
            const auto code = position ? portfolio_code(position->getContractInfo()) : std::string();
            if (code.empty() || !std::isfinite(position->getTotalPosition()) || position->getTotalPosition() < 0) {
                view.positions_complete = false; continue;
            }
            auto& target = view.positions[code];
            (position->getDirection() == WDT_LONG ? target.long_qty : target.short_qty) = position->getTotalPosition();
        }
    });
}

void TraderAdapter::portfolio_orders(const WTSArray* orders) {
    _portfolio_state.update([&](PortfolioView& view) {
        view.orders.clear(); view.orders_complete = orders != nullptr;
        if (orders) for (auto it = orders->begin(); it != orders->end(); ++it)
            if (!set_order(view, static_cast<WTSOrderInfo*>(*it))) view.orders_complete = false;
    });
}

void TraderAdapter::portfolio_order(WTSOrderInfo* order) {
    _portfolio_state.update([&](PortfolioView& view) {
        if (!set_order(view, order)) view.orders_complete = false;
    });
}

void TraderAdapter::portfolio_reset_complete(bool complete) {
    if (!complete) _portfolio_state.update([](PortfolioView& view) {
        view.positions_complete = view.orders_complete = false;
    });
}

bool TraderAdapter::portfolio_snapshot(PortfolioView& result) {
    std::unique_lock<std::recursive_mutex> gate(_reset_gate, std::try_to_lock);
    if (!gate || _reset_active || _reset_blocked || _reconcile_releasing ||
        !_portfolio_state.snapshot(result)) return false;
    result.pending_submissions = _pending_submissions.size();
    return true;
}
