// Compare the independent observed decoder with the unchanged client decoder.
#include "video/custom_byte_block_video_decoder.h"
#include "live_protocol.h"
#include "packetizer.h"
#include "raw_io.h"
#include "platform_util.h"
#include <map>

int main(int argc,char **argv) {
    cw::init_console();auto args=cw::collect_args(argc,argv);
    if(args.empty())return 2;
    std::vector<cw::AccessUnit> units;std::string error;
    if(!cw::split_access_units(args[0],units,error)){cw::eout("%s\n",error.c_str());return 2;}
    const int drop=args.size()>1?std::stoi(args[1]):0;
    std::map<std::uint64_t,std::uint64_t> hashes;
    std::map<std::uint64_t,std::size_t> expected,received;
    RoboGo::video::CustomByteBlockVideoDecoderConfig config;
#ifdef PROBE_LIVE
    config.trace_resolver=[&](std::span<const std::uint8_t> au,std::uint64_t id)->std::uint64_t {
        return hashes.contains(id)&&received[id]==expected[id]&&hashes[id]==cw::live::vcl_hash(au)?id:0;
    };
#endif
    RoboGo::video::CustomByteBlockVideoDecoder decoder(config);
    cw::TransportSpec spec;cw::Packetizer packetizer(spec);
    std::size_t count=0,associated=0,unknown=0;std::uint64_t last=0;int packet=0;
    auto absorb=[&](auto frames) {
        for(const auto &f:frames) {
            const auto hash=cw::live::pixel_sha256(f.rgba);
            cw::out("F %u %lld %d %d %s\n",f.frame_id,static_cast<long long>(f.pts),f.width,f.height,hash.c_str());++count;
#ifdef PROBE_LIVE
            if(f.trace_id){
                if(f.trace_id<=last)throw std::runtime_error("trace went backwards or reused a frame");
                last=f.trace_id;++associated;
            } else ++unknown;
#endif
        }
    };
    try {
        for(std::size_t i=0;i<units.size();++i) {
            const auto id=100001+i;std::vector<cw::WirePacket> packets;
            if(!packetizer.push_frame(units[i].bytes,static_cast<std::uint32_t>(id),packets))continue;
            hashes[id]=cw::live::vcl_hash(units[i].bytes);expected[id]=packets.size();
            for(const auto &p:packets) {
                if(++packet==drop)continue;
                ++received[id];
#ifdef PROBE_LIVE
                absorb(decoder.push_packet_bytes(p.bytes,id));
#else
                absorb(decoder.push_packet_bytes(p.bytes));
#endif
            }
        }
        absorb(decoder.flush());cw::out("END %zu\n",count);
#ifdef PROBE_LIVE
        if(!associated||(!drop&&unknown))throw std::runtime_error("clean trace association failed");
        cw::out("TRACE associated=%zu unknown=%zu\n",associated,unknown);
#endif
        return 0;
    }catch(const std::exception &e){cw::eout("FAIL %s\n",e.what());return 1;}
}
