#include "live_engine.h"
#include "live_protocol.h"
#include "platform_util.h"
#include "packetizer.h"
#include "metrics.h"
#include "video/custom_byte_block_video_decoder.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <cmath>
#include <ctime>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace cw::live {
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
struct PacketMeta { long long pts=0; int size=0,width=0,height=0; double tb=0; };
bool packet_meta(PipeProcess &p,PacketMeta &m,std::string &error) {
    std::string s;
    while(p.line(s)) {
        if(s.starts_with("#dimensions")) {
            const auto colon=s.find(':');
            std::istringstream in(s.substr(colon+1)); char x; in>>m.width>>x>>m.height;
        } else if(s.starts_with("#tb")) {
            const auto colon=s.find(':'); double a=0,b=0; char slash;
            std::istringstream in(s.substr(colon+1)); in>>a>>slash>>b; if(b) m.tb=a/b;
        } else if(!s.empty() && s[0]!='#') {
            std::replace(s.begin(),s.end(),',',' ');
            int stream; long long dts,duration;
            std::istringstream in(s);
            if(in>>stream>>dts>>m.pts>>duration>>m.size) return m.size>0;
            if(error.size()<2048) error+=s+"\n";
        }
    }
    return false;
}
std::string stamp() {
    auto t=std::time(nullptr); std::tm tm{}; localtime_s(&tm,&t);
    char b[40]; std::strftime(b,sizeof(b),"%Y%m%d_%H%M%S",&tm);
    return std::string(b)+"_"+std::to_string(Clock::now().time_since_epoch().count()%1000000);
}
double quantile(std::vector<double> v,double q) {
    if(v.empty()) return 0; std::sort(v.begin(),v.end());
    return v[std::min(v.size()-1,static_cast<std::size_t>(q*(v.size()-1)))];
}
void workload(int ms) { if(ms>0) std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void evidence(StreamFile &file,const std::string &bytes) {
    if(!file.write(bytes))throw std::runtime_error("传输证据文件写入失败");
}
void snapshot(const std::string &path,const std::shared_ptr<const Image> &image) {
    if(!image)return;
    auto rgb=image->bgr;for(std::size_t i=0;i<rgb.size();i+=3)std::swap(rgb[i],rgb[i+2]);
    std::vector<std::uint8_t> png;
    auto callback=[](void *c,void *data,int bytes){auto &v=*static_cast<std::vector<std::uint8_t> *>(c);auto p=static_cast<std::uint8_t *>(data);v.insert(v.end(),p,p+bytes);};
    if(stbi_write_png_to_func(callback,&png,image->width,image->height,3,rgb.data(),image->width*3)){std::string e;write_file(path,png.data(),png.size(),e);}
}
}
bool validate(const Config &c,std::string &e) {
    if(c.width<16||c.height<16||c.width>4096||c.height>4096||c.width%2||c.height%2) e="输出尺寸须为16至4096之间的偶数";
    else if(!std::isfinite(c.fps)||c.fps<=0||c.fps>kMaxFps||!std::isfinite(c.source_fps)||c.source_fps<=0||c.source_fps>240) e="双包实时入口目标帧率必须大于0且不超过25 fps";
    else if(!std::isfinite(c.send_rate)||c.send_rate<=0||c.send_rate>50) e="协议上限为50包/秒";
    else if(c.fps>c.send_rate/kFramePackets) e="目标帧率不能超过包率除以2的预留预算";
    else if(c.raw_width<1||c.raw_height<1||c.raw_width>4096||c.raw_height>4096) e="原片尺寸超出1至4096范围";
    else if(!std::isfinite(c.duration)||c.duration<0||c.test_pre_ms<0||c.test_encode_ms<0||c.test_post_ms<0) e="时间参数非法";
    else if(!validate_strategy(c.strategy,e)) return false;
    else if(c.input.empty()) e="请选择输入视频";
    else return true;
    return false;
}

