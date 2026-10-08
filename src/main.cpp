#include "app.hpp"
#include "config.hpp"
#include "util.hpp"

#include <csignal>
#include <cstring>
#include <sys/resource.h>
#include <unistd.h>

namespace {
mtp::App* g_app = nullptr;

void on_signal(int sig) {
    if (g_app) g_app->request_stop(sig == SIGTERM ? 143 : 0);
}
}  // namespace

int main(int argc, char** argv) {
    std::string config_path;  // without --config: PLAYER_CONFIG, else $CONFIG_DIR/player.json
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::fprintf(stdout, "mxl-test-player [--config file]\n");
            return 0;
        }
    }
    std::signal(SIGPIPE, SIG_IGN);
    // Every MXL flow keeps one descriptor per grain (50 for 1 s at 50p). With
    // Docker's default soft limit of 1024, 16 outputs ran out: flows failed to
    // open and the font load threw.
    rlimit files{};
    if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < files.rlim_max) {
        files.rlim_cur = files.rlim_max;
        setrlimit(RLIMIT_NOFILE, &files);
    }
    mtp::Config cfg;
    try {
        cfg = mtp::load_config(config_path, environ);
    } catch (const mtp::ConfigError& ex) {
        mtp::log_error(ex.what());
        return 78;
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
    } catch (const mtp::ConfigError& ex) {
        mtp::log_error(ex.what());
        return 78;
    } catch (const std::exception& ex) {
        mtp::log_error(ex.what());
        return 75;
    }
}
