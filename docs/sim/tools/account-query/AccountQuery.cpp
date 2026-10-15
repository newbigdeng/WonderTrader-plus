// One-shot SimNow account query using the official CTP Trader API.
#include "ThostFtdcTraderApi.h"
#include <chrono>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iconv.h>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

volatile std::sig_atomic_t interrupted = 0;
void handleSignal(int) { interrupted = 1; }

std::string now() {
    const auto point = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(point);
    std::tm local{}; localtime_r(&seconds, &local);
    char text[40]; std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %z", &local);
    return text;
}
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::map<std::string, std::string> readConfig(const char* filename) {
    std::ifstream input(filename);
    if (!input) throw std::runtime_error("Cannot read config file");
    std::string line, section; std::map<std::string, std::string> values;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') { section = line; continue; }
        const auto separator = line.find('=');
        if (section == "[ctp]" && separator != std::string::npos)
            values[trim(line.substr(0, separator))] = trim(line.substr(separator + 1));
    }
    for (const auto* key : {"front", "broker", "user", "pass", "appid", "authcode"})
        if (values[key].empty()) throw std::runtime_error(std::string("Missing [ctp] key: ") + key);
    if (values["broker"] != "9999" || values["front"] != "tcp://182.254.243.31:30003")
        throw std::runtime_error("This utility is restricted to the configured SimNow first environment (9999 / 30003)");
    return values;
}
template<class T> void copyField(T& destination, const std::string& value) {
    if (value.size() >= sizeof(destination)) throw std::runtime_error("Configuration value exceeds SDK field capacity");
    std::memset(destination, 0, sizeof(destination));
    std::memcpy(destination, value.data(), value.size());
}
std::string ctpError(const char* message) {
    if (!message || !*message) return "";
    iconv_t converter = iconv_open("UTF-8", "GB18030");
    if (converter == reinterpret_cast<iconv_t>(-1)) return "CTP error text unavailable";
    std::string result(std::strlen(message) * 4 + 1, '\0');
    char* source = const_cast<char*>(message); size_t sourceLeft = std::strlen(message);
    char* target = &result[0]; size_t targetLeft = result.size();
    const bool ok = iconv(converter, &source, &sourceLeft, &target, &targetLeft) != static_cast<size_t>(-1);
    iconv_close(converter);
    if (!ok) return "CTP error text could not be decoded";
    result.resize(result.size() - targetLeft); return result;
}
class Log {
    std::ofstream file; std::mutex mutex;
public:
    Log() : file("account_query.log", std::ios::app) {
        if (!file) throw std::runtime_error("Cannot open ./account_query.log");
    }
    void write(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        const std::string line = "[" + now() + "] " + message;
        file << line << '\n'; file.flush();
        std::cout << line << std::endl;
    }
    bool good() { return file.good(); }
};
struct Funds {
    std::string currency, day;
    int settlement = 0;
    std::map<std::string, double> amounts;
};
class AccountQuery : public CThostFtdcTraderSpi {
    Log& log; std::map<std::string, std::string> cfg;
    std::mutex mutex; std::condition_variable changed;
    bool finished = false, success = false;
    int request = 0, queryRequest = 0, error = 0;
    std::string stage = "connecting", loginDay;
    std::vector<Funds> records;

