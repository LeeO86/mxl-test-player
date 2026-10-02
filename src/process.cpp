#include "process.hpp"

#include "util.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>

namespace mtp {

ProcessResult run_process(const std::vector<std::string>& args, const std::function<void(std::string_view)>& on_stdout) {
    ProcessResult result;
    if (args.empty()) return result;
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        result.output = "pipe failed";
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        result.output = "fork failed";
        return result;
    }
    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);
    std::string stdout_buf;
    std::array<char, 4096> buf{};
    bool out_open = true, err_open = true;
    while (out_open || err_open) {
        pollfd fds[2] = {};
        int nfd = 0;
        int oi = -1, ei = -1;
        if (out_open) {
            oi = nfd;
            fds[nfd].fd = out_pipe[0];
            fds[nfd].events = POLLIN;
            ++nfd;
        }
        if (err_open) {
            ei = nfd;
            fds[nfd].fd = err_pipe[0];
            fds[nfd].events = POLLIN;
            ++nfd;
        }
        const int pr = poll(fds, static_cast<nfds_t>(nfd), 1000);
        if (pr < 0) break;
        auto drain = [&](int idx, int fd, bool is_out) {
            if (idx < 0) return;
            if (fds[idx].revents & (POLLIN | POLLHUP)) {
                for (;;) {
                    const ssize_t n = read(fd, buf.data(), buf.size());
                    if (n > 0) {
                        if (is_out) {
                            stdout_buf.append(buf.data(), buf.data() + n);
                            if (on_stdout) {
                                std::size_t pos = 0;
                                while (true) {
                                    auto nl = stdout_buf.find('\n', pos);
                                    if (nl == std::string::npos) {
                                        stdout_buf.erase(0, pos);
                                        break;
                                    }
                                    on_stdout(std::string_view(stdout_buf).substr(pos, nl - pos));
                                    pos = nl + 1;
                                }
                            }
                        } else {
                            result.output.append(buf.data(), buf.data() + n);
                            if (result.output.size() > 200000) result.output.erase(0, result.output.size() - 150000);
                        }
                    } else if (n == 0) {
                        if (is_out) out_open = false;
                        else err_open = false;
                        break;
                    } else {
                        break;
                    }
                }
            }
        };
        drain(oi, out_pipe[0], true);
        drain(ei, err_pipe[0], false);
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) result.code = WEXITSTATUS(status);
    else result.code = -1;
    if (!on_stdout) result.output = stdout_buf + result.output;
    return result;
}

}  // namespace mtp
