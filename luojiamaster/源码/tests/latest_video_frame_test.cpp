#include "app/latest_video_frame.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

struct Frame {
    int id;
    std::vector<unsigned char> pixels;
    std::unique_ptr<int> lifetime;
    std::function<void()> on_destroy;
    explicit Frame(int value) : id(value), pixels(64, static_cast<unsigned char>(value)), lifetime(std::make_unique<int>(value)) {}
    Frame(const Frame &) = delete;
    Frame &operator=(const Frame &) = delete;
    Frame(Frame &&) noexcept = default;
    Frame &operator=(Frame &&) noexcept = default;
    ~Frame() { if (lifetime && on_destroy) on_destroy(); }
};

using Mailbox = RoboGo::app::LatestVideoFrameMailbox<Frame>;

int main() {
    try {
        Mailbox mailbox;
        CHECK(!mailbox.Take().has_value());
        std::atomic<bool> destruction_saw_unlocked{false};
        Frame first(1);
        first.on_destroy = [&]() {
            const auto state = mailbox.Stats(); // deadlocks if destruction holds its mutex
            destruction_saw_unlocked.store(state.pending == 1 && state.submitted == 2);
        };
        mailbox.Push(std::move(first));
        mailbox.Push(Frame(2));
        CHECK(destruction_saw_unlocked.load());
        auto latest = mailbox.Take();
        CHECK(latest && latest->id == 2 && latest->pixels[0] == 2);
        CHECK(mailbox.Stats().pending == 0);
        CHECK(mailbox.Stats().overwritten == 1);
        CHECK(mailbox.Stats().taken == 1);
        latest.reset();

        // A paused consumer must retain exactly one image, regardless of input length.
        Mailbox paused;
        for (int id = 0; id < 10000; ++id) {
            paused.Push(Frame(id));
            CHECK(paused.Stats().pending == 1);
        }
        CHECK(paused.Stats().overwritten == 9999);
        CHECK(paused.Take()->id == 9999);
        paused.Push(Frame(10000));
        paused.Clear();
        CHECK(!paused.Take());

        Mailbox concurrent;
        std::atomic<bool> finished{false};
        std::atomic<bool> failed{false};
        int last_id = -1;
        std::thread consumer([&]() {
            try {
                while (!finished.load(std::memory_order_acquire) || concurrent.Stats().pending != 0) {
                    auto frame = concurrent.Take();
                    if (frame) {
                        CHECK(frame->id > last_id);
                        CHECK(frame->pixels.size() == 64 && frame->lifetime);
                        last_id = frame->id;
                    } else std::this_thread::yield();
                }
            } catch (...) { failed.store(true); }
        });
        for (int id = 0; id < 20000; ++id) concurrent.Push(Frame(id));
        finished.store(true, std::memory_order_release);
        consumer.join();
        const auto stats = concurrent.Stats();
        CHECK(!failed.load());
        CHECK(last_id == 19999);
        CHECK(stats.submitted == 20000 && stats.pending == 0);
        CHECK(stats.taken + stats.overwritten == stats.submitted);
        std::printf("PASS latest mailbox: move-only ownership; lock-free destruction; paused pending<=1; concurrent submitted=%llu taken=%llu overwritten=%llu\n",
                    static_cast<unsigned long long>(stats.submitted),
                    static_cast<unsigned long long>(stats.taken),
                    static_cast<unsigned long long>(stats.overwritten));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL latest mailbox: %s\n", error.what());
        return 1;
    }
}