    void step(const std::string& value) { std::lock_guard<std::mutex> lock(mutex); if (!finished) stage = value; }
    bool stopped() { std::lock_guard<std::mutex> lock(mutex); return finished; }
    void finish(bool ok, const std::string& value, int code = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        if (finished) return;
        success = ok; stage = value; error = code; finished = true; changed.notify_all();
    }
    bool responseError(CThostFtdcRspInfoField* info) {
        if (!info || info->ErrorID == 0) return false;
        log.write("CTP 回调错误 ErrorID=" + std::to_string(info->ErrorID) + " ErrorMsg=" + ctpError(info->ErrorMsg));
        finish(false, "response_error", info->ErrorID); return true;
    }
    void submitted(const char* name, int result, int id) {
        log.write(std::string(name) + " request_id=" + std::to_string(id) + " return=" + std::to_string(result));
        if (result != 0) finish(false, "request_rejected", result);
    }
public:
    CThostFtdcTraderApi* api = nullptr;
    AccountQuery(Log& sink, std::map<std::string, std::string> config) : log(sink), cfg(std::move(config)) {
        // Validate all credential lengths before entering SDK callback threads.
        CThostFtdcReqAuthenticateField a{}; CThostFtdcReqUserLoginField b{};
        copyField(a.BrokerID, cfg["broker"]); copyField(a.UserID, cfg["user"]);
        copyField(a.AppID, cfg["appid"]); copyField(a.AuthCode, cfg["authcode"]);
        copyField(b.Password, cfg["pass"]);
    }
    void OnFrontConnected() override {
        if (stopped()) return;
        log.write("OnFrontConnected：交易前置已连接，开始认证"); step("authentication");
        CThostFtdcReqAuthenticateField field{};
        copyField(field.BrokerID, cfg["broker"]); copyField(field.UserID, cfg["user"]);
        copyField(field.AppID, cfg["appid"]); copyField(field.AuthCode, cfg["authcode"]);
        const int id = ++request; submitted("ReqAuthenticate", api->ReqAuthenticate(&field, id), id);
    }
    void OnFrontDisconnected(int reason) override {
        log.write("OnFrontDisconnected reason=" + std::to_string(reason));
        finish(false, "disconnected", reason);
    }
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* info, int id, bool last) override {
        log.write("OnRspAuthenticate request_id=" + std::to_string(id) + " bIsLast=" + std::to_string(last));
        if (responseError(info) || stopped() || !last) return;
        step("login"); CThostFtdcReqUserLoginField field{};
        copyField(field.BrokerID, cfg["broker"]); copyField(field.UserID, cfg["user"]); copyField(field.Password, cfg["pass"]);
        const int next = ++request; submitted("ReqUserLogin", api->ReqUserLogin(&field, next), next);
    }
    void OnRspUserLogin(CThostFtdcRspUserLoginField* field, CThostFtdcRspInfoField* info, int id, bool last) override {
        log.write("OnRspUserLogin request_id=" + std::to_string(id) + " bIsLast=" + std::to_string(last));
        if (responseError(info) || stopped() || !last) return;
        if (!field) { finish(false, "empty_login_response"); return; }
        loginDay = field->TradingDay; log.write("登录成功 TradingDay=" + loginDay);
        step("query_account"); CThostFtdcQryTradingAccountField query{};
        copyField(query.BrokerID, cfg["broker"]); copyField(query.InvestorID, cfg["user"]);
        copyField(query.CurrencyID, std::string("CNY"));
        queryRequest = ++request;
        submitted("ReqQryTradingAccount", api->ReqQryTradingAccount(&query, queryRequest), queryRequest);
    }
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* field, CThostFtdcRspInfoField* info, int id, bool last) override {
        log.write("OnRspQryTradingAccount request_id=" + std::to_string(id) + " bIsLast=" + std::to_string(last));
        if (id != queryRequest || stopped()) return;
        if (responseError(info)) return;
        if (field) {
            Funds funds; funds.currency = field->CurrencyID; funds.day = field->TradingDay; funds.settlement = field->SettlementID;
            funds.amounts = {{"PreBalance", field->PreBalance}, {"Balance", field->Balance}, {"Available", field->Available},
                {"CurrMargin", field->CurrMargin}, {"FrozenMargin", field->FrozenMargin}, {"FrozenCash", field->FrozenCash},
                {"FrozenCommission", field->FrozenCommission}, {"Commission", field->Commission},
                {"CloseProfit", field->CloseProfit}, {"PositionProfit", field->PositionProfit},
                {"Deposit", field->Deposit}, {"Withdraw", field->Withdraw}, {"WithdrawQuota", field->WithdrawQuota},
                {"Reserve", field->Reserve}, {"Credit", field->Credit}, {"ExchangeMargin", field->ExchangeMargin}};
            for (const auto& amount : funds.amounts) if (!std::isfinite(amount.second)) {
                log.write("资金字段不是有限数：" + amount.first); finish(false, "invalid_account_data"); return;
            }
            log.write("资金记录 CurrencyID=" + funds.currency + " TradingDay=" + funds.day + " SettlementID=" + std::to_string(funds.settlement));
            for (const auto& amount : funds.amounts) {
                std::ostringstream line; line << std::fixed << std::setprecision(6) << "  " << amount.first << '=' << amount.second;
                log.write(line.str());
            }
            records.push_back(std::move(funds));
        }
        if (last) {
            bool hasCny = false; for (const auto& row : records) hasCny = hasCny || row.currency == "CNY";
            finish(hasCny, hasCny ? "complete" : "empty_account_response");
        }
    }
    void OnRspError(CThostFtdcRspInfoField* info, int, bool) override { responseError(info); }
    void wait() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
        std::unique_lock<std::mutex> lock(mutex);
        while (!finished && !interrupted && std::chrono::steady_clock::now() < deadline)
            changed.wait_for(lock, std::chrono::milliseconds(250));
        if (!finished) {
            success = false; stage = interrupted ? "interrupted" : "timeout";
            error = interrupted ? -998 : -999; finished = true;
        }
    }
    bool save() {
        // Called after Release has joined SDK threads. All queried currencies have been collected.
        std::ofstream out("account_latest.json.tmp"); out << std::setprecision(17);
        out << "{\n  \"result\":\"" << (success ? "passed" : "failed") << "\",\n  \"queried_at\":\"" << now()
            << "\",\n  \"stage\":\"" << stage << "\",\n  \"error_id\":" << error << ",\n  \"login_trading_day\":\"" << loginDay << "\",\n  \"accounts\":[";
        for (size_t index = 0; index < records.size(); ++index) {
            const auto& row = records[index];
            out << (index ? "," : "") << "\n    {\"CurrencyID\":\"" << row.currency << "\",\"TradingDay\":\"" << row.day
                << "\",\"SettlementID\":" << row.settlement;
            for (const auto& value : row.amounts) out << ",\"" << value.first << "\":" << value.second;
            out << '}';
        }
        out << "\n  ]\n}\n"; out.flush();
        if (!out.good()) throw std::runtime_error("Could not write account_latest.json.tmp");
        out.close(); if (std::rename("account_latest.json.tmp", "account_latest.json") != 0) throw std::runtime_error("Could not publish account_latest.json");
        log.write(std::string("RESULT=") + (success ? "passed" : "failed") + " stage=" + stage + " error_id=" + std::to_string(error));
        return success && log.good();
    }
};
int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string(argv[1]) == "--help")) {
        std::cout << "Usage: ./AccountQuery [config.ini]\nQueries CNY funds once; writes ./account_query.log and ./account_latest.json.\n";
        return argc > 2 ? 2 : 0;
    }
    umask(0077); std::signal(SIGINT, handleSignal); std::signal(SIGTERM, handleSignal);
    try {
        Log log;
        try {
            auto config = readConfig(argc == 2 ? argv[1] : "config.ini");
            log.write("BEGIN pid=" + std::to_string(getpid()) + " SDK=" + CThostFtdcTraderApi::GetApiVersion() + " front=" + config["front"]);
            AccountQuery query(log, config);
            if (mkdir("flow", 0700) != 0 && errno != EEXIST) throw std::runtime_error("Cannot create ./flow");
            query.api = CThostFtdcTraderApi::CreateFtdcTraderApi("flow/", true);
            if (!query.api) throw std::runtime_error("CreateFtdcTraderApi failed");
            query.api->RegisterSpi(&query);
            query.api->SubscribePrivateTopic(THOST_TERT_QUICK); query.api->SubscribePublicTopic(THOST_TERT_QUICK);
            query.api->RegisterFront(const_cast<char*>(config["front"].c_str())); query.api->Init();
            query.wait();
            // Release on the main thread, never inside an SDK callback.
            query.api->RegisterSpi(nullptr); query.api->Release(); query.api = nullptr;
            return query.save() ? 0 : 1;
        } catch (const std::exception& error) {
            log.write(std::string("FATAL: ") + error.what());
            std::ofstream failed("account_latest.json.tmp");
            failed << "{\"result\":\"failed\",\"queried_at\":\"" << now() << "\",\"stage\":\"startup_or_output_error\",\"accounts\":[]}\n";
            failed.flush(); const bool saved = failed.good(); failed.close();
            if (saved) std::rename("account_latest.json.tmp", "account_latest.json");
            return 2;
        }
    } catch (const std::exception& error) { std::cerr << "FATAL: " << error.what() << '\n'; return 2; }
}
