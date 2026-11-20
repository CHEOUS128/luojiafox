#include "app/ul_preview.h"
#include "panel/brand_mark.h"
#include "app/ul_preview_ui.h"
#include "app/latest_video_frame.h"
#include "app/local_session_capture.h"
#include "platform/platform_backend.h"
#include "resource/resources.h"
#include "video/custom_byte_block_video_decoder.h"
#include "video/rgba_frame_converter.h"
#include "video/video_decoder.h"
#include "video/video_shards_manager.h"
#include "embedded_ul_capture.h"
#include "embedded_ul_map.h"
#include <algorithm>
#include <condition_variable>
#include <span>

namespace RoboGo::app {
namespace {
using Clock = std::chrono::steady_clock;
using Bytes = std::span<const std::uint8_t>;
using Microseconds = std::chrono::microseconds;
using local_capture::Capture;
using local_capture::ReadSample;

class Playback {
public:
    LatestVideoFrameMailbox<VideoFrame> latest[2];
    std::array<std::atomic<std::uint64_t>,2> loops{};
    explicit Playback(const Capture &sample) : sample_(sample) {
        try {
            for (int i=0; i<2; ++i) threads_[i]=std::thread([this,i] {
                try { Run(i); }
                catch (const std::exception &e) { Fail(e.what()); }
                catch (...) { Fail("解码线程遇到未知错误"); }
            });
        } catch (...) { Stop(); throw; }
    }
    ~Playback() { Stop(); }
    std::string Error() { std::lock_guard lock(mutex_); return error_; }
private:
    void Stop() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        cv_.notify_all();
        for (auto &t : threads_) if (t.joinable()) t.join();
    }
    void Fail(const std::string &why) {
        { std::lock_guard lock(mutex_); if(error_.empty()) error_=why; stopping_=true; }
        cv_.notify_all();
    }
    template<typename Consume,typename Reset>
    void Schedule(int i, Consume consume, Reset reset) {
        auto epoch = start_;
        for (;;) {
            for (const auto &p : sample_.lanes[i]) {
                const auto due = epoch + Microseconds(p.at);
                { std::unique_lock lock(mutex_); if (cv_.wait_until(lock,due,[&]{ return stopping_; })) return; }
                consume(p.data);
            }
            ++loops[i];
            // Never drain a backlog of missed *loops* following sleep or minimization.
            epoch = std::max(epoch + sample_.period, Clock::now());
            reset();
        }
    }
    void Run(int i) {
        if (i == 0) {
            video::RgbaFrameConverter converter;
            video::VideoDecoder decoder([&](const AVFrame *f){
                if (auto image=converter.Convert(f)) latest[0].Push(std::move(*image));
            });
            bool ready=false;
            video::VideoShardsManager shards;
            shards.set_frame_callback([&](const std::vector<std::uint8_t> &data) {
                if (!ready) {
                    const int nal=video::VideoDecoder::get_nal_type(data);
                    ready = nal==32 || nal==33 || nal==34 || nal==19 || nal==20 || nal==21;
                }
                if (ready) decoder.decode_frame(data);
            });
            shards.set_reset_callback([&]{decoder.reset(); ready=false;});
            Schedule(0,[&](Bytes data){shards.on_receive_shard(data.data(),data.size());shards.handle_once();},
                     [&]{shards.reset_stream();decoder.reset();ready=false;});
        } else {
            video::CustomByteBlockVideoDecoder decoder;
            Schedule(1,[&](Bytes data) {
                for(auto &f : decoder.push_packet_bytes(data)) latest[1].Push({std::move(f.rgba),f.width,f.height});
            },[&]{decoder.reset_stream();});
        }
    }
    const Capture &sample_;
    Clock::time_point start_ = Clock::now();
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::string error_;
    std::array<std::thread,2> threads_;
};
} // namespace

std::optional<SessionChoice> RunCompetitionLauncher(platform::PlatformBackend &backend) {
    using namespace ul_drawing;
    backend.EnableOfflineTestMode(true);
    backend.SetDesktopWindowKind(platform::DesktopWindowKind::EventPicker);
    if (!backend.Initialize()) throw std::runtime_error("赛事选择窗口初始化失败");
    struct Cleanup {
        platform::PlatformBackend &backend;
        bool active = true;
        ~Cleanup() { if (active) backend.Shutdown(); }
    } cleanup{backend};
    std::optional<Competition> competition;
    std::optional<SessionChoice> choice;
    while (!choice && backend.ProcessEvents()) {
        if (!backend.BeginFrame()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        const auto *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.073f,.105f,.118f,1));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.16f,.22f,.24f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.21f,.31f,.32f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.17f,.26f,.27f,1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16,16));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12,9));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f);
        ImGui::Begin("选择赛事###competition-home", nullptr, ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float width = std::min(560.f, avail);
        const float indent = std::max(1.f, (avail-width)*.5f);
        ImGui::Indent(indent);
        Gap(std::clamp(vp->WorkSize.y*.09f, 4.f, 78.f));
        luojiamaster::brand::Masthead(!competition);
        Gap(competition ? 6.f : 12.f);
        Title(competition ? CompetitionName(*competition) : "选择赛事", std::clamp(width*.05f,20.f,30.f));
        Gap(14);
        if (!competition) {
            for (Competition event : {Competition::Uc2026, Competition::Ul2027, Competition::Uc2027}) {
                if (Button(CompetitionName(event), event == Competition::Ul2027, width)) competition = event;
                Gap(4);
            }
        } else {
            if (Button("正式登录", true, width)) choice = SessionChoice{*competition, false};
            ImGui::TextDisabled("连接官方选手端");
            Gap(9);
            if (Button("本地测试", false, width)) choice = SessionChoice{*competition, true};
            ImGui::TextDisabled("本车和队友均播放录像，可在比赛界面测试");
            Gap(13);
            if (Button("返回选择赛事")) competition.reset();
        }
        ImGui::Unindent(indent);
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(4);
        ImGui::Render();
        (void)backend.RenderFrame(ImGui::GetDrawData());
    }
    const auto timeouts = backend.GetRenderDiagnostics().wait_timeouts;
    backend.Shutdown();
    cleanup.active = false;
    if (choice) {
        if (backend.GetRenderDiagnostics().wait_timeouts != timeouts)
            throw std::runtime_error("显卡尚未完成回收，请重启客户端");
#ifdef _WIN32
        // Deliberate destruction of the launcher HWND posts WM_QUIT to this thread.
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
#endif
    }
    return choice;
}

