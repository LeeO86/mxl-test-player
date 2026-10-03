#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mtp {

struct ProcessResult {
    int code = -1;
    std::string output;
};

// Runs argv without a shell. If `on_stdout` is set, stdout is streamed to it
// instead of being captured. stderr is always captured into `output`.
ProcessResult run_process(const std::vector<std::string>& args, const std::function<void(std::string_view)>& on_stdout = {});

// SIGTERM then SIGKILL every child started by run_process. Does not wait for them to exit.
void stop_children();

}  // namespace mtp
