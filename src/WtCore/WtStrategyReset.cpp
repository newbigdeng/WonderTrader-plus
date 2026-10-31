#include "WtCtaEngine.h"
#include "CtaStraBaseCtx.h"
#include "TraderAdapter.h"
#include "WtDtMgr.h"
#include "WtHelper.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSRiskDef.hpp"
#include "../Share/TimeUtils.hpp"
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <future>
#include <regex>
#ifdef _WIN32
#include <process.h>
#define reconcile_pid _getpid
#else
#include <unistd.h>
#define reconcile_pid getpid
#endif

USING_NS_WTP;

bool WtCtaEngine::request_strategy_reset(const char* strategy, const char* trader,
    const char* request, const char* contracts, bool query_only, const char* metadata)
{
    if (!strategy || !trader || !request || !contracts ||
        !std::regex_match(request, std::regex("[A-Za-z0-9_-]{1,100}"))) return false;
    rapidjson::Document data;
    data.Parse(contracts);
    if (data.HasParseError() || !data.IsArray() || data.Empty()) return false;
    auto job = std::make_shared<ResetJob>();
    job->query_only = query_only; if (metadata) job->metadata = metadata;
    job->strategy = strategy; job->trader = trader; job->request = request;
    for (auto& value : data.GetArray()) {
        if (!value.IsString() || !std::regex_match(value.GetString(),
                std::regex("[A-Z]+\\.[A-Za-z0-9]+\\.[0-9]{4}"))) return false;
        job->contracts.emplace_back(value.GetString());
    }
    std::lock_guard<std::mutex> lock(_reset_jobs_mutex);
    for (const auto& existing : _reset_jobs) {
        if (existing->request == request)
            return existing->strategy == strategy && existing->trader == trader && existing->contracts == job->contracts &&
                existing->query_only == query_only && existing->metadata == job->metadata;
        if (existing->status == "running" || (existing->init_requested && !existing->init_dispatched)) return false;
    }
    _reset_jobs.push_back(job); _reset_pending = true;
    return true; // The event thread owns all subsequent mutations.
}

bool WtCtaEngine::configure_strategy_reconcile(const char* boot, const char* revision)
{
    if (!boot || !revision || !std::regex_match(boot, std::regex("[A-Za-z0-9_-]{1,100}")) ||
        !std::regex_match(revision, std::regex("[A-Za-z0-9_-]{1,100}"))) return false;
    std::lock_guard<std::mutex> lock(_reset_jobs_mutex);
    if (!_reconcile_boot.empty()) return _reconcile_boot == boot && _reconcile_revision == revision;
    _reconcile_boot = boot; _reconcile_revision = revision; return true;
}

bool WtCtaEngine::request_strategy_reconcile(const char* json)
{
    rapidjson::Document request; request.Parse(json ? json : "");
    if (request.HasParseError() || !request.IsObject()) return false;
    for (const char* key : {"request_id","expected_boot_id","plan_revision","strategyid","trader","purpose"})
        if (!request.HasMember(key) || !request[key].IsString()) return false;
    if (!request.HasMember("expected_pid") || !request["expected_pid"].IsUint64() ||
        request["expected_pid"].GetUint64() != static_cast<uint64_t>(reconcile_pid()) ||
        !request.HasMember("requested_at_ms") || !request["requested_at_ms"].IsUint64() ||
        !request.HasMember("contracts") || !request["contracts"].IsArray()) return false;
    const auto requested = request["requested_at_ms"].GetUint64();
    const auto now = static_cast<uint64_t>(TimeUtils::getLocalTimeNow());
    if (requested > now || now - requested > 15000) return false;
    const std::string purpose = request["purpose"].GetString();
    if (purpose != "prepare" && purpose != "before_stop" && purpose != "rollback") return false;
    { std::lock_guard<std::mutex> lock(_reset_jobs_mutex);
      if (_reconcile_boot.empty() || request["expected_boot_id"].GetString() != _reconcile_boot ||
          request["plan_revision"].GetString() != _reconcile_revision) return false; }
    rapidjson::StringBuffer contracts; rapidjson::Writer<rapidjson::StringBuffer> out(contracts);
    request["contracts"].Accept(out);
    return request_strategy_reset(request["strategyid"].GetString(), request["trader"].GetString(),
        request["request_id"].GetString(), contracts.GetString(), true, json);
}

