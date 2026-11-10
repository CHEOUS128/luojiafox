#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>
#include <string>
#include "io/video_sender/cw_tx_worker.hpp"
namespace cw = io::cw;
namespace tools {struct Log{template<class... T>void warn(T&&...) {} template<class... T>void error(T&&...) {}};inline Log* logger(){static Log l;return &l;}}
struct Serial {
 std::vector<uint8_t> wire; int block_after=-1, throw_after_prefix=-1; bool blocked=false, fail=false;
 std::size_t write(const uint8_t*p,std::size_t n){
  if(fail)throw std::runtime_error("injected disconnect before write");
  if(throw_after_prefix>=0){auto take=std::min(n,size_t(throw_after_prefix));wire.insert(wire.end(),p,p+take);throw_after_prefix=-1;throw std::runtime_error("injected unknown written prefix");}
  if(blocked)return 0;
  if(block_after>=0){auto take=std::min(n,static_cast<size_t>(block_after));wire.insert(wire.end(),p,p+take);block_after=-1;blocked=true;return take;}
  wire.insert(wire.end(),p,p+n);return n;
 }
};
struct Gimbal {
 static constexpr uint32_t kVideoBudgetMs=8,kControlBudgetMs=5;
 Serial serial_;std::mutex write_mutex_;
 std::atomic<uint64_t> st_ctrl_ok_{0},st_ctrl_short_{0},st_ctrl_resync_{0},st_video_ok_{0},st_video_short_{0},st_video_stall_{0},st_video_fail_{0};
 std::array<uint8_t,64>ctrl_buf_{};size_t ctrl_len_=0,ctrl_off_=0;
 std::vector<uint8_t>video_next_,video_leftover_;bool video_pending_=false;size_t video_leftover_offset_=0;
 bool trial_video_=false,trial_fault_=false;cw::PacketGate trial_gate_;
 size_t write_some(const uint8_t*,size_t,uint32_t);void pump_writes();bool queue_control(const uint8_t*,size_t);bool try_send_raw(const uint8_t*,size_t);
 void enable_trial_video();bool try_send_trial_video(const std::vector<uint8_t>&);void service_trial_video();bool finish_trial_video();
};
#include "vehicle-exact.inc"
static unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Config {
 std::string cw_profile,x264_preset;int width=0,height=0,max_frame_packets=0,max_frame_bytes=0,gop=0,bitrate_kbps=0;
 double fps=0,sharpen=1,send_rate_hz=0;bool keep_aspect=false,gray=true,large_frame=false,single_packet=true;
};
int main(){try {
 const std::vector<uint8_t> video(309,0xa5),control(32,0xff),next(309,0xa5);
 for(const std::string name: {"baseline","smooth","detail","detail-aq0","quality"}) {
  Config c;cw::apply(c,name);check(!c.gray&&c.keep_aspect&&c.max_frame_packets==2&&c.send_rate_hz==50&&c.fps==20,"profile hard constraints");
 }
 cw::PacketGate gate;auto now=cw::PacketGate::Clock::now();gate.completed(now);
 check(!gate.ready(now+std::chrono::microseconds(19999)),"gate early");check(gate.ready(now+cw::packet_interval),"gate due");
 gate.completed(now+std::chrono::seconds(10));check(!gate.ready(now+std::chrono::seconds(10)),"late catch-up burst");
 auto filtered=cw::without_sei({0,0,0,1,0x67,11,0,0,1,6,22,0,0,0,1,0x65,33});
 check(filtered==std::vector<uint8_t>({0,0,0,1,0x67,11,0,0,0,1,0x65,33}),"SEI removed image bytes");
 for(int mode=0;mode<3;++mode){Gimbal g;for(int i=0;i<50;++i){if(mode){auto c=control;if(mode==2)c[1]=uint8_t(i);g.queue_control(c.data(),c.size());}g.try_send_raw(video.data(),video.size());}check(g.st_video_ok_==50,"legacy control matrix changed");}
 // Every possible split, including zero bytes, retains exact control/video boundaries.
 for(int split=0;split<309;++split){
  Gimbal g;g.enable_trial_video();g.serial_.block_after=split;check(g.try_send_trial_video(video),"initial enqueue");g.service_trial_video();
  check(!g.try_send_trial_video(next),"overwrote partial packet");
  g.queue_control(control.data(),control.size());g.serial_.blocked=false;g.service_trial_video();
  auto expected=video;expected.insert(expected.end(),control.begin(),control.end());check(g.serial_.wire==expected,"video/control interleaved");
 }
 for(int split=0;split<32;++split){Gimbal g;g.enable_trial_video();g.serial_.block_after=split;
  g.queue_control(control.data(),control.size());check(g.try_send_trial_video(video),"control partial enqueue");g.service_trial_video();
  g.serial_.blocked=false;g.service_trial_video();auto expected=control;expected.insert(expected.end(),video.begin(),video.end());check(g.serial_.wire==expected,"control/video interleaved");}
 {Gimbal g;g.enable_trial_video();check(g.try_send_trial_video(video),"enqueue");g.service_trial_video();check(g.try_send_trial_video(next),"next enqueue");
  g.service_trial_video();check(g.st_video_ok_==1,"serial pacing bypassed");
  check(g.queue_control(control.data(),control.size()),"control starved during video pacing");check(g.st_ctrl_ok_==1&&g.st_video_ok_==1,"control priority");
  check(g.finish_trial_video()&&g.st_video_ok_==2,"paced completion without control input");}
 {Gimbal g;g.enable_trial_video();g.serial_.block_after=100;g.try_send_trial_video(video);g.service_trial_video();
  g.serial_.blocked=false;g.serial_.fail=true;bool caught=false;try{g.service_trial_video();}catch(...){caught=true;}
  check(caught&&g.trial_fault_,"write exception not reported");check(g.video_leftover_offset_==0,"exception discarded acknowledged offset");
  g.serial_.fail=false;g.queue_control(control.data(),control.size());g.pump_writes();
  check(g.serial_.wire.size()==100&&!g.finish_trial_video(),"unknown partial write resumed or inserted controls");}
 {Gimbal g;g.enable_trial_video();g.serial_.throw_after_prefix=13;
  g.try_send_trial_video(video);try{g.service_trial_video();}catch(...){}
  g.queue_control(control.data(),control.size());g.pump_writes();
  check(g.serial_.wire.size()==13&&g.trial_fault_,"exception after hidden partial write inserted control bytes");
  check(!g.finish_trial_video(),"hidden prefix exception reported success");}
 {std::vector<std::chrono::steady_clock::time_point> times;int pulls=0;std::atomic<int> tries{0},accepted{0};
  cw::TxWorker worker([&]{return ++pulls<=4?cw::TxWorker::Bundle{video,next}:cw::TxWorker::Bundle{};},[&](const auto&){if(++tries%3==0)return false;times.push_back(std::chrono::steady_clock::now());++accepted;return true;},[]{});
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(accepted<8&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
  worker.stop();check(!worker.failed()&&times.size()==8,"worker ownership/retry");
  for(size_t i=1;i<times.size();++i)check(times[i]-times[i-1]>=cw::packet_interval,"worker burst");}
 std::printf("PASS vehicle trial: %u checks; exact serial methods; every video/control short-write offset; pacing; retry; SEI; legacy matrix\n",checks);return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
