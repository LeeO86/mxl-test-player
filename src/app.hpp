#pragma once

#include "config.hpp"

namespace mtp {

class App {
public:
    explicit App(Config cfg);
    ~App();
    int run();
    void request_stop(int code);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace mtp