bool WtCtaEngine::arm_strategy_startup_guard(const char* nonce)
{
    if (!nonce || !std::regex_match(nonce, std::regex("[A-Za-z0-9_-]{1,100}"))) return false;
    std::lock_guard<std::recursive_mutex> events(_reset_events);
    std::lock_guard<std::mutex> jobs(_reset_jobs_mutex);
    if (_reconcile_boot.empty() || !_ctx_map.empty() || _reset_pending || !_adapter_mgr ||
        _adapter_mgr->getAdapters().size() != 1 || !_startup_nonce.empty()) return false;
    std::shared_ptr<WtLocalExecuter> local; size_t count = 0;
    _exec_mgr.enum_executer([&](ExecCmdPtr value) { ++count; local = std::dynamic_pointer_cast<WtLocalExecuter>(value); });
    if (count != 1 || !local || !local->reset_trader()) return false;
    _startup_nonce = nonce; _startup_guard = true;
    local->reset_trader()->startup_guard(true); local->freeze_for_reset(true);
    return true; // Called before external contexts are created and strategy_init/run execute.
}

const char* WtCtaEngine::query_strategy_startup_guard()
{
    static thread_local std::string response;
    std::lock_guard<std::mutex> lock(_reset_jobs_mutex);
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    out.StartObject(); out.Key("capable"); out.Bool(true); out.Key("active"); out.Bool(_startup_guard);
    out.Key("nonce"); out.String(_startup_nonce.c_str());
    out.Key("plan_revision"); out.String(_reconcile_revision.c_str()); out.EndObject();
    response = buffer.GetString(); return response.c_str();
}

bool WtCtaEngine::release_strategy_startup_guard(const char* nonce)
{
    std::lock_guard<std::recursive_mutex> events(_reset_events);
    if (_pool) _pool->wait();
    std::lock_guard<std::mutex> jobs(_reset_jobs_mutex);
    if (!_startup_guard || !nonce || _startup_nonce != nonce || _reset_pending || _reset_jobs.empty()) return false;
    const auto& job = _reset_jobs.back();
    if (!job->query_only || job->status != "completed" || job->snapshot.empty()) return false;
    rapidjson::Document metadata; metadata.Parse(job->metadata.c_str());
    if (std::string(metadata["purpose"].GetString()) != "prepare") return false;
    const auto now = TimeUtils::getLocalTimeNow();
    if (job->completed > now || now - job->completed > 3000) return false;
    rapidjson::Document snapshot; snapshot.Parse(job->snapshot.c_str());
    if (!snapshot["query_complete"].GetBool() || !snapshot["execution_idle"].GetBool() ||
        !snapshot["positions"].Empty() || !snapshot["alive_orders"].Empty() ||
        !job->executor->reset_trader()->startup_release_ready(snapshot["event_seq"].GetUint64())) return false;
    static_cast<CtaStraBaseCtx*>(job->context.get())->freeze_for_reset(false);
    job->executor->freeze_for_reset(false);
    job->executor->reset_trader()->startup_guard(false);
    _startup_guard = false; return true;
}

