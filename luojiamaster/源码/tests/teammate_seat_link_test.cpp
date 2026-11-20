#include "net/teammate_link.h"
#include <iostream>
#include "video/teammate_video_receiver.h"
#include "app/latest_video_frame.h"
#include "panel/commander_panel.h"
#include "platform/platform_util.h"

using namespace std::chrono_literals;
using namespace RoboGo::net;
using RoboGo::app::Competition;

namespace {
using Bytes = std::vector<std::uint8_t>;

template<class Predicate> bool Wait(Predicate predicate, std::chrono::milliseconds limit = 3500ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

struct Inbox {
    std::mutex mutex;
    std::vector<Bytes> frames;
    void put(Bytes frame) {
        std::lock_guard lock(mutex);
        frames.push_back(std::move(frame));
    }
    bool contains(const Bytes &frame) {
        std::lock_guard lock(mutex);
        return std::find(frames.begin(), frames.end(), frame) != frames.end();
    }
    bool empty() {
        std::lock_guard lock(mutex);
        return frames.empty();
    }
};

struct Lane {
    Inbox inbox;
    std::unique_ptr<TeammateLink> link;
    void open(Competition competition, int local, int remote) {
        link = std::make_unique<TeammateLink>(TeamSeatLinkConfig(competition, local, remote, true),
            [this](Bytes &&bytes) { inbox.put(std::move(bytes)); });
    }
};

// Extract the first twelve HEVC NALs from the existing raw recording. Keeping
// individual VPS, SPS and PPS packets reproduces a camera that sends them separately.
std::vector<Bytes> RecordedNals() {
    namespace path = RoboGo::platform_util;
    const auto fixture = path::PathFromUtf8(__FILE__).parent_path() / path::PathFromUtf8("fixtures/测试画面.h265");
    auto input = path::OpenBinaryInput(path::PathToUtf8(fixture));
    if (!input) return {};
    Bytes data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 4 < data.size() && starts.size() < 13; ++i) {
        if (data[i] != 0 || data[i+1] != 0) continue;
        const std::size_t prefix = data[i+2] == 1 ? 3 : (data[i+2] == 0 && data[i+3] == 1 ? 4 : 0);
        if (prefix) { starts.push_back(i); i += prefix - 1; }
    }
    std::vector<Bytes> result;
    for (std::size_t i = 1; i < starts.size(); ++i)
        result.emplace_back(data.begin() + starts[i-1], data.begin() + starts[i]);
    return result;
}

bool RecordedSeat(Competition competition, int local, int remote, const std::vector<Bytes> &nals,
                  bool late_join) {
    if (nals.size() < 6) return false;
    std::atomic<unsigned> pictures{0};
    RoboGo::video::TeammateVideoReceiver decoder({}, [&](RoboGo::app::VideoFrame &&frame) {
        if (frame.width > 0 && frame.height > 0 && frame.Pixels()) ++pictures;
    });
    const auto tx_cfg = TeamSeatLinkConfig(competition, local, remote, true);
    const auto rx_cfg = TeamSeatLinkConfig(competition, remote, local, true);
    TeammateLink tx(tx_cfg, {});
    if (late_join) {
        for (int i = 0; i < 3; ++i) tx.push_frame(nals[i]);
        // No screen is connected yet. Force the initial VPS/SPS/PPS out of the
        // eight-frame queue, as when the second operator logs in a minute later.
        for (int i = 0; i < 16; ++i) tx.push_frame(nals[3]);
    }
    TeammateLink rx(rx_cfg, [&](Bytes &&data) { decoder.submit_frame(std::move(data)); });
    if (!Wait([&] { return tx.stats().peer_connected; })) return false;
    // Allow the uninteresting waiting queue to drain before the first IDR.
    if (!Wait([&] { return tx.stats().queued_now == 0; })) return false;
    for (std::size_t i = late_join ? 3 : 0; i < nals.size(); ++i) {
        tx.push_frame(nals[i]);
        std::this_thread::sleep_for(8ms);
    }
    const bool ok = Wait([&] { return pictures.load() != 0; }, 1200ms);
    const auto result = decoder.stats();
    std::cout << "hevc_late=" << late_join << " width=" << result.width << " height=" << result.height
              << " pictures=" << pictures << " context_opens=" << result.context_rebuilds << '\n';
    return ok;
}

bool CheckCommanderPreviewWidth(bool floating) {
    CommanderDeskState desk;
    desk.floating_cards = floating;
    desk.thumbnail_pixels = {650.f, 205.f};
    if (desk.PreviewWidth(0, 1920, 1080) != 384 || desk.PreviewWidth(0, 320, 180) != 0) return false;
    desk.zoom[0] = 2.f;
    if (desk.PreviewWidth(0, 1920, 1080) != 0) return false;
    desk.focused = 1;
    // A zoomed peer stays visible behind the focused card in floating mode.
    if (desk.PreviewWidth(0, 1920, 1080) != (floating ? 0 : 384) ||
        desk.PreviewWidth(1, 1920, 1080) != 0) return false;
    desk.focused = -1;
    return desk.PreviewWidth(1, 1920, 1080) == 384;
}

bool CheckCommanderConversion() {
    using namespace RoboGo;
    using Frame = app::VideoFrame;
    app::LatestVideoFrameMailbox<Frame> reference, pooled;
    video::TeammateVideoReceiver old({}, [&](Frame &&frame) { reference.Push(std::move(frame)); });
    video::TeammateVideoReceiver optimized({.reuse_aligned_rgba = true}, [&](Frame &&frame) { pooled.Push(std::move(frame)); });
    const auto fixtures = platform_util::PathFromUtf8(__FILE__).parent_path() / "fixtures";
    std::array<std::optional<Frame>, 3> retained;
    // The clips are independent HEVC streams at different output sizes.
    for (int clip = 1; clip <= 3; ++clip) {
        auto input = platform_util::OpenBinaryInput(platform_util::PathToUtf8(fixtures / ("peer-view-" + std::to_string(clip) + ".bin")));
        for (int i = 0; i < 5; ++i) {
            std::array<unsigned char, 4> head{};
            if (!input.read(reinterpret_cast<char *>(head.data()), 4)) return false;
            const auto length = (std::uint32_t(head[0]) << 24) | (std::uint32_t(head[1]) << 16)
                              | (std::uint32_t(head[2]) << 8) | head[3];
            if (length == 0 || length > 8 * 1024 * 1024) return false;
            Bytes packet(length);
            if (!input.read(reinterpret_cast<char *>(packet.data()), length)) return false;
            const bool thumb = clip == 1 && i == 1;
            optimized.SetPreviewWidth(thumb ? 128 : 0);
            old.submit_frame(Bytes(packet));
            optimized.submit_frame(std::move(packet));
            if (!Wait([&] { return reference.Stats().pending != 0; })) return false;
            auto a = reference.Take();
            if (clip == 2 && i == 3) {
                // Three retained CPU frames force exactly one drop, then release
                // a slot and require the following frame to match the old path.
                if (!Wait([&] { return optimized.stats().frames_dropped == 1; }) || pooled.Stats().pending) return false;
                retained[0].reset();
                continue;
            }
            if (!Wait([&] { return pooled.Stats().pending != 0; })) return false;
            auto b = pooled.Take();
            if (thumb) {
                const auto decoded = optimized.stats();
                if (!a || !b || decoded.width != a->width || decoded.height != a->height ||
                    b->width != 128 || b->height != 72) return false;
                continue;
            }
            if (!a || !b || a->shared_buffer || !b->shared_buffer || !b->Pixels() ||
                a->width != b->width || a->height != b->height) return false;
            for (int y = 0; y < b->height; ++y) {
                if (std::memcmp(a->Pixels() + y * a->RowPitch(), b->Pixels() + y * b->RowPitch(),
                                static_cast<size_t>(b->width) * 4) != 0) return false;
            }
            if (clip == 2 && i < 3) retained[i] = std::move(*b);
        }
        for (auto &frame : retained) frame.reset();
    }
    return optimized.stats().decode_errors == 0;
}

bool OfferHello(std::uint16_t port, const std::array<std::uint8_t, 8> &hello) {
    using tcp = boost::asio::ip::tcp;
    boost::asio::io_context io;
    return Wait([&] {
        tcp::socket socket(io);
        boost::system::error_code ec;
        socket.connect({boost::asio::ip::address_v4::loopback(), port}, ec);
        if (ec) return false;
        boost::asio::write(socket, boost::asio::buffer(hello), ec);
        // One framed payload. An incompatible handshake must never reach the callback.
        const std::array<std::uint8_t, 9> video{0,0,0,1,0,0,0,0,77};
        boost::asio::write(socket, boost::asio::buffer(video), ec);
        return true;
    });
}
}

