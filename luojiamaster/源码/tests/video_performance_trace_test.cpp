#include "app/video_performance_trace.h"
#include <iostream>
#include <stdexcept>

using namespace RoboGo::app::performance;
using namespace std::chrono_literals;
static void require(bool condition, const char *text) { if (!condition) throw std::runtime_error(text); }
static TraceClock::time_point t(int milliseconds) { return TraceClock::time_point{} + std::chrono::milliseconds(milliseconds); }
static DataArrival origin(int packet, int milliseconds, int loop = 0) { return {true, static_cast<std::uint64_t>(loop), static_cast<std::uint64_t>(packet), static_cast<std::uint64_t>(packet), t(milliseconds), t(milliseconds)}; }
static std::vector<std::uint8_t> udp(int frame, int shard, int size, std::initializer_list<std::uint8_t> payload) {
    std::vector<std::uint8_t> result{std::uint8_t(frame>>8),std::uint8_t(frame),std::uint8_t(shard>>8),std::uint8_t(shard),std::uint8_t(size>>24),std::uint8_t(size>>16),std::uint8_t(size>>8),std::uint8_t(size)};
    result.insert(result.end(), payload); return result;
}
int main() {
    CausalByteOrigins stream;
    const std::vector<std::uint8_t> a{1,2,3}, b{4,5,6,7}, whole{1,2,3,4,5,6}, tail{7};
    stream.Append(a, origin(1,10)); stream.Append(b, origin(2,20));
    auto across = stream.Resolve(whole,0);
    require(across.known && across.first==t(10) && across.complete==t(20), "cross-input AU must use max actual arrival");
    require(stream.Resolve(tail,6).known, "lookahead bytes must survive earlier AU pruning");
    require(!stream.Resolve(tail,0).known, "old offsets must never be order matched");
    stream.Reset(); stream.Append(a,origin(1,10));
    const std::vector<std::uint8_t> bad{1,2,4};
    require(!stream.Resolve(bad,0).known, "mismatched bytes must remain unknown");
    require(!stream.Resolve(a,-1).known, "negative parser offset must remain unknown");
    require(!stream.Resolve(whole,0).known, "future/unreceived bytes must remain unknown");
    stream.Reset(); stream.Append(a,origin(1,10)); stream.Append(b,{});
    require(!stream.Resolve(whole,0).known, "one unknown input invalidates entire AU association");
    stream.Reset(); stream.Append(a,origin(1,10,0)); stream.Append(b,origin(2,20,1));
    require(!stream.Resolve(whole,0).known, "loop boundary must not be stitched into one observed AU");
    CausalByteOrigins bounded(8); for(int i=0;i<30;++i) bounded.Append(a,origin(i+1,i));
    require(bounded.BufferedBytes()<=8, "observation memory must be bounded");
    UdpArrivalRegistry registry;
    auto p0=udp(65535,0,6,{1,2,3}), p1=udp(65535,1,6,{4,5,6});
    registry.Add(0,1,p0,t(10));
    require(!registry.Resolve(0,65535,whole).known,"missing shard must remain unknown");
    registry.Add(0,2,p1,t(20));
    auto complete=registry.Resolve(0,65535,whole);
    require(complete.known && complete.complete==t(20),"dense UDP shards must resolve exact bytes");
    registry.Add(0,3,p0,t(30));
    require(registry.Resolve(0,65535,whole).complete==t(20),"identical duplicate must not move availability later");
    auto pbad=udp(65535,1,6,{4,5,9}); registry.Add(0,4,pbad,t(40));
    require(!registry.Resolve(0,65535,whole).known,"conflicting duplicate must be unknown");
    auto wrapped=udp(0,0,3,{1,2,3}); registry.Add(0,5,wrapped,t(50));
    require(registry.Resolve(0,0,a).known,"wire sequence wrap must not collide with prior id");
    registry.Add(1,1,p0,t(60)); registry.Add(1,2,p1,t(70));
    require(registry.Resolve(1,65535,whole).complete==t(70),"loop must get distinct actual ingress timestamps");
    registry.Add(1,10000,p0,t(80));
    require(!registry.Resolve(1,65535,whole).known,"very late id reuse must not inherit an old frame's timestamp");
    std::cout << "PASS 14 causal-byte/UDP provenance checks; no output-order matching\n";
}