struct Engine::Impl {
    struct Record {
        std::uint64_t id=0,hash=0,ordinal=0;
        double source=0,pre_start=-1,pre_end=-1,enc_start=-1,enc_end=-1,release=-1;
        double first_arrive=-1,last_arrive=-1,decode_start=-1,decode_end=-1,post_end=-1,present=-1;
        std::size_t coded_bytes=0,packets=0,received=0;
        bool matched=false;
        std::string why;
        std::shared_ptr<Image> reference;
        double psnr=-1,ssim=-1;
    };
    struct Bundle { std::uint64_t id; std::vector<WirePacket> packets; double release=0; };
    struct Incoming { std::uint64_t id; Outer bytes; double arrival; };
    struct MetricJob { std::uint64_t id; std::shared_ptr<Image> reference,decoded; };
    Config cfg;
    Processor pre,post;
    ProcessingState processing_state;
    Clock::time_point origin;
    std::atomic<bool> quit{false},source_done{false},encode_done{false},channel_done{false},rx_done{false},metric_done{false};
    std::atomic<std::shared_ptr<PipeProcess>> source_process,encoder_process;
    std::thread source_thread,encoder_thread,channel_thread,rx_thread,metric_thread;
    std::mutex mutex;
    std::condition_variable cv;
    View state;
    std::shared_ptr<Image> pending;
    std::deque<Bundle> send_queue;
    std::deque<Incoming> rx_queue;
    std::deque<MetricJob> metric_queue;
    std::map<std::uint64_t,Record> records;
    std::deque<std::pair<double,std::uint64_t>> sent_times,present_times;
    std::deque<double> decode_times;
    std::map<int,int> display_bins;
    int next_display_bin=2,stable_windows=0,failed_windows=0,minimum_stable_fps=1000000;
    std::vector<double> latencies;
    StreamFile events,frames,quality,tx_bytes,tx_times,rx_bytes,rx_times,fingerprints,display_seconds;
    StreamFile processed,associations,source_map,displays;
    std::uint64_t rx_count=0,tx_h264_bytes=0,tx_padding_bytes=0;
    std::size_t max_sent_frame_bytes=0,max_sent_frame_packets=0;
    std::uint64_t next_id=1,outer_seq=0,metric_skipped=0,source_skipped=0,source_late=0;
    std::size_t max_queue=0,max_rx=0,max_records=0;
    double last_delivery=-1,min_gap=1e30,source_supply_ms=0;
    bool started=false,closed=false;

