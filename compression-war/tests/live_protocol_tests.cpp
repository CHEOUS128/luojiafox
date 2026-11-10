#include "live_engine.h"
#include "live_protocol.h"
#include "packetizer.h"
#include "platform_util.h"
#include <algorithm>
#include <stdexcept>
namespace cw::live {
int protocol_self_test() {
    int checks=0;
    auto check=[&](bool ok,const char *name){if(!ok)throw std::runtime_error(name);++checks;};
    try {
        Transport t{},out{};for(int i=0;i<300;++i)t[i]=static_cast<std::uint8_t>(i);
        auto f=wrap(t,255);
        // Independent golden values computed using the unmodified car CRC lookup tables.
        check(f.size()==309&&f[4]==0x1d&&f[307]==0x70&&f[308]==0xeb,"car CRC golden vector");
        check(unwrap(f,out)&&out==t,"outer payload preservation");
        for(std::size_t i=0;i<f.size();++i){auto bad=f;bad[i]^=1;check(!unwrap(bad,out),"corrupt frame accepted");}
        for(int i=0;i<300;++i){auto frame=wrap(t,static_cast<std::uint8_t>(i));check(unwrap(frame,out)&&frame[3]==i%256,"outer sequence wrap");}
        auto pb=protobuf(t);check(pb.size()==303&&pb[0]==10&&pb[1]==0xac&&pb[2]==2,"protobuf wire format");
        check(unprotobuf(pb,out)&&out==t,"protobuf binary preservation");pb[1]=0xad;check(!unprotobuf(pb,out),"bad protobuf length accepted");
        TransportSpec spec;Packetizer p(spec);std::vector<WirePacket> packets;
        check(!p.push_frame({},0,packets),"empty AU");
        for(auto n:{1,296,297,2368}) {
            std::vector<std::uint8_t> au(n,0x53);check(p.push_frame(au,1,packets),"legal AU rejected");
            check(packets.size()==(n+295)/296,"fragment count");
            for(const auto &packet:packets){check(packet.bytes.size()==300,"transport size");auto len=(packet.bytes[0]<<8)|packet.bytes[1];check(len>=3&&len<=298,"inner length");}
        }
        check(!p.push_frame(std::vector<std::uint8_t>(2369),2,packets),"oversize AU accepted");
        TransportSpec two;two.max_packets_per_frame=kFramePackets;Packetizer strict(two);
        for(auto n:{1,296,297,592})check(strict.push_frame(std::vector<std::uint8_t>(n,0x61),1,packets)&&packets.size()<=2,"two-packet legal boundary");
        check(!strict.push_frame(std::vector<std::uint8_t>(593),2,packets),"third packet accepted");
        const std::vector<std::uint8_t> abc{'a','b','c'};
        check(pixel_sha256(abc)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 golden");
        Packetizer sequence(spec);
        for(int i=0;i<65538;++i){check(sequence.push_frame({0x61},3,packets),"sequence AU");check(((packets[0].bytes[2]<<8)|packets[0].bytes[3])==i%65536,"inner sequence wrap");}
        for(auto shape:{std::pair{1280,1024},std::pair{1920,1080},std::pair{720,1280}}){
            const auto b=fit(shape.first,shape.second,192,152);
            check(b.width>0&&b.height>0&&b.x>=0&&b.y>=0&&b.x+b.width<=192&&b.y+b.height<=152,"aspect fit bounds");
            check(std::abs(double(b.width)/b.height-double(shape.first)/shape.second)<.02,"aspect ratio");
        }
        const std::vector<std::uint8_t> a{0,0,0,1,0x67,0x12,0,0,1,0x65,0x88,0x99};
        const std::vector<std::uint8_t> b{0,0,1,0x65,0x88,0x99};
        check(vcl_hash(a)==vcl_hash(b)&&vcl_hash(a)!=0,"trace hash parser grouping");
        Config c;std::string error;c.send_rate=51;check(!validate(c,error),"over-rate validation");c.send_rate=50;c.width=191;check(!validate(c,error),"odd width validation");
        c.width=192;c.fps=26;check(!validate(c,error),"over-25-fps accepted");c.fps=25;check(validate(c,error),"25-fps rejected");
        Strategy st;st.width=2147483647;st.upscale=4;check(!validate_strategy(st,error),"strategy integer overflow accepted");
        st=Strategy{};st.width=32;st.height=32;Image scene;scene.width=64;scene.height=32;scene.bgr.assign(64*32*3,0);
        for(std::size_t i=1;i<scene.bgr.size();i+=3)scene.bgr[i]=100;
        ProcessingState history;preprocess_strategy(scene,st,history);
        check(scene.width==32&&scene.height==32,"strategy output dimensions");
        check(scene.bgr[1]==0&&scene.bgr[(8*32)*3+1]==100&&scene.bgr[(23*32+31)*3+1]==100&&scene.bgr[(24*32)*3+1]==0,"full view/padding calibration");
        st.color="gray";preprocess_strategy(scene,st,history);bool neutral=true;
        for(std::size_t i=0;i<scene.bgr.size();i+=3)neutral=neutral&&scene.bgr[i]==scene.bgr[i+1]&&scene.bgr[i]==scene.bgr[i+2];check(neutral,"gray processing retains color");
        st=Strategy{};st.width=32;st.height=32;st.temporal=8;history={};scene.bgr.assign(32*32*3,30);preprocess_strategy(scene,st,history);
        scene.bgr.assign(32*32*3,200);preprocess_strategy(scene,st,history);check(std::all_of(scene.bgr.begin(),scene.bgr.end(),[](auto v){return v==200;}),"temporal gate ghosts large motion");
        st.sharpen=2;reconstruct_pixels(scene.bgr,scene.width,scene.height,st);check(std::all_of(scene.bgr.begin(),scene.bgr.end(),[](auto v){return v==200;}),"sharpen changes flat color");
        cw::out("PASS live protocol: %d checks (car CRC golden, corruption, protobuf, packet limits, sequence wraps, full FOV fit)\n",checks);return 0;
    }catch(const std::exception &e){cw::eout("FAIL live protocol: %s\n",e.what());return 1;}
}
} // namespace cw::live