int main() {
    int errors = 0;
    auto check = [&](bool value, const char *name) {
        std::cout << (value ? "PASS " : "FAIL ") << name << '\n';
        errors += !value;
    };
    check(CheckCommanderPreviewWidth(false), "split layout: focused video and zoom use expected preview sizes");
    check(CheckCommanderPreviewWidth(true), "floating layout: background zoomed card keeps its source resolution");
    check(CheckCommanderConversion(), "three real commander clips match legacy RGBA, bounded pool resumes after a drop");
    constexpr Competition ul = Competition::Ul2027, uc = Competition::Uc2027;
    check(TeamSeatIds(ul, 3) == std::vector<int>{1} && TeamSeatIds(ul, 101) == std::vector<int>{103},
          "UL opposite operator follows red/blue login");
    check(TeamSeatIds(uc, 104) == std::vector<int>({101,103}) &&
          TeamSeatIds(Competition::Uc2026, 3) == std::vector<int>({1,4}), "UC stable order both seasons");
    check(TeamSeatIds(Competition::Uc2026, 6).empty() && TeamSeatIds(ul, 4).empty() &&
          !TeamSeatLinkConfig(uc, 3, 101, true).seats.has_value(), "unsupported and opponent seats excluded");
    const auto config = TeamSeatLinkConfig(uc, 1, 3, true);
    Lane a3, a4, b1, b4, c1, c3;
    a3.open(uc, 1, 3);
    check(OfferHello(config.listen_port, {0x52,0x4d,0x54,0x4d,2,4,103,101}), "opponent rejected");
    check(OfferHello(config.listen_port, {0x52,0x4d,0x54,0x4d,2,0,3,1}), "other season rejected");
    check(OfferHello(config.listen_port, {0x52,0x4d,0x54,0x4d,2,4,4,1}), "other source seat rejected");
    check(OfferHello(config.listen_port, {0x52,0x4d,0x54,0x4d,1,0,0,0}), "anonymous older version rejected");
    for (int i = 0; i < 24; ++i) a3.link->push_frame({1, static_cast<std::uint8_t>(i)});
    const auto offline = a3.link->stats();
    check(offline.queued_now == 8 && offline.frames_dropped == 16, "missing peer queue bounded at 8");
    a4.open(uc,1,4); b1.open(uc,3,1); b4.open(uc,3,4); c1.open(uc,4,1); c3.open(uc,4,3);
    const std::array<Lane *,6> lanes{&a3,&a4,&b1,&b4,&c1,&c3};
    check(Wait([&] { return std::ranges::all_of(lanes, [](Lane *lane) {
        const auto stats = lane->link->stats();
        return stats.peer_connected && stats.peer_served;
    }); }), "three stations pair in six directions on loopback");
    check(a3.inbox.empty(), "no frames from any mismatched handshake");
    for (int i=0; i<6; ++i) lanes[i]->link->push_frame({42, static_cast<std::uint8_t>(i)});
    const std::array<int,6> source{2,4,0,5,1,3};
    check(Wait([&] {
        for (int i=0; i<6; ++i)
            if (!lanes[i]->inbox.contains({42,static_cast<std::uint8_t>(source[i])})) return false;
        return true;
    }), "six distinct payloads arrive at their logged-in seat");
    for (int i=0; i<6; ++i)
        for (int other=0; other<6; ++other)
            if (source[i] != other && lanes[i]->inbox.contains({42,static_cast<std::uint8_t>(other)})) ++errors;

    c1.link->set_local_inverted(true);
    c1.link->push_frame({4,91});
    check(Wait([&] { return a4.inbox.contains({4,91}) && a4.link->stats().peer_frame_inverted; })
          && !a3.link->stats().peer_frame_inverted, "rotation stays with source");
    c1.link.reset(); c3.link.reset();
    check(Wait([&] { return !a4.link->stats().peer_served && !b4.link->stats().peer_served; }),
          "disconnect clears two remote seats");
    b1.link->push_frame({3,92});
    check(Wait([&] { return a3.inbox.contains({3,92}); }), "remaining view survives independently");
    c1.open(uc,4,1); c3.open(uc,4,3);
    check(Wait([&] { return c1.link->stats().peer_connected && c3.link->stats().peer_connected; }),
          "same seat reconnects");
    c1.link->push_frame({4,93});
    check(Wait([&] { return a4.inbox.contains({4,93}); }) && !a3.inbox.contains({4,93}), "reconnect preserves slot");
    const auto start = std::chrono::steady_clock::now();
    for (auto *lane : lanes) lane->link.reset();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start);
    check(ms < 2500ms, "idle sockets shut down promptly");
    const auto nals = RecordedNals();
    check(RecordedSeat(ul, 103, 101, nals, false), "UL blue real video with individually framed parameter sets");
    check(RecordedSeat(Competition::Uc2026, 1, 4, nals, true), "2026 late login after camera parameters left full queue");
    std::cout << "shutdown_ms=" << ms.count() << " failures=" << errors << '\n';
    return errors ? 1 : 0;
}
