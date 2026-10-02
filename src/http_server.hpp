#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mtp {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::vector<std::uint8_t> body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::map<std::string, std::string> headers;
    std::vector<std::uint8_t> body;
    void set_text(const std::string& s, const std::string& type = "application/json; charset=utf-8");
};

class HttpServer {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;
    HttpServer() = default;
    ~HttpServer();
    void start(int port, Handler handler);
    void stop();
    void broadcast_text(const std::string& text);

private:
    void accept_loop();
    void serve(int fd);
    void add_ws(int fd);
    void drop_ws(int fd);

    int port_ = 0;
    int listen_fd_ = -1;
    Handler handler_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex ws_mu_;
    std::vector<int> ws_;
};

std::string query_get(const std::string& query, const std::string& key);

}  // namespace mtp
