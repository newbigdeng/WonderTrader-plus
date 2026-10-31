#include "WtCtaEngine.h"
#include "TraderAdapter.h"
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <cmath>
#ifdef _WIN32
#include <process.h>
#define portfolio_pid _getpid
#else
#include <unistd.h>
#define portfolio_pid getpid
#endif

USING_NS_WTP;

const char* WtCtaEngine::query_portfolio_state(const char* traderid, const char* strategyid)
{
    static thread_local std::string result;
    const auto failure = [&](const char* reason) {
        rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
        out.StartObject();
        out.Key("schema"); out.Uint(1); out.Key("producer"); out.String("wt-native-portfolio/v1");
        out.Key("pid"); out.Uint64(portfolio_pid()); out.Key("scope"); out.String("trader_account");
        out.Key("traderid"); out.String(traderid ? traderid : "");
        out.Key("strategyid"); out.String(strategyid ? strategyid : "");
        for (const char* key : {"valid", "ready", "connected", "positions_complete", "orders_complete"}) {
            out.Key(key); out.Bool(false);
        }
        out.Key("reason"); out.String(reason); out.EndObject();
        result = buffer.GetString(); return result.c_str();
    };
    std::unique_lock<std::recursive_mutex> events(_reset_events, std::try_to_lock);
    if (!events) return failure("event_loop_busy");
    if (!traderid || !*traderid || !_adapter_mgr) return failure("trader_unavailable");
    auto adapter = _adapter_mgr->getAdapter(traderid);
    if (!adapter) return failure("trader_unavailable");
    if (strategyid && *strategyid) {
        bool found = false;
        for (const auto& context : _ctx_map) if (std::string(context.second->name()) == strategyid) found = true;
        if (!found) return failure("strategy_unavailable");
    }
    PortfolioView view;
    if (!adapter->portfolio_snapshot(view)) return failure("broker_snapshot_busy_or_resetting");
    if (!adapter->isReady() || !view.connected || !view.positions_complete || !view.orders_complete || !view.trading_date)
        return failure("broker_snapshot_incomplete");
    std::map<std::string, double> targets;
    std::vector<std::pair<std::shared_ptr<WtLocalExecuter>, uint64_t>> revisions;
    std::vector<std::string> routes;
    uint64_t task_count = 0, revision_sum = 0;
    bool compatible = true, routed = !strategyid || !*strategyid;
    const auto& route = _exec_mgr.get_route(strategyid ? strategyid : "");
    _exec_mgr.enum_executer([&](ExecCmdPtr command) {
        auto local = std::dynamic_pointer_cast<WtLocalExecuter>(command);
        if (!local) { compatible = false; return; }
        if (local->reset_trader() != adapter.get()) return;
        if (route.count("ALL") || route.count(local->name())) routed = true;
        uint64_t revision = 0, pending = 0;
        std::map<std::string, double> local_targets;
        if (!local->portfolio_targets(local_targets, revision, pending)) { compatible = false; return; }
        for (const auto& target : local_targets) targets[target.first] += target.second;
        revisions.emplace_back(local, revision); routes.emplace_back(local->name());
        task_count += pending; revision_sum += revision;
    });
    if (!compatible || revisions.empty() || !routed) return failure("execution_route_busy_or_unavailable");
    PortfolioView verified;
    if (!adapter->portfolio_snapshot(verified) || verified.event_seq != view.event_seq ||
        verified.pending_submissions != view.pending_submissions) return failure("broker_snapshot_changed");
    for (const auto& revision : revisions)
        if (revision.first->portfolio_revision() != revision.second) return failure("execution_snapshot_changed");
    bool target_pending = _exec_mgr.portfolio_targets_pending();
    for (const auto& position : view.positions) {
        const auto& value = position.second;
        if (!std::isfinite(value.long_qty) || !std::isfinite(value.short_qty) ||
            value.long_qty < 0 || value.short_qty < 0) return failure("invalid_native_position");
        if (value.long_qty && value.short_qty) target_pending = true;
        if (targets.find(position.first) == targets.end() && value.long_qty != value.short_qty) target_pending = true;
    }
    for (const auto& target : targets) {
        auto found = view.positions.find(target.first);
        const double actual = found == view.positions.end() ? 0 : found->second.long_qty - found->second.short_qty;
        if (target.second != actual) target_pending = true;
    }
    std::map<std::string, uint64_t> working;
    for (const auto& order : view.orders) ++working[order.second.code];
    const bool idle = !target_pending && task_count == 0 && view.pending_submissions == 0 && working.empty();
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    out.StartObject();
    out.Key("schema"); out.Uint(1); out.Key("producer"); out.String("wt-native-portfolio/v1");
    out.Key("pid"); out.Uint64(portfolio_pid()); out.Key("scope"); out.String("trader_account");
    out.Key("traderid"); out.String(traderid); out.Key("strategyid"); out.String(strategyid ? strategyid : "");
    out.Key("trading_date"); out.Uint(view.trading_date);
    for (const char* key : {"valid", "ready", "connected", "positions_complete", "orders_complete"}) {
        out.Key(key); out.Bool(true);
    }
    out.Key("event_seq"); out.Uint64(view.event_seq + revision_sum);
    out.Key("pending_submissions"); out.Uint64(view.pending_submissions);
    out.Key("execution_tasks"); out.Uint64(task_count);
    out.Key("execution_idle"); out.Bool(idle); out.Key("target_pending"); out.Bool(target_pending);
    out.Key("route"); out.StartArray(); for (const auto& name : routes) out.String(name.c_str()); out.EndArray();
    out.Key("actual_positions"); out.StartArray();
    for (const auto& item : view.positions) {
        out.StartObject(); out.Key("code"); out.String(item.first.c_str());
        out.Key("long"); out.Double(item.second.long_qty); out.Key("short"); out.Double(item.second.short_qty); out.EndObject();
    }
    out.EndArray(); out.Key("working_orders"); out.StartObject();
    for (const auto& order : working) { out.Key(order.first.c_str()); out.Uint64(order.second); }
    out.EndObject(); out.Key("reason"); out.String(idle ? "complete_idle" : "complete_with_pending_execution");
    out.EndObject(); result = buffer.GetString(); return result.c_str();
}
