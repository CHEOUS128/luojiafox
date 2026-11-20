#include "app/application.h"
#include "app/ul_preview.h"
#include "app/video_stability_test.h"
#include "logging/logging.h"
#include "luojiamaster_brand.h"
#include "platform/launcher_restart.h"
#include "platform/platform_backend.h"

namespace {

#ifdef _WIN32
std::vector<std::string> CollectCommandLineArgs() {
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR *argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv_w == nullptr) {
        return args;
    }

    for (int i = 1; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, arg.data(), size, nullptr, nullptr);
        arg.resize(static_cast<size_t>(size - 1));
        args.push_back(std::move(arg));
    }
    LocalFree(argv_w);
    return args;
}
#else
std::vector<std::string> CollectCommandLineArgs(const int argc, char *argv[]) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(std::max(argc - 1, 0)));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}
#endif

int RunMain(const std::vector<std::string> &args) {
    auto platform_backend = RoboGo::platform::CreatePlatformBackend();
    if (std::find(args.begin(), args.end(), "--offline-video-test") != args.end()) {
        // Branch before normal options, services, sockets, MQTT and keyboard/mouse state.
        RoboGo::logging::initialize(platform_backend->application_name(), false);
        int result = 1;
        try {
            result = RoboGo::app::RunVideoStabilityTest(*platform_backend, args);
        } catch (const std::exception &error) {
            SPDLOG_ERROR("Offline video test failed: {}", error.what());
            platform_backend->Shutdown();
        }
        RoboGo::logging::shutdown();
        return result;
    }
    using namespace RoboGo;
    const auto has = [&](std::string_view opt) {
        return std::find(args.begin(), args.end(), opt) != args.end();
    };
    const bool check_uc = has("--window-check-uc");
    const bool check_ul = has("--window-check-ul");
    if (check_uc && check_ul) return 2;

    const bool diagnostic = has("--ul-preview") || check_uc || check_ul;
    const bool local_trial = has("--local-trial");
    const bool welcome = args.empty() || has("--welcome");
    if (welcome && local_trial) return 2;
    std::optional<app::Competition> requested_event;
    for (const auto &arg : args) {
        if (!arg.starts_with("--competition=")) continue;
        if (requested_event || welcome) return 2;
        if (arg == "--competition=uc2026") requested_event = app::Competition::Uc2026;
        else if (arg == "--competition=ul2027") requested_event = app::Competition::Ul2027;
        else if (arg == "--competition=uc2027") requested_event = app::Competition::Uc2027;
        else return 2;
    }
    if (diagnostic) {
        logging::initialize(platform_backend->application_name(), false);
        struct LogShutdown { ~LogShutdown() { logging::shutdown(); } } logs;
        const auto entry = check_uc ? app::EntryScreen::CheckUc :
                           check_ul ? app::EntryScreen::CheckUl : app::EntryScreen::Welcome;
        try {
            return app::RunUlPreview(*platform_backend, entry);
        } catch (const std::exception &error) {
            SPDLOG_ERROR("离线检修窗口启动失败 {}", error.what());
            return 1;
        }
    }
    app::LaunchOptions options;
    try {
        options = app::ParseLaunchOptions(args);
    } catch (const std::exception &e) {
        logging::initialize(platform_backend->application_name(), false);
        SPDLOG_ERROR("启动参数无效 {}", e.what());
        logging::shutdown();
        return 2;
    }
    logging::initialize(platform_backend->application_name(), options.invert_udp_video);
    struct LogShutdown { ~LogShutdown() { logging::shutdown(); } } logs;
    try {
        app::SessionChoice choice{requested_event.value_or(app::Competition::Ul2027), local_trial};
        if (welcome) {
            auto selected = app::RunCompetitionLauncher(*platform_backend);
            if (!selected) return 0;
            choice = *selected;
            platform_backend = platform::CreatePlatformBackend();
        }
        options.competition = choice.competition;
        options.local_trial = choice.local_trial;
        options.uc_classic = choice.competition != app::Competition::Ul2027;
        SPDLOG_INFO("进入赛事 {} / {}", app::CompetitionName(choice.competition),
                    choice.local_trial ? "本地测试" : "正式登录");
        const int result = app::RunApplication(*platform_backend, options);
        if (result != app::kReturnToLauncher) return result;
        // RunApplication has already stopped every service and released the window.
        if (platform_util::RestartAtCompetitionLauncher()) return 0;
        throw std::runtime_error("无法重新打开赛事入口");
    } catch (const std::exception &error) {
        SPDLOG_ERROR("客户端运行失败 {}", error.what());
#ifdef _WIN32
        MessageBoxW(nullptr, L"客户端窗口未能继续运行，请重开程序并查看本次日志。", LUOJIAMASTER_PRODUCT_W, MB_OK | MB_ICONERROR);
#endif
        return 1;
    }
}

}// namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE h_instance, HINSTANCE previous_instance, PSTR command_line, int show_command) {
    (void) h_instance;
    (void) previous_instance;
    (void) command_line;
    (void) show_command;
    return RunMain(CollectCommandLineArgs());
}
#else
int main(const int argc, char *argv[]) {
    return RunMain(CollectCommandLineArgs(argc, argv));
}
#endif
