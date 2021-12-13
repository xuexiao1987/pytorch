#include <torch/csrc/monitor/events.h>

#include <shared_mutex>
#include <sstream>
#include <unordered_set>
#include <vector>
#include <algorithm>

namespace torch {
namespace monitor {

namespace {
class EventHandlers {
 public:
  void registerEventHandler(std::shared_ptr<EventHandler> handler) noexcept {
    std::unique_lock<std::shared_timed_mutex> lock(mu_);

    handlers_.emplace_back(std::move(handler));
  }

  void unregisterEventHandler(
      const std::shared_ptr<EventHandler>& handler) noexcept {
    std::unique_lock<std::shared_timed_mutex> lock(mu_);

    auto it = std::find(handlers_.begin(), handlers_.end(), handler);
    handlers_.erase(it);
  }

  void logEvent(const Event& e) {
    std::shared_lock<std::shared_timed_mutex> lock(mu_);

    for (auto& handler : handlers_) {
      handler->handle(e);
    }
  }

  static EventHandlers& get() noexcept {
    static EventHandlers ehs;
    return ehs;
  }

 private:
  std::shared_timed_mutex mu_{};
  std::vector<std::shared_ptr<EventHandler>> handlers_{};
};
} // namespace

void logEvent(const Event& e) {
  EventHandlers::get().logEvent(e);
}

void registerEventHandler(std::shared_ptr<EventHandler> p) {
  EventHandlers::get().registerEventHandler(std::move(p));
}

void unregisterEventHandler(const std::shared_ptr<EventHandler>& p) {
  EventHandlers::get().unregisterEventHandler(p);
}

} // namespace monitor
} // namespace torch