const char* WtCtaEngine::query_strategy_reset(const char* request)
{
    static thread_local std::string response;
    std::lock_guard<std::mutex> lock(_reset_jobs_mutex);
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    auto field = [&](const char* key, const std::string& value) { writer.Key(key); writer.String(value.c_str()); };
    std::shared_ptr<ResetJob> job;
    for (const auto& candidate : _reset_jobs) if (candidate->request == (request ? request : "")) job = candidate;
    if (job) {
        field("status", job->status); field("step", job->step); field("message", job->message);
        field("error", job->error); field("archive_path", job->archive); field("request_id", job->request);
        if (job->query_only) {
            rapidjson::Document metadata; metadata.Parse(job->metadata.c_str());
            writer.Key("schema"); writer.Int(1); writer.Key("pid"); writer.Uint64(reconcile_pid());
            field("boot_id", _reconcile_boot); field("plan_revision", _reconcile_revision);
            field("purpose", metadata["purpose"].GetString());
            writer.Key("completed_at_ms"); writer.Uint64(job->completed);
            if (job->snapshot.empty()) {
                for (const char* key : {"query_complete","positions_complete","orders_complete","ready","connected","execution_idle"})
                    { writer.Key(key); writer.Bool(false); }
            } else {
                rapidjson::Document snapshot; snapshot.Parse(job->snapshot.c_str());
                auto* adapter = job->executor->reset_trader();
                if (!adapter->isReady() || adapter->reconcile_event_seq() != snapshot["event_seq"].GetUint64()) {
                    snapshot["query_complete"].SetBool(false); snapshot["ready"].SetBool(false);
                }
                for (auto it = snapshot.MemberBegin(); it != snapshot.MemberEnd(); ++it) {
                    writer.Key(it->name.GetString()); it->value.Accept(writer);
                }
            }
        }
    } else { field("status", "failed"); field("step", "failed"); field("error", "reset request does not exist"); }
    writer.EndObject(); response = buffer.GetString();
    if (job && job->query_only && job->status == "completed") {
        rapidjson::Document metadata; metadata.Parse(job->metadata.c_str());
        if (std::string(metadata["purpose"].GetString()) != "prepare")
            job->executor->reset_trader()->retain_reconcile_proof(response);
    }
    return response.c_str();
}

bool WtCtaEngine::wakeup_strategy_reset(const char* request)
{
    std::lock_guard<std::mutex> lock(_reset_jobs_mutex);
    if (!request || _reset_jobs.empty()) return false;
    const auto& job = _reset_jobs.back();
    if (job->request != request || job->status != "completed" || !job->context) return false;
    if (!job->init_dispatched) { job->init_requested = true; _reset_pending = true; }
    return true;
}

