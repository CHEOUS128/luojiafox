#include "app/application.h"
#include "logging/logging.h"
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
    const RoboGo::app::LaunchOptions options = RoboGo::app::ParseLaunchOptions(args);
    RoboGo::logging::initialize(platform_backend->application_name(), options.invert_udp_video, options.enable_network_capture);
    const int result = RoboGo::app::RunApplication(*platform_backend, options);
    RoboGo::logging::shutdown();
    return result;
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
