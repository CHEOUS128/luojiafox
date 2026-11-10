#include "strategy.h"
#include "live_engine.h"
#include "live_protocol.h"
#include "platform_util.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
extern "C" {
#include <libswscale/swscale.h>
}
namespace cw::live {
namespace {
std::uint8_t byte(double v){return static_cast<std::uint8_t>(std::clamp(std::lround(v),0L,255L));}
void scale(std::vector<std::uint8_t> &bgr,int &w,int &h,int nw,int nh,int flags){
    auto ctx=sws_getContext(w,h,AV_PIX_FMT_BGR24,nw,nh,AV_PIX_FMT_BGR24,flags,nullptr,nullptr,nullptr);
    if(!ctx)throw std::runtime_error("strategy scaler allocation failed");
    std::vector<std::uint8_t> out(static_cast<std::size_t>(nw)*nh*3);
    const std::uint8_t *src[]={bgr.data()};int ss[]={w*3};std::uint8_t *dst[]={out.data()};int ds[]={nw*3};
    const int n=sws_scale(ctx,src,ss,0,h,dst,ds);sws_freeContext(ctx);
    if(n!=nh)throw std::runtime_error("strategy scaler failed");bgr=std::move(out);w=nw;h=nh;
}
}
bool validate_strategy(const Strategy &s,std::string &e){
    if(s.width<16||s.height<16||s.width>4096||s.height>4096||s.width%2||s.height%2||!std::isfinite(s.fps)||s.fps<=0||s.fps>25)e="invalid strategy dimensions/fps";
    else if(s.color!="color"&&s.color!="gray")e="color must be color or gray";
    else if(std::set<std::string>{"ultrafast","superfast","veryfast","faster","fast","medium","slow","slower","veryslow"}.count(s.preset)==0)e="unsupported x264 preset";
    else if(s.gop<1||s.gop>1000||s.slices<1||s.slices>64||s.threads<0||s.threads>64||s.aq<0||s.aq>3)e="invalid encoder strategy";
    else if(s.bitrate<0||s.bitrate>118400)e="bitrate outside channel payload capacity";
    else if(!std::isfinite(s.aq_strength)||s.aq_strength<0||s.aq_strength>3||!std::isfinite(s.denoise)||s.denoise<0||s.denoise>128||!std::isfinite(s.temporal)||s.temporal<0||s.temporal>128||!std::isfinite(s.sharpen)||s.sharpen<0||s.sharpen>4)e="invalid processing strength";
    else if(s.chroma_block<1||s.chroma_block>256||s.upscale<1||s.upscale>4||s.width*s.upscale>4096||s.height*s.upscale>4096)e="invalid processing dimensions";
    else return true;return false;
}
Strategy load_strategy(const std::string &path,bool receiver_only){
    std::vector<std::uint8_t> data;std::string error;if(!read_file(path,data,error))throw std::runtime_error(error);
    std::istringstream in(std::string(data.begin(),data.end()));std::string line;Strategy s;s.offline_metrics=!receiver_only;std::set<std::string> seen;
    while(std::getline(in,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty()||line[0]=='#')continue;
        auto eq=line.find('=');if(eq==std::string::npos)throw std::runtime_error("strategy line requires key=value");
        const auto k=line.substr(0,eq),v=line.substr(eq+1);if(!seen.insert(k).second)throw std::runtime_error("duplicate strategy key: "+k);
        if(receiver_only&&k!="sharpen"&&k!="upscale")throw std::runtime_error("receiver configuration contains non-receiver key: "+k);
        std::size_t end=0;auto num=[&](){auto x=std::stod(v,&end);if(end!=v.size()||!std::isfinite(x))throw std::runtime_error("invalid numeric value: "+k);return x;};
        auto integer=[&](){auto x=num();if(x!=std::floor(x)||x<-2147483647||x>2147483647)throw std::runtime_error("invalid integer: "+k);return int(x);};
        if(k=="name")s.name=v;else if(k=="label")s.label=v;else if(k=="family")s.family=v;else if(k=="color")s.color=v;else if(k=="preset")s.preset=v;
        else if(k=="width")s.width=integer();else if(k=="height")s.height=integer();else if(k=="fps")s.fps=num();
        else if(k=="gop")s.gop=integer();else if(k=="slices")s.slices=integer();else if(k=="aq")s.aq=integer();else if(k=="threads")s.threads=integer();
        else if(k=="bitrate")s.bitrate=integer();else if(k=="aq_strength")s.aq_strength=num();else if(k=="denoise")s.denoise=num();
        else if(k=="temporal")s.temporal=num();else if(k=="chroma_block")s.chroma_block=integer();else if(k=="sharpen")s.sharpen=num();else if(k=="upscale")s.upscale=integer();
        else if(k=="offline_metrics"){const int n=integer();if(n!=0&&n!=1)throw std::runtime_error("offline_metrics must be 0 or 1");s.offline_metrics=n!=0;}
        else throw std::runtime_error("unknown strategy key: "+k);
    }
    if(!validate_strategy(s,error))throw std::runtime_error(error);return s;
}
std::string strategy_text(const Strategy &s,bool receiver_only){
    std::ostringstream o;o.precision(12);
    if(!receiver_only)o<<"name="<<s.name<<"\nlabel="<<s.label<<"\nfamily="<<s.family<<"\ncolor="<<s.color<<"\npreset="<<s.preset
      <<"\nwidth="<<s.width<<"\nheight="<<s.height<<"\nfps="<<s.fps<<"\ngop="<<s.gop<<"\nslices="<<s.slices<<"\naq="<<s.aq<<"\naq_strength="<<s.aq_strength
      <<"\nbitrate="<<s.bitrate<<"\nthreads="<<s.threads<<"\ndenoise="<<s.denoise<<"\ntemporal="<<s.temporal<<"\nchroma_block="<<s.chroma_block<<"\noffline_metrics="<<s.offline_metrics<<'\n';
    o<<"sharpen="<<s.sharpen<<"\nupscale="<<s.upscale<<'\n';return o.str();
}
void preprocess_strategy(Image &im,const Strategy &s,ProcessingState &state){
    const auto f=fit(im.width,im.height,s.width,s.height);
    scale(im.bgr,im.width,im.height,f.width,f.height,SWS_AREA);
    std::vector<std::uint8_t> fitted(static_cast<std::size_t>(s.width)*s.height*3,0);
    for(int y=0;y<f.height;++y)std::copy_n(im.bgr.data()+static_cast<std::size_t>(y)*f.width*3,f.width*3,fitted.data()+(static_cast<std::size_t>(y+f.y)*s.width+f.x)*3);
    im.width=s.width;im.height=s.height;im.bgr=std::move(fitted);auto &v=im.bgr;
    if(s.denoise>0){
        const auto in=v;const double denom=2*s.denoise*s.denoise;
        for(int y=f.y;y<f.y+f.height;++y)for(int x=f.x;x<f.x+f.width;++x){
            const auto p=(static_cast<std::size_t>(y)*im.width+x)*3;double sum[3]={},ws=0;
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
                int yy=std::clamp(y+dy,f.y,f.y+f.height-1),xx=std::clamp(x+dx,f.x,f.x+f.width-1);auto q=(static_cast<std::size_t>(yy)*im.width+xx)*3;
                double d=0;for(int c=0;c<3;++c){double a=int(in[p+c])-int(in[q+c]);d+=a*a/3;}
                double w=std::exp(-d/denom-.5*(dx*dx+dy*dy));ws+=w;for(int c=0;c<3;++c)sum[c]+=in[q+c]*w;
            }for(int c=0;c<3;++c)v[p+c]=byte(sum[c]/ws);
        }
    }
    if(s.temporal>0){
        if(state.width==im.width&&state.height==im.height&&state.previous.size()==v.size())
            for(std::size_t p=0;p<v.size();p+=3){int d=0;for(int c=0;c<3;++c)d=std::max(d,std::abs(int(v[p+c])-int(state.previous[p+c])));if(d<=s.temporal)for(int c=0;c<3;++c)v[p+c]=std::uint8_t((3*int(v[p+c])+state.previous[p+c]+2)/4);}
        state.previous=v;state.width=im.width;state.height=im.height;
    }
    if(s.color=="gray")for(std::size_t p=0;p<v.size();p+=3){auto y=std::uint8_t((77*v[p+2]+150*v[p+1]+29*v[p]+128)>>8);v[p]=v[p+1]=v[p+2]=y;}
    else if(s.chroma_block>1){
        // Low-resolution chroma remains inside the transmitted H.264 picture.
        // No side information, source IDs or original colors reach the receiver.
        const auto in=v;
        for(int y=f.y;y<f.y+f.height;y+=s.chroma_block)for(int x=f.x;x<f.x+f.width;x+=s.chroma_block){
            int y1=std::min(y+s.chroma_block,f.y+f.height),x1=std::min(x+s.chroma_block,f.x+f.width);double cb=0,cr=0;int n=0;
            for(int yy=y;yy<y1;++yy)for(int xx=x;xx<x1;++xx){auto p=(static_cast<std::size_t>(yy)*im.width+xx)*3;double Y=.299*in[p+2]+.587*in[p+1]+.114*in[p];cb+=(in[p]-Y)/1.772;cr+=(in[p+2]-Y)/1.402;++n;}
            cb/=n;cr/=n;
            for(int yy=y;yy<y1;++yy)for(int xx=x;xx<x1;++xx){auto p=(static_cast<std::size_t>(yy)*im.width+xx)*3;double Y=.299*in[p+2]+.587*in[p+1]+.114*in[p];v[p]=byte(Y+1.772*cb);v[p+1]=byte(Y-.344136*cb-.714136*cr);v[p+2]=byte(Y+1.402*cr);}
        }
    }
}
void reconstruct_pixels(std::vector<std::uint8_t> &v,int &w,int &h,const Strategy &s){
    if(s.upscale>1)scale(v,w,h,w*s.upscale,h*s.upscale,SWS_BICUBIC);
    if(s.sharpen<=0)return;const auto in=v;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)for(int c=0;c<3;++c){
        int sum=0;for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)sum+=in[(static_cast<std::size_t>(std::clamp(y+dy,0,h-1))*w+std::clamp(x+dx,0,w-1))*3+c]*(dy==0?2:1)*(dx==0?2:1);
        auto p=(static_cast<std::size_t>(y)*w+x)*3+c;
        const double detail=std::clamp(double(in[p])-sum/16.,-12.,12.);v[p]=byte(in[p]+s.sharpen*detail);
    }
}
std::vector<std::uint8_t> rgba_pixels(const std::vector<std::uint8_t> &bgr){
    std::vector<std::uint8_t> out(bgr.size()/3*4);for(std::size_t i=0;i<bgr.size()/3;++i){out[4*i]=bgr[3*i+2];out[4*i+1]=bgr[3*i+1];out[4*i+2]=bgr[3*i];out[4*i+3]=255;}return out;
}
}