void WtCtaEngine::process_strategy_resets()
{
    if (!_reset_pending) return;
    std::lock_guard<std::recursive_mutex> eventLock(_reset_events);
    if (_pool) _pool->wait();
    std::unique_lock<std::mutex> jobsLock(_reset_jobs_mutex);
    std::shared_ptr<ResetJob> job;
    for (const auto& candidate : _reset_jobs) if (candidate->status == "running") { job = candidate; break; }
    if (!job) {
        for (const auto& candidate : _reset_jobs) {
            if (candidate->status == "completed" && candidate->init_requested && !candidate->init_dispatched) {
                candidate->init_dispatched = true;
                auto context = candidate->context;
                _reset_pending = false;
                jobsLock.unlock(); // Python may query this job while holding its own gate lock.
                uint32_t date, time; TimeUtils::getDateTime(date, time);
                static_cast<CtaStraBaseCtx*>(context.get())->pump_reset_callback(date, time / 100000);
                return;
            }
        }
        _reset_pending = false; return;
    }
    auto fail = [&](const std::string& error) {
        job->status = job->step = "failed"; job->error = job->message = error;
        if (job->executor && job->begun && (!job->query_only || job->adapter_begun))
            job->executor->reset_trader()->fail_strategy_reset();
        if (job->query_only && job->begun) {
            job->executor->freeze_for_reset(job->previous_executor_frozen);
            static_cast<CtaStraBaseCtx*>(job->context.get())->freeze_for_reset(job->previous_context_frozen);
        }
        // A started reset never automatically unfreezes on failure.
    };
    const auto now = TimeUtils::getLocalTimeNow();
    if (!job->begun) {
        // Conservative first version: one CTA context, one dedicated local executor/channel.
        if (_ctx_map.size() != 1 || !_adapter_mgr) { fail("reset requires an exclusive CTA trading channel"); return; }
        job->context = _ctx_map.begin()->second;
        auto* context = dynamic_cast<CtaStraBaseCtx*>(job->context.get());
        const auto& route = _exec_mgr.get_route(job->strategy.c_str());
        size_t executors = 0;
        _exec_mgr.enum_executer([&](ExecCmdPtr executor) {
            ++executors;
            auto local = std::dynamic_pointer_cast<WtLocalExecuter>(executor);
            if (local && route.size() == 1 && route.count(executor->name())) job->executor = local;
        });
        if (!context || job->strategy != context->name() || executors != 1 || !job->executor ||
            !job->executor->reset_trader() || job->trader != job->executor->reset_trader()->id() ||
            !job->executor->reset_trader()->exclusive_reset_sink(job->executor.get())) {
            fail("strategy/executor/channel ownership is not exclusive"); return;
        }
        job->begun = true; job->started = now;
        job->previous_context_frozen = context->reset_frozen();
        job->previous_executor_frozen = job->executor->reset_frozen();
        context->freeze_for_reset(true); job->executor->freeze_for_reset(true);
        if (!job->executor->reset_trader()->begin_strategy_reset(job->request, job->contracts, job->error, job->query_only)) {
            fail(job->error); return;
        }
        job->adapter_begun = true;
        job->message = "decisions and old executor targets frozen";
    }
    if (now - job->started > (job->query_only ? 10000u : 120000u)) { fail("administrative query timed out"); return; }
    auto* adapter = job->executor->reset_trader();
    const auto step = adapter->advance_strategy_reset(now, [this, now](const std::string& code, bool buy) {
        auto* tick = _data_mgr->grab_last_tick(code.c_str());
        if (!tick) return 0.0;
        const auto time = TimeUtils::makeTime(tick->actiondate(), tick->actiontime());
        const auto age = static_cast<int64_t>(now) - static_cast<int64_t>(time);
        const double price = (age >= -1000 && age <= 5000) ? (buy ? tick->askprice(0) : tick->bidprice(0)) : 0.0;
        tick->release(); return price;
    }, job->error);
    job->step = step;
    if (step == "failed") { fail(job->error); return; }
    if (step != "ready") return;
    if (job->query_only) {
        bool idle = static_cast<CtaStraBaseCtx*>(job->context.get())->reconcile_idle() &&
            job->executor->reconcile_idle() && _exec_mgr.reconcile_idle();
        for (const auto& signal : _sig_map) if (signal.second._volume != 0) idle = false;
        for (const auto& position : _pos_map) {
            SpinLock lock(position.second->_mtx);
            if (position.second->_volume != 0) idle = false;
            for (const auto& detail : position.second->_details) if (detail._volume != 0) idle = false;
        }
        job->snapshot = adapter->reconcile_snapshot(idle);
        adapter->finish_strategy_reset();
        job->executor->freeze_for_reset(job->previous_executor_frozen);
        static_cast<CtaStraBaseCtx*>(job->context.get())->freeze_for_reset(job->previous_context_frozen);
        job->status = job->step = "completed"; job->completed = now;
        job->message = "read-only broker reconciliation completed; original native gates restored";
        _reset_pending = false; return;
    }
    job->step = "archiving";
    job->archive = std::string(WtHelper::getOutputDir()) + "strategy-reset/" + job->request;
    auto* context = static_cast<CtaStraBaseCtx*>(job->context.get());
    if (!context->archive_and_reset(job->archive, job->error)) { fail(job->error); return; }
    job->executor->clear_reset_targets(); _exec_mgr.clear_cached_targets(); _sig_map.clear();
    // Portfolio valuation tasks share this map. Queue the zeroing behind existing tasks
    // and keep map entries alive; no callback can retain an erased map element.
    auto cleared = std::make_shared<std::promise<void>>(); auto done = cleared->get_future();
    push_task([this, cleared]() {
        for (auto& entry : _pos_map) {
            SpinLock lock(entry.second->_mtx);
            entry.second->_volume = 0; entry.second->_dynprofit = 0; entry.second->_details.clear();
        }
        if (_port_fund) _port_fund->fundInfo()._dynprofit = 0;
        cleared->set_value();
    });
    done.wait(); save_datas();
    if (!adapter->finish_strategy_reset(&job->error)) { fail(job->error); return; }
    job->executor->freeze_for_reset(false); context->freeze_for_reset(false);
    job->status = job->step = "completed";
    job->message = "broker confirmed flat; old strategy state archived; Python decisions remain paused";
    _reset_pending = false;
}
