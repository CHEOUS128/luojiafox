#include "live_protocol.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <sstream>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
namespace cw::live {
std::string pixel_sha256(std::span<const std::uint8_t> bytes) {
    struct Algorithm {
        BCRYPT_ALG_HANDLE handle=nullptr;
        Algorithm(){if(BCryptOpenAlgorithmProvider(&handle,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 provider failed");}
        ~Algorithm(){BCryptCloseAlgorithmProvider(handle,0);}
    };
    static Algorithm algorithm;
    BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptCreateHash(algorithm.handle,&hash,nullptr,0,nullptr,0,0)<0)throw std::runtime_error("SHA256 create failed");
    std::array<unsigned char,32> digest{};
    const auto status=BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0);
    const auto finish=status<0?status:BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0);
    BCryptDestroyHash(hash);
    if(finish<0)throw std::runtime_error("SHA256 digest failed");
    static constexpr char digits[]="0123456789abcdef";std::string result;
    for(auto b:digest){result+=digits[b>>4];result+=digits[b&15];}return result;
}
std::string fingerprint_line(std::uint64_t ordinal,std::uint32_t frame_id,std::int64_t pts,
                             int width,int height,std::span<const std::uint8_t> rgba) {
    std::ostringstream s;s<<ordinal<<','<<frame_id<<','<<pts<<','<<width<<','<<height<<','<<pixel_sha256(rgba)<<'\n';return s.str();
}
std::uint8_t crc8(std::span<const std::uint8_t> b) {
    std::uint8_t c = 0xff;
    for (auto v : b) { c ^= v; for (int i=0;i<8;++i) c = (c & 1) ? (c >> 1) ^ 0x8c : c >> 1; }
    return c;
}
std::uint16_t crc16(std::span<const std::uint8_t> b) {
    std::uint16_t c = 0xffff;
    for (auto v : b) { c ^= v; for (int i=0;i<8;++i) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1; }
    return c;
}
Outer wrap(const Transport &data, std::uint8_t seq) {
    Outer f{}; f[0]=0xa5; f[1]=0x2c; f[2]=1; f[3]=seq;
    f[4]=crc8(std::span(f).first(4)); f[5]=0x10; f[6]=3;
    std::copy(data.begin(), data.end(), f.begin()+7);
    auto crc=crc16(std::span(f).first(307)); f[307]=crc&255; f[308]=crc>>8; return f;
}
bool unwrap(std::span<const std::uint8_t> f, Transport &data) {
    if(f.size()!=309 || f[0]!=0xa5 || f[1]!=0x2c || f[2]!=1 || f[5]!=0x10 || f[6]!=3 ||
       f[4]!=crc8(f.first(4)) || (f[307]|(f[308]<<8))!=crc16(f.first(307))) return false;
    std::copy(f.begin()+7, f.begin()+307, data.begin()); return true;
}
std::vector<std::uint8_t> protobuf(const Transport &data) {
    std::vector<std::uint8_t> b{0x0a,0xac,0x02}; b.insert(b.end(),data.begin(),data.end()); return b;
}
bool unprotobuf(std::span<const std::uint8_t> b, Transport &data) {
    // CustomByteBlock: field 1, wire type 2, length 300 (varint AC 02).
    if(b.size()!=303 || b[0]!=0x0a || b[1]!=0xac || b[2]!=2) return false;
    std::copy(b.begin()+3,b.end(),data.begin()); return true;
}
std::uint64_t vcl_hash(std::span<const std::uint8_t> a) {
    std::uint64_t h=1469598103934665603ULL; bool vcl=false;
    std::vector<std::size_t> starts;
    for(std::size_t i=0;i+3<a.size();++i) {
        if(a[i]==0 && a[i+1]==0 && a[i+2]==1) { starts.push_back(i+3); i+=2; }
    }
    for(std::size_t j=0;j<starts.size();++j) {
        auto begin=starts[j], end=j+1<starts.size()?starts[j+1]-3:a.size();
        while(end>begin && a[end-1]==0) --end;
        const int type=a[begin]&31;
        if(type<1 || type>5) continue;
        vcl=true;
        // Include a NAL separator; start-code length and trailing_zero_8bits are irrelevant.
        h=(h^0xff)*1099511628211ULL;
        for(auto i=begin;i<end;++i) h=(h^a[i])*1099511628211ULL;
    }
    return vcl?h:0;
}
Fit fit(int sw,int sh,int tw,int th) {
    if(sw<=0||sh<=0||tw<=0||th<=0) return {};
    const double scale=std::min(double(tw)/sw,double(th)/sh);
    const int w=std::max(1,int(std::floor(sw*scale+0.00001)));
    const int h=std::max(1,int(std::floor(sh*scale+0.00001)));
    return {(tw-w)/2,(th-h)/2,w,h};
}
} // namespace cw::live
