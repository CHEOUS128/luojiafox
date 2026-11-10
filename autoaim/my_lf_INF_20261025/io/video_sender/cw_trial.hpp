#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace io::cw {
constexpr int frame_bytes = 592, frame_packets = 2, data_bytes = 300;
constexpr int bitrate = 74548, vbv_bits = 4736;
constexpr auto packet_interval = std::chrono::milliseconds(20);
inline const char* version() { return "cw-trial-20261109-r2"; }
inline bool valid_profile(const std::string& s) {
  return s == "baseline" || s == "smooth" || s == "detail" || s == "detail-aq0" || s == "quality";
}
// Only called explicitly: existing YAML and the no-profile path keep their meaning.
template<class Config> void apply(Config& c, const std::string& name) {
  if (!valid_profile(name)) throw std::invalid_argument("unknown --cw-profile: " + name);
  c.cw_profile = name; c.width = 192; c.height = 152; c.fps = 20;
  c.keep_aspect = true; c.sharpen = 0; c.gray = false;
  c.large_frame = true; c.single_packet = false; c.max_frame_bytes = 296;
  c.max_frame_packets = frame_packets; c.send_rate_hz = 50; c.bitrate_kbps = 0;
  c.gop = name == "quality" ? 20 : 10; c.x264_preset = "slow";
  if (name == "smooth") { c.width = 160; c.height = 128; }
  if (name == "detail" || name == "detail-aq0" || name == "quality") { c.width = 320; c.height = 256; }
}
// Completion -> next start; late service never accumulates catch-up credits.
struct PacketGate {
  using Clock = std::chrono::steady_clock;
  Clock::time_point next{};
  bool ready(Clock::time_point now) const { return now >= next; }
  void completed(Clock::time_point now) { next = now + packet_interval; }
};
// Annex B only. Preserve every non-SEI byte, including SPS/PPS and all VCL data.
inline std::vector<std::uint8_t> without_sei(const std::vector<std::uint8_t>& in) {
  struct Start { std::size_t offset, header; };
  std::vector<Start> starts;
  for (std::size_t i = 0; i + 3 < in.size();) {
    std::size_t n = 0;
    if (in[i] == 0 && in[i+1] == 0) {
      if (in[i+2] == 1) n = 3;
      else if (in[i+2] == 0 && in[i+3] == 1) n = 4;
    }
    if (n && i + n < in.size()) { starts.push_back({i, i+n}); i += n + 1; }
    else ++i;
  }
  if (starts.empty() || starts.front().offset != 0)
    throw std::runtime_error("cw trial requires Annex B encoder output");
  std::vector<std::uint8_t> out;
  for (std::size_t k=0; k<starts.size(); ++k) {
    const auto end = k+1 < starts.size() ? starts[k+1].offset : in.size();
    if ((in[starts[k].header] & 31) != 6)
      out.insert(out.end(), in.begin()+starts[k].offset, in.begin()+end);
  }
  return out;
}
}
