#include "live_engine.h"
#include "live_protocol.h"
#include "platform_util.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>
#include <memory>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <psapi.h>

using namespace cw::live;
namespace {
Config cfg;
std::unique_ptr<Engine> engine;
View current;
std::string startup_error;
bool autostart=false,close_on_end=false,right_native=false;
HFONT font=nullptr;
std::uint64_t last_probe_second=0;
enum { Path=100,Choose,Start,Stop,Replay,Width,Height,Fps,Rate,Loop,RawWidth,RawHeight,SourceFps,Native };
std::wstring wide(const std::string &s){return cw::to_native_path(s);}
std::string utf8(const std::wstring &w){
    const int n=WideCharToMultiByte(CP_UTF8,0,w.data(),static_cast<int>(w.size()),nullptr,0,nullptr,nullptr);
    std::string s(n,'\0');if(n)WideCharToMultiByte(CP_UTF8,0,w.data(),static_cast<int>(w.size()),s.data(),n,nullptr,nullptr);return s;
}
std::wstring field(HWND w,int id){wchar_t b[32768]{};GetDlgItemTextW(w,id,b,32768);return b;}
void set(HWND w,int id,const std::wstring &s){SetDlgItemTextW(w,id,s.c_str());}
HWND control(HWND w,const wchar_t *kind,const wchar_t *text,int id,int x,int y,int width,int height,DWORD style=0){
    auto c=CreateWindowExW(kind==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,kind,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,w,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return c;
}
void label(HWND w,const wchar_t *s,int x,int y,int width){control(w,L"STATIC",s,0,x,y,width,23);}
void choose(HWND w){
    wchar_t name[32768]{};OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=w;ofn.lpstrFile=name;ofn.nMaxFile=32768;
    ofn.lpstrFilter=L"视频及相机原片\0*.mp4;*.mkv;*.mov;*.avi;*.h265;*.h264;*.bgr;*.raw;*.bin\0全部文件\0*.*\0";
    ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(GetOpenFileNameW(&ofn))set(w,Path,name);
}
void stop(){if(engine){engine->stop();current=engine->view();}}
void start(HWND w){
    stop();startup_error.clear();current=View{};current.status="启动中";
    try {
        cfg.input=utf8(field(w,Path));cfg.width=std::stoi(field(w,Width));cfg.height=std::stoi(field(w,Height));
        cfg.fps=std::stod(field(w,Fps));cfg.send_rate=std::stod(field(w,Rate));
        cfg.raw_width=std::stoi(field(w,RawWidth));cfg.raw_height=std::stoi(field(w,RawHeight));cfg.source_fps=std::stod(field(w,SourceFps));
        cfg.loop=SendDlgItemMessageW(w,Loop,BM_GETCHECK,0,0)==BST_CHECKED;
        engine=std::make_unique<Engine>(cfg);cfg.output.clear();
        if(!engine->start(startup_error))engine.reset();
        last_probe_second=0;
    }catch(const std::exception &){startup_error="参数须填写有效数字";engine.reset();}
    for(int id:{Path,Choose,Width,Height,Fps,Rate,Loop,RawWidth,RawHeight,SourceFps})EnableWindow(GetDlgItem(w,id),!engine);
    InvalidateRect(w,nullptr,FALSE);
}
void probe(const View &v){
    const auto sec=static_cast<std::uint64_t>(v.now_ms/1000);
    if(sec==last_probe_second)return;last_probe_second=sec;
    PROCESS_MEMORY_COUNTERS pm{};GetProcessMemoryInfo(GetCurrentProcess(),&pm,sizeof(pm));
    cw::out("live_probe t=%.0fms input=%llu encoded=%llu decoded=%llu displayed=%llu packets=%llu fps=%.0f decode_fps=%.0f pps=%.0f age=%.1fms queue=%zu rx=%zu memory_mb=%.1f\n",v.now_ms,v.input,v.encoded,v.decoded,v.displayed,v.sent,v.update_fps,v.decode_fps,v.packets_sec,v.age_ms,v.queued_frames,v.rx_packets,double(pm.WorkingSetSize)/1048576);
    std::fflush(stdout);
}
void text(HDC dc,int x,int y,const std::wstring &s){TextOutW(dc,x,y,s.data(),static_cast<int>(s.size()));}
struct Cache {
    std::shared_ptr<const Image> image;
    std::vector<std::uint8_t> bgra;
    void assign(const std::shared_ptr<const Image> &v){
        if(image==v)return;image=v;bgra.clear();if(!v)return;
        bgra.resize(static_cast<std::size_t>(v->width)*v->height*4);
        for(std::size_t i=0;i<bgra.size()/4;++i){bgra[4*i]=v->bgr[3*i];bgra[4*i+1]=v->bgr[3*i+1];bgra[4*i+2]=v->bgr[3*i+2];bgra[4*i+3]=255;}
    }
};
Cache left_cache,right_cache;
void image(HDC dc,RECT box,Cache &cache,const std::shared_ptr<const Image> &im,bool native=false){
    FillRect(dc,&box,reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));if(!im)return;cache.assign(im);
    auto f=fit(im->width,im->height,box.right-box.left,box.bottom-box.top);
    if(native&&im->width<=box.right-box.left&&im->height<=box.bottom-box.top)
        f={(box.right-box.left-im->width)/2,(box.bottom-box.top-im->height)/2,im->width,im->height};
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=im->width;bi.bmiHeader.biHeight=-im->height;
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);
    StretchDIBits(dc,box.left+f.x,box.top+f.y,f.width,f.height,0,0,im->width,im->height,cache.bgra.data(),&bi,DIB_RGB_COLORS,SRCCOPY);
}
LRESULT CALLBACK window_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE: {
        font=CreateFontW(-18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        label(w,L"输入",12,18,40);control(w,L"EDIT",wide(cfg.input).c_str(),Path,56,12,790,30,ES_AUTOHSCROLL);
        control(w,L"BUTTON",L"选择文件",Choose,854,12,106,30);control(w,L"BUTTON",L"开始",Start,970,12,70,30);
        control(w,L"BUTTON",L"停止",Stop,1048,12,70,30);control(w,L"BUTTON",L"重新播放",Replay,1126,12,100,30);
        label(w,L"输出框",12,57,60);control(w,L"EDIT",std::to_wstring(cfg.width).c_str(),Width,72,52,56,28,ES_NUMBER);label(w,L"×",132,57,20);
        control(w,L"EDIT",std::to_wstring(cfg.height).c_str(),Height,150,52,56,28,ES_NUMBER);
        label(w,L"目标fps",220,57,75);control(w,L"EDIT",wide(std::to_string(cfg.fps)).c_str(),Fps,295,52,65,28);
        label(w,L"包/秒",374,57,60);control(w,L"EDIT",wide(std::to_string(cfg.send_rate)).c_str(),Rate,434,52,65,28);
        control(w,L"BUTTON",L"循环输入",Loop,515,52,110,28,BS_AUTOCHECKBOX);SendDlgItemMessageW(w,Loop,BM_SETCHECK,cfg.loop?BST_CHECKED:BST_UNCHECKED,0);
        label(w,L"裸帧尺寸",640,57,85);control(w,L"EDIT",std::to_wstring(cfg.raw_width).c_str(),RawWidth,725,52,65,28,ES_NUMBER);
        control(w,L"EDIT",std::to_wstring(cfg.raw_height).c_str(),RawHeight,796,52,65,28,ES_NUMBER);
        label(w,L"裸帧fps",875,57,80);control(w,L"EDIT",wide(std::to_string(cfg.source_fps)).c_str(),SourceFps,955,52,65,28);
        control(w,L"BUTTON",L"右栏原生1:1",Native,1050,52,170,28,BS_AUTOCHECKBOX);
        SetTimer(w,1,16,nullptr);return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==Choose)choose(w);
        if(LOWORD(wp)==Native){right_native=SendDlgItemMessageW(w,Native,BM_GETCHECK,0,0)==BST_CHECKED;InvalidateRect(w,nullptr,FALSE);}
        if(LOWORD(wp)==Start||LOWORD(wp)==Replay)start(w);
        if(LOWORD(wp)==Stop){stop();for(int id:{Path,Choose,Width,Height,Fps,Rate,Loop,RawWidth,RawHeight,SourceFps})EnableWindow(GetDlgItem(w,id),TRUE);}
        return 0;
    case WM_TIMER:
        if(engine){current=engine->view();probe(current);if(current.finished||!current.error.empty()){if(!current.error.empty())engine->stop();for(int id:{Path,Choose,Width,Height,Fps,Rate,Loop,RawWidth,RawHeight,SourceFps})EnableWindow(GetDlgItem(w,id),TRUE);}}
        InvalidateRect(w,nullptr,FALSE);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;auto dc=BeginPaint(w,&ps);RECT client;GetClientRect(w,&client);
        // Double buffer avoids flashing while the two views update independently.
        auto mem=CreateCompatibleDC(dc);auto bitmap=CreateCompatibleBitmap(dc,std::max(1L,client.right),std::max(1L,client.bottom));auto old=SelectObject(mem,bitmap);
        auto bg=CreateSolidBrush(RGB(22,27,34));FillRect(mem,&client,bg);DeleteObject(bg);SelectObject(mem,font);
        SetBkMode(mem,TRANSPARENT);SetTextColor(mem,RGB(225,231,238));
        std::wostringstream model;model<<L"C板旁路 / 理想通道 · "<<kPacketBytes<<L" B/包（净码流≤"<<kPayloadBytes
          <<L" B）· 每帧≤"<<kFramePackets<<L"包 / "<<kFrameBytes<<L" B · 50包/s按双包预算≤"<<kMaxFps<<L" fps";
        text(mem,12,91,model.str());
        text(mem,12,120,L"相机原片（未处理）");const int half=client.right/2;
        std::wostringstream frame_label;frame_label<<L"接收端（实际解码）";
        if(current.right){frame_label<<L" "<<current.right->width<<L"×"<<current.right->height;if(current.right->id)frame_label<<L" · "<<current.right->coded_bytes<<L" B / "<<current.right->packets<<L"/"<<kFramePackets<<L"包";else frame_label<<L" · 字节关联未知";}
        text(mem,half+8,120,frame_label.str());
        RECT l{12,150,half-8,std::max(200L,client.bottom-150)},r{half+8,150,client.right-12,std::max(200L,client.bottom-150)};
        image(mem,l,left_cache,current.left);image(mem,r,right_cache,current.right,right_native);
        std::wostringstream s;s.precision(1);s<<std::fixed<<L"状态："<<wide(startup_error.empty()?current.status:startup_error)
          <<L"   输入/编码/解出/显示："<<current.input<<L" / "<<current.encoded<<L" / "<<current.decoded<<L" / "<<current.displayed;
        if(current.failed_windows)s<<L"   未达15 fps："<<current.failed_windows<<L"个整秒";
        text(mem,12,client.bottom-133,s.str());s.str(L"");s.clear();
        if(current.right&&!current.right->id)s<<L"当前图像未可靠关联，延迟/画面年龄未知    ";
        else s<<std::fixed<<L"至提交显示延迟 "<<current.latency_ms<<L" ms    画面年龄 "<<current.age_ms<<L" ms    ";
        s<<L"解码 "<<current.decode_fps<<L" fps    显示 "<<current.update_fps<<L" fps    发包 "<<current.packets_sec<<L" /50 包/s";
        text(mem,12,client.bottom-105,s.str());s.str(L"");s.clear();
        s<<L"覆盖 "<<current.overwritten<<L"   超限 "<<current.oversize<<L"   积压丢帧 "<<current.backlog<<L"   通道丢包 "<<current.network_drop<<L"   接收溢出 "<<current.rx_drop<<L"   未关联 "<<current.unknown<<L"   发送排队 "<<current.queued_frames;
        text(mem,12,client.bottom-77,s.str());text(mem,12,client.bottom-49,wide(current.error.empty()?current.output:current.error));
        const auto frame=current.right;
        BitBlt(dc,0,84,client.right,std::max(0L,client.bottom-84),mem,0,84,SRCCOPY);GdiFlush();
        if(engine&&frame)engine->presented(frame->id);
        if(engine&&current.finished){engine->stop();if(close_on_end)PostMessageW(w,WM_CLOSE,0,0);}
        SelectObject(mem,old);DeleteObject(bitmap);DeleteDC(mem);EndPaint(w,&ps);return 0;
    }
    case WM_SIZE:InvalidateRect(w,nullptr,FALSE);return 0;
    case WM_GETMINMAXINFO:{auto p=reinterpret_cast<MINMAXINFO *>(lp);p->ptMinTrackSize={1250,650};return 0;}
    case WM_CLOSE:stop();DestroyWindow(w);return 0;
    case WM_DESTROY:if(font)DeleteObject(font);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}
}
int main(int argc,char **argv){
    cw::TimingResolution timing;
    cw::init_console();auto args=cw::collect_args(argc,argv);
    try {
        for(std::size_t i=0;i<args.size();++i){const auto &a=args[i];
            auto value=[&]()->std::string{if(++i>=args.size())throw std::runtime_error("参数缺少值："+a);return args[i];};
            if(a=="--help"){cw::out("cw_live: 300 B/packet, <=2 packets/frame, <=592 H264 bytes, fps<=25\n  --input <video|bgr> --size WxH --fps 20 --send-rate 50\n  --raw-size WxH --source-fps 20 --loop --duration seconds --out directory\n  --autostart --close-on-end (GUI playback/validation)\n  --headless (no rendering; latency is consumption only)\n  --self-test\n  Test only: --test-pre-ms N --test-encode-ms N --test-post-ms N --drop-packet N --test-drop-all --test-oversize-frame N\n");return 0;}
            if(a=="--self-test")return protocol_self_test();
            if(a=="--input")cfg.input=value();else if(a=="--ffmpeg")cfg.ffmpeg=value();
            else if(a=="--out")cfg.output=value();else if(a=="--fps")cfg.fps=std::stod(value());
            else if(a=="--source-fps")cfg.source_fps=std::stod(value());else if(a=="--send-rate")cfg.send_rate=std::stod(value());
            else if(a=="--duration")cfg.duration=std::stod(value());else if(a=="--headless")cfg.headless=true;else if(a=="--loop")cfg.loop=true;
            else if(a=="--autostart")autostart=true;else if(a=="--close-on-end")close_on_end=true;
            else if(a=="--size"||a=="--raw-size"){auto v=value();auto x=v.find('x');if(x==std::string::npos)throw std::runtime_error("尺寸格式应为WxH");int w=std::stoi(v.substr(0,x)),h=std::stoi(v.substr(x+1));if(a=="--size"){cfg.width=w;cfg.height=h;}else{cfg.raw_width=w;cfg.raw_height=h;}}
            else if(a=="--test-pre-ms")cfg.test_pre_ms=std::stoi(value());else if(a=="--test-encode-ms")cfg.test_encode_ms=std::stoi(value());
            else if(a=="--test-post-ms")cfg.test_post_ms=std::stoi(value());else if(a=="--drop-packet")cfg.drop_packet=std::stoi(value());
            else if(a=="--test-drop-all")cfg.test_drop_all=true;else if(a=="--test-oversize-frame")cfg.test_oversize_frame=std::stoi(value());
            else throw std::runtime_error("未知参数："+a);
        }
        std::string error;if(!validate(cfg,error))throw std::runtime_error(error);
        if(cfg.headless){
            engine=std::make_unique<Engine>(cfg);if(!engine->start(error))throw std::runtime_error(error);
            do {current=engine->view();if(current.right)engine->presented(current.right->id);probe(current);std::this_thread::sleep_for(std::chrono::milliseconds(5));}while(current.running&&!current.finished);
            engine->stop();current=engine->view();cw::out("summary: %s/summary.txt\n",current.output.c_str());if(!current.error.empty())throw std::runtime_error(current.error);return 0;
        }
        SetProcessDPIAware();WNDCLASSW wc{};wc.lpfnWndProc=window_proc;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"RM26CompressionLive";
        if(!RegisterClassW(&wc))throw std::runtime_error("RegisterClassW failed");
        auto w=CreateWindowExW(0,wc.lpszClassName,L"压片战争 · 实时链路仿真",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1320,900,nullptr,nullptr,wc.hInstance,nullptr);
        if(!w)throw std::runtime_error("CreateWindowExW failed");ShowWindow(w,SW_SHOW);UpdateWindow(w);
        std::uint64_t size=0;if(!cw::file_size(cfg.input,size))choose(w);
        current.status="就绪";if(autostart)PostMessageW(w,WM_COMMAND,Start,0);
        MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
        const bool failed=!startup_error.empty()||!current.error.empty();engine.reset();return failed?2:0;
    }catch(const std::exception &e){cw::eout("[失败] %s\n",e.what());return 2;}
}
