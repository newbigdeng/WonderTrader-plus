#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace wtp {
struct PortfolioPosition { double long_qty = 0, short_qty = 0; };
struct PortfolioOrder { std::string code, order_id, entrust_id; };
struct PortfolioView {
    bool connected = false, positions_complete = false, orders_complete = false;
    uint32_t trading_date = 0;
    uint64_t event_seq = 0, pending_submissions = 0;
    std::map<std::string, PortfolioPosition> positions;
    std::map<std::string, PortfolioOrder> orders;
};

// Only callback publications own this mutex. Readers never touch mutable broker caches.
class PortfolioState {
public:
    struct Mutation {
        std::atomic<unsigned>& active;
        explicit Mutation(std::atomic<unsigned>& value) : active(value) { ++active; }
        ~Mutation() { --active; }
        Mutation(const Mutation&) = delete;
    };
    Mutation mutation() { return Mutation(_active); }
    template<class Update> void update(Update action) {
        std::lock_guard<std::mutex> lock(_mutex);
        action(_view); ++_view.event_seq;
    }
    bool snapshot(PortfolioView& result) {
        if (_active.load() != 0) return false;
        std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
        if (!lock || _active.load() != 0) return false;
        result = _view;
        return _active.load() == 0;
    }
private:
    friend struct NativePortfolioTestAccessor;
    std::atomic<unsigned> _active{0};
    std::mutex _mutex;
    PortfolioView _view;
};
}
