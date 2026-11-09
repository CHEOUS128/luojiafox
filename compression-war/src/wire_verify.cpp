// Independent consumer: no source video, Engine, encoder or observation IDs.
// Linked to rm_decoder (the unchanged client decoder), not cw_live_decoder.
#include "video/custom_byte_block_video_decoder.h"
#include "live_protocol.h"
#include "platform_util.h"
#include <sstream>
#include <cmath>
#include <thread>

namespace {
using namespace cw::live;
struct Tx {double time;std::string status;};
struct Rx {std::size_t tx;double time;};
void require(bool value,const std::string &why){if(!value)throw std::runtime_error(why);}
std::vector<std::string> lines(const std::string &path) {
    std::vector<std::uint8_t> bytes;std::string error;
    require(cw::read_file(path,bytes,error),error);
    std::vector<std::string> result;std::istringstream in(std::string(bytes.begin(),bytes.end()));std::string s;
    while(std::getline(in,s)){if(!s.empty()&&s.back()=='\r')s.pop_back();if(!s.empty())result.push_back(s);}return result;
}
std::vector<Rx> received_times(const std::string &root) {
    std::vector<Rx> rx;auto rows=lines(root+"/rx-times.csv");
    require(!rows.empty()&&rows[0]=="packet,tx_packet,time_ms","invalid RX timing header");
    for(std::size_t i=1;i<rows.size();++i){
        auto s=rows[i];std::replace(s.begin(),s.end(),',',' ');std::istringstream in(s);std::size_t id;Rx row{};
        require(bool(in>>id>>row.tx>>row.time)&&id==i&&std::isfinite(row.time)&&row.time>=0,"invalid RX timing row");
        if(!rx.empty())require(row.time-rx.back().time>=20.0-0.000001,"RX interval below 20 ms");
        rx.push_back(row);
    }
    return rx;
}
std::size_t replay_received_only(const std::string &root) {
    // This function opens exactly three inputs. TX logs, source video, summary,
    // image snapshots and source-frame observation records are never opened.
    const auto rx=received_times(root);std::uint64_t size=0;
    require(cw::file_size(root+"/rx-300.bin",size)&&size==rx.size()*kPacketBytes,"RX file length is not 300 bytes per packet");
    cw::StreamFile input;require(input.open(root+"/rx-300.bin","rb"),"cannot open received packets");
    const auto expected=lines(root+"/decoded.csv");
    require(!expected.empty()&&expected[0]=="ordinal,frame_id,pts,width,height,sha256","invalid fingerprint header");
    RoboGo::video::CustomByteBlockVideoDecoder decoder;
    std::size_t decoded=0;std::string actual="ordinal,frame_id,pts,width,height,sha256\n";
    auto absorb=[&](auto frames){for(const auto &f:frames){auto line=fingerprint_line(++decoded,f.frame_id,f.pts,f.width,f.height,f.rgba);require(decoded<expected.size()&&line==expected[decoded]+"\n","replayed pixels/frame order/dimensions do not match runtime");actual+=line;}};
    const auto origin=std::chrono::steady_clock::now();
    for(const auto &row:rx){std::this_thread::sleep_until(origin+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double,std::milli>(row.time)));Transport data{};require(input.read(data.data(),data.size())==data.size(),"truncated RX replay");absorb(decoder.push_packet_bytes(data));}
    absorb(decoder.flush());require(decoded+1==expected.size(),"runtime has additional decoded frames absent from RX capture");
    std::string error;require(cw::write_file(root+"/replayed-decoded.csv",actual.data(),actual.size(),error),error);
    std::ostringstream report;report<<"PASS RX-only independent replay\nsource_video_read=0\nobservation_ids_used=0\ntx_records_read=0\nsummary_read=0\nclient_decoder=unmodified\nrx_packets="<<rx.size()<<"\ndecoded="<<decoded<<"\npixel_sha256_match=1\n";
    const auto result=report.str();require(cw::write_file(root+"/rx-replay-verification.txt",result.data(),result.size(),error),error);cw::out("%s",result.c_str());return decoded;
}
struct AccessUnitAudit {
    AVCodecParserContext *parser=av_parser_init(AV_CODEC_ID_H264);
    AVCodecContext *context=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    std::size_t units=0,largest=0,largest_packets=0,idr_units=0,largest_idr=0,sps=0,pps=0;
    struct Span {std::int64_t begin,end;std::size_t packet;};
    std::vector<Span> spans;
    std::int64_t offset=0;
    ~AccessUnitAudit(){if(parser)av_parser_close(parser);if(context)avcodec_free_context(&context);}
    void output(const std::uint8_t *data,int size) {
        if(!size)return;
        require(size<=static_cast<int>(kFrameBytes),"captured H264 access unit exceeds 592 bytes");
        const auto begin=parser->frame_offset,end=begin+size;std::size_t count=0;
        for(const auto &s:spans)if(s.begin<end&&s.end>begin)++count;
        require(count<=kFramePackets,"captured H264 access unit spans more than two packets");
        ++units;largest=std::max(largest,static_cast<std::size_t>(size));largest_packets=std::max(largest_packets,count);
        bool idr=false;
        for(int i=0;i+3<size;++i)if(data[i]==0&&data[i+1]==0&&data[i+2]==1){
            const int type=data[i+3]&31;require(type!=6,"unexpected SEI NAL in transmitted bitstream");
            if(type==5)idr=true;if(type==7)++sps;if(type==8)++pps;
        }
        if(idr){++idr_units;largest_idr=std::max(largest_idr,static_cast<std::size_t>(size));}
    }
    void push(std::span<const std::uint8_t> payload,std::size_t packet) {
        require(parser&&context,"H264 parser initialization failed");
        spans.push_back({offset,offset+static_cast<std::int64_t>(payload.size()),packet});offset+=payload.size();
        const auto *p=payload.data();std::size_t left=payload.size();
        while(left) {
            std::uint8_t *data=nullptr;int n=0;
            const int used=av_parser_parse2(parser,context,&data,&n,p,static_cast<int>(left),AV_NOPTS_VALUE,AV_NOPTS_VALUE,0);
            require(used>=0,"captured H264 parser failure");output(data,n);
            p+=used;left-=used;if(!used&&!n)break;
        }
    }
    void flush(){std::uint8_t *data=nullptr;int n=0;require(av_parser_parse2(parser,context,&data,&n,nullptr,0,AV_NOPTS_VALUE,AV_NOPTS_VALUE,0)>=0,"H264 parser flush failure");output(data,n);}
};
}
int main(int argc,char **argv) {
    cw::init_console();cw::TimingResolution timing;
    const auto args=cw::collect_args(argc,argv);
    const bool rx_only=args.size()==2&&args[0]=="--rx-only";
    if(args.size()!=1&&!rx_only){cw::out("cw_wire_verify [--rx-only] <run-directory>\nRX-only inputs: rx-300.bin, rx-times.csv, decoded.csv (comparison only).\nDefault also audits TX framing/CRC/budget, without source-frame observation IDs.\n");return 2;}
    const auto root=args.back();
    try {
        const std::string pending="PENDING validation; no PASS result for this attempt\n";std::string write_error;
        require(cw::write_file(root+"/rx-replay-verification.txt",pending.data(),pending.size(),write_error),write_error);
        if(!rx_only)require(cw::write_file(root+"/wire-verification.txt",pending.data(),pending.size(),write_error),write_error);
        if(rx_only){replay_received_only(root);return 0;}
        bool natural=false;for(const auto &s:lines(root+"/summary.txt"))if(s=="natural_end=1")natural=true;
        require(natural,"capture did not complete naturally; not a complete replay proof");
        std::vector<Tx> tx;auto txlines=lines(root+"/tx-times.csv");
        require(!txlines.empty()&&txlines[0]=="packet,time_ms,source_frame,status","invalid TX timing header");
        for(std::size_t i=1;i<txlines.size();++i){auto s=txlines[i];std::replace(s.begin(),s.end(),',',' ');std::istringstream in(s);std::size_t id;std::string ignored;Tx row{};require(bool(in>>id>>row.time>>ignored>>row.status)&&id==i&&std::isfinite(row.time)&&row.time>=0,"invalid TX timing row");require(row.status=="accepted"||row.status=="network-drop"||row.status=="rx-overflow","unknown TX status");tx.push_back(row);}
        const auto rx=received_times(root);
        std::uint64_t txsize=0,rxsize=0;require(cw::file_size(root+"/tx-0310.bin",txsize)&&txsize==tx.size()*309,"TX file length is not 309 bytes per packet");
        require(cw::file_size(root+"/rx-300.bin",rxsize)&&rxsize==rx.size()*kPacketBytes,"RX file length is not 300 bytes per packet");
        cw::StreamFile txfile,rxfile;require(txfile.open(root+"/tx-0310.bin","rb")&&rxfile.open(root+"/rx-300.bin","rb"),"capture open failed");
        AccessUnitAudit audit;std::size_t accepted=0,max_window=0,left=0;
        std::uint64_t h264=0,padding=0;double min_gap=1e30;
        for(std::size_t i=0;i<tx.size();++i) {
            if(i){const double gap=tx[i].time-tx[i-1].time;require(gap>=20.0-0.000001,"captured packet interval below 20 ms");min_gap=std::min(min_gap,gap);}
            while(left<i&&tx[i].time-tx[left].time>=1000)++left;max_window=std::max(max_window,i-left+1);require(max_window<=50,"more than 50 packets in a rolling second");
            Outer outer{};Transport data{};require(txfile.read(outer.data(),outer.size())==outer.size()&&unwrap(outer,data),"TX CRC/outer frame validation failed");require(outer[3]==static_cast<std::uint8_t>(i),"outer sequence mismatch");
            const auto inner=(data[0]<<8)|data[1];require(inner>=3&&inner<=298,"invalid inner packet length");const std::size_t n=inner-2;
            require(std::all_of(data.begin()+4+n,data.end(),[](auto b){return b==0;}),"nonzero padding");h264+=n;padding+=kPayloadBytes-n;
            audit.push(std::span(data).subspan(4,n),i);
            if(tx[i].status=="accepted") {
                require(accepted<rx.size()&&rx[accepted].tx==i+1&&std::abs(rx[accepted].time-tx[i].time)<0.000001,"RX/TX timing mapping mismatch");
                Transport received{};require(rxfile.read(received.data(),received.size())==received.size()&&received==data,"RX packet differs from actual TX payload");++accepted;
            }
        }
        audit.flush();require(accepted==rx.size(),"unaccounted RX packets");
        require(!audit.units||(audit.sps&&audit.pps),"SPS/PPS absent from transmitted stream");
        const auto decoded=replay_received_only(root);std::string error;
        std::ostringstream report;report<<"PASS independent captured-packet replay\nsource_video_read=0\nobservation_ids_used=0\nclient_decoder=unmodified\n"
          <<"tx_packets="<<tx.size()<<"\nrx_packets="<<rx.size()<<"\ndecoded="<<decoded<<"\nmax_packets_rolling_second="<<max_window<<"\nmin_packet_gap_ms="<<(tx.size()>1?min_gap:0)
          <<"\nmax_h264_au_bytes="<<audit.largest<<"\nmax_h264_au_packets="<<audit.largest_packets
          <<"\nh264_access_units="<<audit.units<<"\nidr_access_units="<<audit.idr_units<<"\nmax_idr_h264_bytes="<<audit.largest_idr
          <<"\nsei_nals=0\nsps_nals="<<audit.sps<<"\npps_nals="<<audit.pps
          <<"\ntx_h264_bytes="<<h264<<"\ntx_inner_header_bytes="<<tx.size()*4<<"\ntx_padding_bytes="<<padding<<"\ntx_data_bytes="<<tx.size()*kPacketBytes<<"\ntx_outer_bytes="<<txsize<<"\npixel_sha256_match=1\n";
        const auto s=report.str();require(cw::write_file(root+"/wire-verification.txt",s.data(),s.size(),error),error);cw::out("%s",s.c_str());return 0;
    }catch(const std::exception &e){
        const std::string failure="FAIL independent replay: "+std::string(e.what())+"\n";std::string ignored;
        cw::write_file(root+(rx_only?"/rx-replay-verification.txt":"/wire-verification.txt"),failure.data(),failure.size(),ignored);
        cw::eout("%s",failure.c_str());return 1;
    }
}