    Impl(Config c,Processor a,Processor b):cfg(std::move(c)),pre(std::move(a)),post(std::move(b)) {cfg.strategy.width=cfg.width;cfg.strategy.height=cfg.height;cfg.strategy.fps=cfg.fps;}
    double now() const { return std::chrono::duration<double,std::milli>(Clock::now()-origin).count(); }
    void event(std::uint64_t id,const char *name,double at,const std::string &detail="") {
        std::ostringstream s; s<<std::fixed<<std::setprecision(3)<<at<<','<<id<<','<<name<<','<<detail<<'\n'; events.write(s.str());
    }
    void row(const Record &r) {
        std::ostringstream s; s<<std::fixed<<std::setprecision(3)<<r.id<<','<<r.source<<','<<r.pre_start<<','<<r.pre_end<<','
        <<r.enc_start<<','<<r.enc_end<<','<<r.release<<','<<r.first_arrive<<','<<r.last_arrive<<','<<r.decode_start<<','
        <<r.decode_end<<','<<r.post_end<<','<<r.present<<','<<r.coded_bytes<<','<<r.packets<<','<<r.received<<','<<r.why<<'\n'; frames.write(s.str());
    }
    void trim() {
        while(records.size()>512) { row(records.begin()->second); records.erase(records.begin()); }
        max_records=std::max(max_records,records.size());
    }
    void finish_display_bins(bool all=false) {
        if(!state.left)return;
        const double end=all?state.left->source_ms:std::min(state.left->source_ms,now()-1000);
        const int limit=static_cast<int>(std::floor(end/1000));
        while(next_display_bin<limit) {
            const int count=display_bins[next_display_bin];
            ++stable_windows;minimum_stable_fps=std::min(minimum_stable_fps,count);
            if(count<kMinimumDisplayFps)++failed_windows;
            evidence(display_seconds,std::to_string(next_display_bin)+","+std::to_string(count)+","+(count>=kMinimumDisplayFps?"PASS":"FAIL")+"\n");
            display_bins.erase(next_display_bin++);
        }
        while(!display_bins.empty()&&display_bins.begin()->first<next_display_bin)display_bins.erase(display_bins.begin());
    }
    void fail(const std::string &e) {
        std::lock_guard lock(mutex); if(state.error.empty()) state.error=e;
        quit=true; event(0,"error",now(),e); cv.notify_all();
    }
    bool wait_until(double t) {
        std::unique_lock lock(mutex);
        cv.wait_until(lock,origin+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double,std::milli>(t)),[&]{return quit.load();});
        return !quit;
    }
    void source() {
        const bool raw=cfg.input.ends_with(".bgr")||cfg.input.ends_with(".raw")||cfg.input.ends_with(".bin");
        std::uint64_t index=0; double next_sample=0,loop_base=0;
        bool keep=true;
        while(keep&&!quit) {
            StreamFile file; auto proc=std::make_shared<PipeProcess>();
            PacketMeta meta; meta.width=cfg.raw_width;meta.height=cfg.raw_height;meta.tb=1/cfg.source_fps;
            if(raw) { if(!file.open(cfg.input,"rb")){fail("无法打开原片："+cfg.input);break;} }
            else {
                std::string e;
                if(!proc->start(cfg.ffmpeg,{"-hide_banner","-loglevel","error","-i",cfg.input,"-map","0:v:0","-an","-sn","-dn","-c:v","rawvideo","-pix_fmt","bgr24","-fps_mode","passthrough","-f","tee","[f=rawvideo:flush_packets=1]pipe:1|[f=framecrc:flush_packets=1]pipe:2"},e)){fail(e);break;}
                source_process=proc;
            }
            double first_pts=0,last_pts=-1,step=1000/cfg.source_fps;
            std::uint64_t count=0;
            while(!quit) {
                const double supply_start=now(); std::string e;
                if(!raw && !packet_meta(*proc,meta,e)) { if(!e.empty()&&!quit)fail("输入解码失败："+e); break; }
                if(meta.width<1||meta.height<1||meta.width>4096||meta.height>4096){fail("视频原始尺寸超出支持范围");break;}
                auto image=std::make_shared<Image>(); image->width=meta.width;image->height=meta.height;
                const auto n=static_cast<std::size_t>(meta.width)*meta.height*3;
                image->bgr.resize(n);
                if(raw) {
                    const auto got=file.read(image->bgr.data(),n);
                    if(!got)break;
                    if(got!=n){fail("裸帧文件尾部不完整，检查原片尺寸");break;}
                } else {
                    if(meta.size!=n||meta.tb<=0||!proc->read(image->bgr.data(),n)){fail("视频帧数据不完整或分辨率变化");break;}
                    proc->buffer_limit(n*2);
                }
                const double supply_end=now();
                double pts=raw?count*step:meta.pts*meta.tb*1000;
                if(!count)first_pts=pts;
                pts-=first_pts;
                if(count&&pts<=last_pts){fail("源视频时间戳不是严格递增，请先修复素材");break;}
                if(count)step=pts-last_pts;
                last_pts=pts;
                if(index==0){loop_base=now();next_sample=loop_base;} // Align sampling to the first actual source timestamp.
                const double due=loop_base+pts;
                if(cfg.duration>0&&due>=cfg.duration*1000){keep=false;break;}
                if(count>0&&now()>due+step) {
                    std::lock_guard lock(mutex);++source_late;source_supply_ms+=supply_end-supply_start;
                    event(0,"source-late-skip",now(),"pts_ms="+std::to_string(due));++count;++index;continue;
                }
                if(!wait_until(due))break;
                image->id=next_id++;image->source_ms=now();
                {
                    std::lock_guard lock(mutex);
                    source_supply_ms+=supply_end-supply_start;
                    state.left=image; ++state.input;
                    Record r; r.id=image->id;r.source=image->source_ms;
                    event(r.id,"source",r.source,"pts_ms="+std::to_string(due));
                    std::ostringstream sm;sm.precision(12);sm<<r.id<<','<<index<<','<<count<<','<<pts<<','<<r.source<<','<<meta.width<<','<<meta.height<<'\n';evidence(source_map,sm.str());
                    if(due+0.01>=next_sample) {
                        next_sample+=1000/cfg.fps;
                        if(next_sample<due)next_sample=due+1000/cfg.fps;
                        if(pending){++state.overwritten;records[pending->id].why="encode-overwrite";event(pending->id,"encode-overwrite",now());}
                        pending=image;
                    } else {r.why="sample-skip";++source_skipped;}
                    records.emplace(r.id,std::move(r));trim();cv.notify_all();
                }
                ++count;++index;
            }
            if(!raw)proc->stop();
            if(!count&&!quit&&keep){fail("输入没有可用图像帧");break;}
            loop_base+=std::max(0.0,last_pts)+step;
            if(!cfg.loop)keep=false;
        }
        source_done=true;cv.notify_all();
    }
    void encode() {
        TransportSpec tx;tx.send_rate_hz=cfg.send_rate;tx.max_packets_per_frame=kFramePackets;Packetizer packetizer(tx);
        std::shared_ptr<PipeProcess> proc;
        PacketMeta meta; bool restart=true;
        while(!quit) {
            std::shared_ptr<Image> input;
            {
                std::unique_lock lock(mutex);cv.wait(lock,[&]{return quit||pending||source_done;});
                if(quit||(!pending&&source_done))break;
                input=std::move(pending);pending.reset();
            }
            try {
                const auto id=input->id;
                auto small=std::make_shared<Image>(*input);
                {std::lock_guard lock(mutex);records[id].pre_start=now();event(id,"pre-start",now());}
                workload(cfg.test_pre_ms);if(pre)pre(*small);else preprocess_strategy(*small,cfg.strategy,processing_state);
                if(small->width!=cfg.width||small->height!=cfg.height||small->bgr.size()!=static_cast<std::size_t>(cfg.width)*cfg.height*3)throw std::runtime_error("预处理输出尺寸或像素格式错误");
                {std::lock_guard lock(mutex);auto &r=records[id];r.pre_end=now();r.reference=small;r.enc_start=now();event(id,"encode-start",r.enc_start);}
                if(restart) {
                    if(proc)proc->stop();proc=std::make_shared<PipeProcess>();encoder_process=proc;
                    if(quit)break;
                    const int bitrate=cfg.strategy.bitrate?cfg.strategy.bitrate:static_cast<int>(cfg.send_rate*.85*296*8/1.35);
                    const auto &s=cfg.strategy;
                    const std::string xp="repeat-headers=1:annexb=1:bframes=0:scenecut=0:force-cfr=1:aq-mode="+std::to_string(s.aq)+":aq-strength="+std::to_string(s.aq_strength)+":keyint="+std::to_string(s.gop)+":min-keyint="+std::to_string(s.gop)+":slices="+std::to_string(s.slices);
                    std::vector<std::string> args={"-hide_banner","-loglevel","error","-probesize","32","-analyzeduration","0","-fpsprobesize","0","-f","rawvideo","-pixel_format","bgr24","-video_size",std::to_string(cfg.width)+"x"+std::to_string(cfg.height),"-framerate",std::to_string(cfg.fps),"-i","pipe:0","-map","0:v:0","-an","-c:v","libx264","-preset",s.preset,"-tune","zerolatency","-pix_fmt","yuv420p","-b:v",std::to_string(bitrate),"-minrate",std::to_string(bitrate),"-maxrate",std::to_string(bitrate),"-bufsize",std::to_string(kVbvBits),"-g",std::to_string(s.gop),"-keyint_min",std::to_string(s.gop),"-bf","0","-x264-params",xp,"-bsf:v","filter_units=remove_types=6","-f","tee","[f=h264:flush_packets=1]pipe:1|[f=framecrc:flush_packets=1]pipe:2"};
                    if(s.threads>0){args.insert(args.end()-3,{"-threads",std::to_string(s.threads)});}
                    std::string arglog;for(const auto &a:args)arglog+=a+"\n";std::string save_error;
                    if(!write_file(cfg.output+"/encoder-args.txt",arglog.data(),arglog.size(),save_error))throw std::runtime_error(save_error);
                    std::string error;if(!proc->start(cfg.ffmpeg,args,error))throw std::runtime_error(error);
                    proc->buffer_limit(4*1024*1024);restart=false;
                }
                workload(cfg.test_encode_ms);
                if(!proc->write(small->bgr.data(),small->bgr.size())) {if(quit)break;throw std::runtime_error("编码器输入管道断开");}
                std::string error;
                if(!packet_meta(*proc,meta,error)||meta.size>4*1024*1024) {if(quit)break;throw std::runtime_error("编码器无有效输出："+error);}
                std::vector<std::uint8_t> au(meta.size);
                if(!proc->read(au.data(),au.size())){if(quit)break;throw std::runtime_error("编码器输出不完整");}
                if(cfg.test_oversize_frame>0&&id==static_cast<std::uint64_t>(cfg.test_oversize_frame))au.resize(kFrameBytes+1,0);
                Bundle b;b.id=id;
                const bool fits=packetizer.push_frame(au,static_cast<std::uint32_t>(id),b.packets);
                {
                    std::lock_guard lock(mutex);auto &r=records[id];++state.encoded;
                    r.enc_end=now();r.coded_bytes=au.size();r.hash=vcl_hash(au);r.packets=b.packets.size();
                    event(id,"encode-end",r.enc_end,"bytes="+std::to_string(au.size()));
                    if(!fits){++state.oversize;r.why="oversize";r.reference.reset();restart=true;event(id,"oversize-restart-IDR",now());}
                    else {
                        send_queue.push_back(std::move(b));
                        while(send_queue.size()>static_cast<std::size_t>(std::max(2.0,cfg.fps))) {
                            const auto old=send_queue.front().id;send_queue.pop_front();++state.backlog;
                            records[old].why="backlog";records[old].reference.reset();event(old,"backlog-drop",now());
                            // Following P frames still pass to the real decoder, which detects the sequence gap.
                        }
                        max_queue=std::max(max_queue,send_queue.size());cv.notify_all();
                    }
                }
            } catch(const std::exception &e){if(!quit)fail(e.what());break;}
        }
        if(proc)proc->close_input();
        encode_done=true;cv.notify_all();
    }
    void channel() {
        double tokens=0,last=now();
        const double period=1000/cfg.send_rate;
        while(!quit) {
            Bundle b;
            {
                std::unique_lock lock(mutex);
                cv.wait_for(lock,2ms,[&]{return quit||!send_queue.empty()||encode_done;});
                if(quit|| (send_queue.empty()&&encode_done))break;
                const double t=now();tokens=std::min(double(kFramePackets),tokens+(t-last)*cfg.send_rate/1000);last=t;
                if(send_queue.empty())continue;
                if(tokens<send_queue.front().packets.size()) {
                    const double wait=(send_queue.front().packets.size()-tokens)*1000/cfg.send_rate;
                    cv.wait_for(lock,std::chrono::duration<double,std::milli>(wait),[&]{return quit.load();});continue;
                }
                b=std::move(send_queue.front());send_queue.pop_front();tokens-=b.packets.size();
                if(b.packets.size()>kFramePackets)throw std::runtime_error("双包发送约束失效");
                max_sent_frame_packets=std::max(max_sent_frame_packets,b.packets.size());
                max_sent_frame_bytes=std::max(max_sent_frame_bytes,records[b.id].coded_bytes);
                b.release=now();records[b.id].release=b.release;event(b.id,"token-release",b.release);
            }
            for(const auto &p:b.packets) {
                if(!wait_until(now()+period))break;
                Transport data{};std::copy(p.bytes.begin(),p.bytes.end(),data.begin());
                const auto outer=wrap(data,static_cast<std::uint8_t>(outer_seq++));
                std::lock_guard lock(mutex);
                const double at=now();
                if(last_delivery>=0)min_gap=std::min(min_gap,at-last_delivery);
                last_delivery=at;++state.sent;sent_times.emplace_back(at,state.sent);
                const std::size_t payload=((data[0]<<8)|data[1])-2;
                tx_h264_bytes+=payload;tx_padding_bytes+=kPayloadBytes-payload;
                evidence(tx_bytes,std::string(reinterpret_cast<const char *>(outer.data()),outer.size()));
                event(b.id,"channel-deliver",at,"packet="+std::to_string(state.sent));
                std::string status="accepted";
                if(cfg.test_drop_all||static_cast<long long>(state.sent)==cfg.drop_packet)status="network-drop";
                else if(rx_queue.size()>=256)status="rx-overflow";
                std::ostringstream txrow;txrow<<state.sent<<','<<std::fixed<<std::setprecision(9)<<at<<','<<b.id<<','<<status<<'\n';evidence(tx_times,txrow.str());
                if(status=="network-drop"){++state.network_drop;records[b.id].why="injected-network-drop";event(b.id,"network-drop",at);continue;}
                if(status=="rx-overflow"){++state.rx_drop;records[b.id].why="rx-overflow";event(b.id,"rx-overflow",at);continue;}
                evidence(rx_bytes,std::string(reinterpret_cast<const char *>(data.data()),data.size()));
                std::ostringstream rxrow;rxrow<<++rx_count<<','<<state.sent<<','<<std::fixed<<std::setprecision(9)<<at<<'\n';evidence(rx_times,rxrow.str());
                rx_queue.push_back({b.id,outer,at});max_rx=std::max(max_rx,rx_queue.size());cv.notify_all();
            }
        }
        channel_done=true;cv.notify_all();
    }
    std::uint64_t resolve(std::span<const std::uint8_t> au,std::uint64_t observed_id) {
        const auto hash=vcl_hash(au);std::lock_guard lock(mutex);
        // Only a fully received, byte-identical access unit may acquire an ID.
        // Trace metadata never supplies bytes or changes decoder decisions.
        const auto it=records.find(observed_id);
        if(it!=records.end()){auto &r=it->second;if(hash&&r.hash==hash&&!r.matched&&r.packets&&r.received==r.packets){r.matched=true;return observed_id;}}
        return 0;
    }
    void receive() {
        RoboGo::video::CustomByteBlockVideoDecoderConfig dc;
        dc.trace_resolver=[this](auto au,auto id){return resolve(au,id);};
        try {
            RoboGo::video::CustomByteBlockVideoDecoder decoder(dc);
            auto accept=[&](auto decoded,double decode_start,double decode_end) {
                for(auto &f:decoded) {
                    const auto fingerprint=fingerprint_line(state.decoded+1,f.frame_id,f.pts,f.width,f.height,f.rgba);
                    auto image=std::make_shared<Image>();image->id=f.trace_id;image->width=f.width;image->height=f.height;
                    image->bgr.resize(static_cast<std::size_t>(f.width)*f.height*3);
                    for(std::size_t i=0;i<image->bgr.size()/3;++i){image->bgr[3*i]=f.rgba[4*i+2];image->bgr[3*i+1]=f.rgba[4*i+1];image->bgr[3*i+2]=f.rgba[4*i];}
                    workload(cfg.test_post_ms);if(post)post(*image);else reconstruct_pixels(image->bgr,image->width,image->height,cfg.strategy);
                    image->decoded_ordinal=state.decoded+1;
                    const auto post_fingerprint=fingerprint_line(image->decoded_ordinal,f.frame_id,f.pts,image->width,image->height,rgba_pixels(image->bgr));
                    std::lock_guard lock(mutex);++state.decoded;
                    evidence(fingerprints,fingerprint);evidence(processed,post_fingerprint);decode_times.push_back(decode_end);
                    evidence(associations,std::to_string(image->decoded_ordinal)+","+std::to_string(image->id)+"\n");
                    auto it=records.find(image->id);
                    if(!image->id||it==records.end()){++state.unknown;image->id=0;event(0,"decoded-unassociated",now());}
                    else {
                        auto &r=it->second;r.ordinal=image->decoded_ordinal;image->source_ms=r.source;r.decode_start=decode_start;r.decode_end=decode_end;r.post_end=now();
                        image->coded_bytes=r.coded_bytes;image->packets=r.packets;
                        event(image->id,"decoded",decode_end);event(image->id,"post-end",r.post_end);
                        if(!cfg.strategy.offline_metrics&&r.reference&&metric_queue.size()<2) {metric_queue.push_back({image->id,r.reference,image});cv.notify_all();}
                        else ++metric_skipped;
                        r.reference.reset();
                    }
                    if(state.right&&state.right->id&&records.contains(state.right->id)&&records[state.right->id].present<0)records[state.right->id].why="display-overwrite";
                    state.right=std::move(image);
                }
            };
            while(!quit) {
                Incoming p;
                {
                    std::unique_lock lock(mutex);cv.wait(lock,[&]{return quit||!rx_queue.empty()||channel_done;});
                    if(quit||(rx_queue.empty()&&channel_done))break;
                    p=std::move(rx_queue.front());rx_queue.pop_front();
                }
                Transport data{},decoded{};
                if(!unwrap(p.bytes,data)||!unprotobuf(protobuf(data),decoded)){fail("协议帧校验失败");break;}
                {
                    std::lock_guard lock(mutex);auto &r=records[p.id];++r.received;
                    if(r.first_arrive<0)r.first_arrive=p.arrival;r.last_arrive=p.arrival;
                }
                const double begin=now();auto result=decoder.push_packet_bytes(decoded,p.id);const double end=now();
                accept(std::move(result),begin,end);
            }
            // EOF drains parser state only. It never injects unsent transport packets.
            if(!quit){const double begin=now();auto result=decoder.flush();accept(std::move(result),begin,now());}
        }catch(const std::exception &e){if(!quit)fail(e.what());}
        rx_done=true;cv.notify_all();
    }
    void metrics() {
        while(!quit) {
            MetricJob job;
            {
                std::unique_lock lock(mutex);cv.wait(lock,[&]{return quit||!metric_queue.empty()||rx_done;});
                if(quit||(metric_queue.empty()&&rx_done))break;
                job=std::move(metric_queue.front());metric_queue.pop_front();
            }
            const auto &a=*job.reference;const auto &b=*job.decoded;
            if(a.width!=b.width||a.height!=b.height)continue;
            const auto n=static_cast<std::size_t>(a.width)*a.height;
            std::vector<std::uint8_t> y1(n),y2(n);
            for(std::size_t i=0;i<n;++i){auto y=[](const std::uint8_t *p){return static_cast<std::uint8_t>((77*p[2]+150*p[1]+29*p[0]+128)>>8);};y1[i]=y(a.bgr.data()+3*i);y2[i]=y(b.bgr.data()+3*i);}
            const double p=psnr_y(y1.data(),y2.data(),a.width,a.height),s=ssim_y(y1.data(),y2.data(),a.width,a.height);
            std::lock_guard lock(mutex);
            if(records.contains(job.id)){records[job.id].psnr=p;records[job.id].ssim=s;}
            quality.write(std::to_string(job.id)+","+std::to_string(p)+","+std::to_string(s)+"\n");
        }
        metric_done=true;cv.notify_all();
    }
};

