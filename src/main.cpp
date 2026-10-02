#include "app.hpp"
#include "config.hpp"
#include "util.hpp"

#include <csignal>
#include <cstring>
#include <unistd.h>

namespace {
mtp::App* g_app = nullptr;

void on_signal(int sig) {
    if (g_app) g_app->request_stop(sig == SIGTERM ? 143 : 0);
}
}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "/config/player.json";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::fprintf(stdout, "mxl-test-player [--config file]\n");
            return 0;
        }
    }
    std::signal(SIGPIPE, SIG_IGN);
    mtp::Config cfg;
    try {
        cfg = mtp::load_config(config_path, environ);
    } catch (const std::exception& ex) {
        mtp::log_error(ex.what());
        return 78;
    }
    try {
        mtp::App app(std::move(cfg));
        g_app = &app;
        std::signal(SIGTERM, on_signal);
        std::signal(SIGINT, on_signal);
        const int code = app.run();
        g_app = nullptr;
        return code;
    } catch (const std::exception& ex) {
        mtp::log_error(ex.what());
        return 75;
    }
}
