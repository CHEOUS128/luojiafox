#pragma once
#include "cw_trial.hpp"
#include <atomic>
#include <functional>
#include <thread>

namespace io::cw {
// Bounded handoff: one whole frame outside VideoSender, one packet in serial queue.
// The sink returns true only after taking ownership. false retries the SAME bytes.
class TxWorker {
public:
  using Packet = std::vector<std::uint8_t>;
  using Bundle = std::vector<Packet>;
  TxWorker(std::function<Bundle()> pull, std::function<bool(const Packet&)> push,
           std::function<void()> service)
    : pull_(std::move(pull)), push_(std::move(push)), service_(std::move(service)),
      thread_([this] { run(); }) {}
  ~TxWorker() { stop(); }
  TxWorker(const TxWorker&) = delete;
  TxWorker& operator=(const TxWorker&) = delete;
  void stop() { quit_ = true; if (thread_.joinable()) thread_.join(); }
  bool failed() const { return failed_.load(); }
private:
  void run() noexcept {
    try {
      Bundle b; std::size_t index = 0; PacketGate gate;
      while (!quit_) {
        service_();
        if (index == b.size()) { b = pull_(); index = 0; }
        if (b.size() > frame_packets) throw std::runtime_error("cw bundle exceeds two packets");
        if (index < b.size() && gate.ready(PacketGate::Clock::now())) {
          if (b[index].size() != 309) throw std::runtime_error("cw packet is not 309 bytes");
          if (push_(b[index])) { ++index; gate.completed(PacketGate::Clock::now()); }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      // Stop discards not-yet-accepted bytes. No accelerated end-of-file flush.
    } catch (...) { failed_ = true; }
  }
  std::function<Bundle()> pull_;
  std::function<bool(const Packet&)> push_;
  std::function<void()> service_;
  std::atomic<bool> quit_{false}, failed_{false};
  std::thread thread_;
};
}