Engine::Engine(Config c,Processor a,Processor b):impl_(std::make_unique<Impl>(std::move(c),std::move(a),std::move(b))){}
Engine::~Engine(){stop();}
bool Engine::start(std::string &e) {
    auto &p=*impl_;if(p.started){e="请创建新的运行会话";return false;}
    if(!validate(p.cfg,e))return false;
    p.cfg.ffmpeg=find_executable(p.cfg.ffmpeg);if(p.cfg.ffmpeg.empty()){e="找不到ffmpeg.exe";return false;}
    if(p.cfg.output.empty())p.cfg.output="runs/live/"+stamp();
    if(!ensure_dir(p.cfg.output,e))return false;
    for(const auto *name:{"summary.txt","events.csv","tx-0310.bin","rx-300.bin","last-received.png"}) {
        std::uint64_t size=0;if(file_size(p.cfg.output+"/"+name,size)){e="输出目录含已有记录，请选择新的输出目录";return false;}
    }
    if(!p.events.open(p.cfg.output+"/events.csv","wb")||!p.frames.open(p.cfg.output+"/per_frame.csv","wb")||!p.quality.open(p.cfg.output+"/quality.csv","wb")){e="无法创建运行日志";return false;}
    for(auto pair:{std::pair{&p.tx_bytes,"tx-0310.bin"},std::pair{&p.tx_times,"tx-times.csv"},std::pair{&p.rx_bytes,"rx-300.bin"},std::pair{&p.rx_times,"rx-times.csv"},std::pair{&p.fingerprints,"decoded.csv"},std::pair{&p.display_seconds,"display-seconds.csv"},std::pair{&p.processed,"processed.csv"},std::pair{&p.associations,"decoded-association.csv"},std::pair{&p.source_map,"source-map.csv"},std::pair{&p.displays,"display.csv"}})
        if(!pair.first->open(p.cfg.output+"/"+pair.second,"wb")){e="无法创建传输证据文件";return false;}
    p.tx_times.write("packet,time_ms,source_frame,status\n");p.rx_times.write("packet,tx_packet,time_ms\n");
    p.fingerprints.write("ordinal,frame_id,pts,width,height,sha256\n");p.display_seconds.write("second,displayed,result\n");
    p.events.write("time_ms,frame_id,event,detail\n");
    p.processed.write("ordinal,frame_id,pts,width,height,sha256\n");p.associations.write("ordinal,source_id\n");
    p.source_map.write("source_id,source_index,file_index,pts_ms,source_ms,width,height\n");p.displays.write("source_id,ordinal,present_ms,source_ms\n");
    for(bool receiver:{false,true}){const auto data=strategy_text(p.cfg.strategy,receiver);if(!write_file(p.cfg.output+(receiver?"/receiver.ini":"/strategy.ini"),data.data(),data.size(),e))return false;}
    p.frames.write("frame_id,source_ms,pre_start_ms,pre_end_ms,encode_start_ms,encode_end_ms,token_release_ms,first_arrive_ms,last_arrive_ms,decode_start_ms,decode_end_ms,post_end_ms,present_ms,coded_bytes,packets,received,why\n");
    p.quality.write("frame_id,psnr_y,ssim_y\n");
    p.origin=Clock::now();p.started=true;p.state.output=p.cfg.output;
    p.metric_thread=std::thread([&p]{p.metrics();});p.rx_thread=std::thread([&p]{p.receive();});
    p.channel_thread=std::thread([&p]{try{p.channel();}catch(const std::exception &e){p.fail(e.what());p.channel_done=true;p.cv.notify_all();}});p.encoder_thread=std::thread([&p]{p.encode();});p.source_thread=std::thread([&p]{p.source();});
    return true;
}
View Engine::view() {
    auto &p=*impl_;std::lock_guard lock(p.mutex);auto v=p.state;
    v.now_ms=p.started?p.now():0;v.finished=p.started&&p.source_done&&p.encode_done&&p.channel_done&&p.rx_done&&p.metric_done;
    v.running=p.started&&!p.closed&&!v.finished&&!p.quit;
    v.queued_frames=p.send_queue.size();v.rx_packets=p.rx_queue.size();
    while(!p.sent_times.empty()&&p.sent_times.front().first<v.now_ms-1000)p.sent_times.pop_front();
    while(!p.present_times.empty()&&p.present_times.front().first<v.now_ms-1000)p.present_times.pop_front();
    while(!p.decode_times.empty()&&p.decode_times.front()<v.now_ms-1000)p.decode_times.pop_front();
    p.finish_display_bins();v.stable_windows=p.stable_windows;v.failed_windows=p.failed_windows;v.minimum_stable_fps=p.stable_windows?p.minimum_stable_fps:0;
    v.packets_sec=static_cast<double>(p.sent_times.size());v.update_fps=static_cast<double>(p.present_times.size());v.decode_fps=static_cast<double>(p.decode_times.size());
    if(v.right&&v.right->id)v.age_ms=v.now_ms-v.right->source_ms;
    v.status=!v.error.empty()?"错误":v.running?"实时运行":v.finished?"播放结束":"已停止";return v;
}
void Engine::presented(std::uint64_t id) {
    if(!id)return;auto &p=*impl_;std::lock_guard lock(p.mutex);auto it=p.records.find(id);
    if(it==p.records.end()||it->second.present>=0||it->second.post_end<0)return;
    const double at=p.now();auto &r=it->second;r.present=at;++p.state.displayed;
    p.state.latency_ms=at-r.source;p.latencies.push_back(at-r.source);
    // Distribution uses a bounded rolling sample; event/per-frame logs keep the full run.
    if(p.latencies.size()>4096)p.latencies.erase(p.latencies.begin());
    p.present_times.emplace_back(at,id);p.event(id,p.cfg.headless?"headless-consumed":"present-submit",at);
    ++p.display_bins[static_cast<int>(at/1000)];
    std::ostringstream ds;ds.precision(12);ds<<id<<','<<r.ordinal<<','<<at<<','<<r.source<<'\n';evidence(p.displays,ds.str());
}
void Engine::stop() {
    auto &p=*impl_;if(!p.started||p.closed)return;
    const bool natural=!p.quit&&p.source_done&&p.encode_done&&p.channel_done&&p.rx_done&&p.metric_done;
    p.quit=true;p.cv.notify_all();
    if(auto proc=p.source_process.load())proc->stop();
    if(auto proc=p.encoder_process.load())proc->stop();
    for(auto *t:{&p.source_thread,&p.encoder_thread,&p.channel_thread,&p.rx_thread,&p.metric_thread})if(t->joinable())t->join();
    if(auto proc=p.source_process.load())proc->stop();if(auto proc=p.encoder_process.load())proc->stop();
    std::lock_guard lock(p.mutex);
    for(const auto &[id,r]:p.records)p.row(r);
    p.finish_display_bins(true);
    std::ostringstream s;s<<std::fixed<<std::setprecision(3)
      <<"mode="<<(p.cfg.headless?"headless; no real display":"live; actual GDI display submission")<<"\ninput="<<p.cfg.input
      <<"\nsource_raw_size="<<p.cfg.raw_width<<'x'<<p.cfg.raw_height<<"\nsource_raw_fps="<<p.cfg.source_fps
      <<"\noutput_size="<<p.cfg.width<<'x'<<p.cfg.height<<"\nstrategy="<<p.cfg.strategy.name<<"\ncolor="<<p.cfg.strategy.color<<"; yuv420p; full field of view; aspect-fit; padding; no crop"
      <<"\ntarget_fps="<<p.cfg.fps<<"\nsend_rate="<<p.cfg.send_rate<<"\nbitrate="<<(p.cfg.strategy.bitrate?p.cfg.strategy.bitrate:int(p.cfg.send_rate*.85*296*8/1.35))
      <<"\ngop="<<p.cfg.strategy.gop<<"\nmetrics_mode="<<(p.cfg.strategy.offline_metrics?"offline":"legacy in-runtime encoder-input comparison")<<"\nmax_frame_packets="<<kFramePackets<<"\nmax_frame_h264_bytes="<<kFrameBytes<<"\nvbv_bits="<<kVbvBits
      <<"\nreserved_theoretical_fps="<<p.cfg.send_rate/kFramePackets<<"\nsei=removed; SPS/PPS/VCL preserved\nmodel=C-board bypass; ideal channel; no additional wireless loss or latency"
      <<"\nffmpeg="<<p.cfg.ffmpeg<<"\nnatural_end="<<natural<<"\nwall_ms="<<p.now()
      <<"\nsource_frames="<<p.state.input<<"\nsampling_skipped="<<p.source_skipped<<"\nencode_overwrite="<<p.state.overwritten
      <<"\nsource_late_skipped="<<p.source_late
      <<"\nencoded="<<p.state.encoded<<"\noversize="<<p.state.oversize<<"\nbacklog_frames="<<p.state.backlog
      <<"\npackets_sent="<<p.state.sent<<"\nnetwork_drop="<<p.state.network_drop<<"\nrx_overflow="<<p.state.rx_drop
      <<"\ndecoded="<<p.state.decoded<<"\ndecoded_unassociated="<<p.state.unknown<<"\ndisplayed_or_headless_consumed="<<p.state.displayed
      <<"\nmin_packet_gap_ms="<<(p.min_gap<1e29?p.min_gap:0)<<"\nlatency_median_ms="<<quantile(p.latencies,.5)<<"\nlatency_p95_ms="<<quantile(p.latencies,.95)
      <<"\nlatency_sample_limit=4096; headless excludes actual rendering\nsource_supply_work_ms="<<p.source_supply_ms
      <<"\nmax_send_queue_frames="<<p.max_queue<<"\nmax_rx_queue_packets="<<p.max_rx<<"\nmax_trace_records="<<p.max_records
      <<"\nmax_sent_frame_packets="<<p.max_sent_frame_packets<<"\nmax_sent_frame_h264_bytes="<<p.max_sent_frame_bytes
      <<"\ntx_h264_bytes="<<p.tx_h264_bytes<<"\ntx_inner_header_bytes="<<p.state.sent*4<<"\ntx_padding_bytes="<<p.tx_padding_bytes
      <<"\ntx_data_bytes="<<p.state.sent*kPacketBytes<<"\ntx_outer_frame_bytes="<<p.state.sent*309<<"\nrx_packets="<<p.rx_count
      <<"\nstable_display_windows="<<p.stable_windows<<"\nminimum_stable_display_fps="<<(p.stable_windows?p.minimum_stable_fps:0)
      <<"\ndisplay_windows_below_15="<<p.failed_windows<<"\ndisplay_acceptance="<<(p.cfg.headless?"NOT_APPLICABLE_HEADLESS":!p.stable_windows?"INSUFFICIENT_DATA":p.failed_windows?"FAIL":"PASS")
      <<"\nmetrics_skipped="<<p.metric_skipped<<"\ntest_pre_ms="<<p.cfg.test_pre_ms<<"\ntest_encode_ms="<<p.cfg.test_encode_ms
      <<"\ntest_post_ms="<<p.cfg.test_post_ms<<"\ntest_drop_all="<<p.cfg.test_drop_all<<"\ntest_oversize_frame="<<p.cfg.test_oversize_frame<<"\nerror="<<p.state.error<<'\n';
    std::string e;const auto report=s.str();write_file(p.cfg.output+"/summary.txt",report.data(),report.size(),e);
    snapshot(p.cfg.output+"/last-source.png",p.state.left);snapshot(p.cfg.output+"/last-received.png",p.state.right);
    p.events.close();p.frames.close();p.quality.close();p.closed=true;
    p.tx_bytes.close();p.tx_times.close();p.rx_bytes.close();p.rx_times.close();p.fingerprints.close();p.display_seconds.close();
    p.processed.close();p.associations.close();p.source_map.close();p.displays.close();
    p.pending.reset();p.send_queue.clear();p.rx_queue.clear();p.metric_queue.clear();p.records.clear();
}
} // namespace cw::live