int RunUlPreview(platform::PlatformBackend &backend,EntryScreen entry) {
    struct Shutdown { platform::PlatformBackend &b; bool active=true; ~Shutdown(){if(active)b.Shutdown();} } guard{backend};
    backend.EnableOfflineTestMode(true);
    if (!backend.Initialize()) throw std::runtime_error("离线体验窗口初始化失败");
    const auto map=resource::LoadEmbeddedImage({embedded_ul_map_data,embedded_ul_map_data_len},"UL 3V3 map");
    std::string status;
    std::optional<Capture> sample;
    // Read the fixture only after an explicit offline playback request.
    if(!map) status+=" 地图资源读取失败";
    if(map) (void)backend.UploadTexture(TextureSlot::MiniMap,*map);
    ul_preview::UiState ui{};
    if(entry!=EntryScreen::Welcome){ui.OpenChecks(entry==EntryScreen::CheckUl); ui.dedicated_check=true;}
    std::optional<resource::DecodedImage> classic_map;
    bool classic_map_read=false;
    std::unique_ptr<Playback> play;
    std::array<std::optional<VideoFrame>,2> pending{};
    std::array<std::uint64_t,2> submitted{};
    std::array<bool,2> current{};
    std::uint64_t logged_loop=0;

    const TextureSlot slots[2]={TextureSlot::UdpVideo,TextureSlot::CustomVideo};
    while(backend.ProcessEvents()) {
        if(!backend.BeginFrame()){std::this_thread::sleep_for(std::chrono::milliseconds(8));continue;}
        std::array<bool,2> uploaded{};
        std::array<TextureView,2> views{};
        if(play) {
            const std::string problem=play->Error();
            if(!problem.empty()) {status="回放失败："+problem;play.reset();pending={};current={};}
        }
        if(play) for(int i=0;i<2;++i){
            if(auto next=play->latest[i].Take()) pending[i]=std::move(next);
            if(pending[i] && backend.UploadTexture(slots[i],*pending[i])) {
                pending[i].reset(); uploaded[i]=true;
            }
            if(current[i] || uploaded[i]) views[i]=backend.GetTextureView(slots[i]);
        }
        const bool classic_check=ui.page==ul_preview::Page::WindowChecks && !ui.checks.ul;
        if(classic_check && !classic_map_read) {
            classic_map_read=true; classic_map=resource::LoadEmbeddedMapImage();
            if(!classic_map) status="基础版地图资源读取失败";
        }
        if(classic_check && classic_map && !backend.GetTextureView(TextureSlot::UlMap).IsReady())
            (void)backend.UploadTexture(TextureSlot::UlMap,*classic_map);
        const auto action=ul_preview::Draw(ui,views[0],views[1],backend.GetTextureView(classic_check?TextureSlot::UlMap:TextureSlot::MiniMap),
                                             submitted[0],submitted[1],status);
        ImGui::Render();
        if(backend.RenderFrame(ImGui::GetDrawData())){
            for(int i=0;i<2;++i) if(uploaded[i] && views[i].IsReady()){current[i]=true;++submitted[i];}
        }
        if(play) {
            const auto loop=std::min(play->loops[0].load(),play->loops[1].load());
            if(loop>logged_loop) {
                logged_loop=loop;
                SPDLOG_INFO("UL offline loop={} submitted_official={} submitted_camera={}",loop,submitted[0],submitted[1]);
                if(loop>=2 && (!submitted[0] || !submitted[1]) && status.empty())
                    status="有一路尚未出画，可重播样本。";
                else if(submitted[0] && submitted[1] && status=="有一路尚未出画，可重播样本。") status.clear();
            }
        }
        if(action==ul_preview::Action::None) continue;
        play.reset(); pending={}; submitted={}; current={}; logged_loop=0;
        if(action==ul_preview::Action::Quit) break;
        if(action==ul_preview::Action::Start || action==ul_preview::Action::Restart) {
            if(!sample) try { sample=ReadSample({embedded_ul_capture_data,embedded_ul_capture_data_len}); }
            catch(const std::exception &e) { status=e.what(); }
            if(sample) try {
                status=(classic_check?classic_map.has_value():map.has_value())?"":"地图资源读取失败";
                play=std::make_unique<Playback>(*sample);
                SPDLOG_INFO("UL offline playback {}: role={} side={} official shards={} camera packets={} loop_us={}",
                            action==ul_preview::Action::Start?"started":"restarted",
                            ui.heavy?"heavy":"infantry",ui.blue?"blue":"red",
                            sample->lanes[0].size(),sample->lanes[1].size(),sample->period.count());
            } catch(const std::exception &e){status=std::string("无法启动回放：")+e.what();}
        } else if(action==ul_preview::Action::Back || action==ul_preview::Action::Stop){
            status.clear();
            if(action==ul_preview::Action::Back) SPDLOG_INFO("Offline preview returned to selection");
        }
    }
    play.reset();
    backend.Shutdown();guard.active=false;
    return 0;
}
} // namespace RoboGo::app
