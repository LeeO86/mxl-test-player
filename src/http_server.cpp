#include "http_server.hpp"

#include "util.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace mtp {
namespace {

std::uint32_t rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1(const std::uint8_t* msg, std::size_t len, std::uint8_t out[20]) {
    std::uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(len) * 8ull;
    const std::size_t padded = ((len + 9 + 63) / 64) * 64;
    std::vector<std::uint8_t> buf(padded, 0);
    std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    for (int i = 0; i < 8; ++i) buf[padded - 1 - i] = static_cast<std::uint8_t>(bit_len >> (8 * i));
    for (std::size_t off = 0; off < padded; off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(buf[off + 4 * i]) << 24) | (std::uint32_t(buf[off + 4 * i + 1]) << 16) |
                   (std::uint32_t(buf[off + 4 * i + 2]) << 8) | buf[off + 4 * i + 3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    const std::uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = hs[i] >> 24;
        out[i * 4 + 1] = hs[i] >> 16;
        out[i * 4 + 2] = hs[i] >> 8;
        out[i * 4 + 3] = hs[i];
    }
}

std::string b64(const std::uint8_t* data, std::size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (std::size_t i = 0; i < n; i += 3) {
        const unsigned v = (data[i] << 16) | ((i + 1 < n ? data[i + 1] : 0) << 8) | (i + 2 < n ? data[i + 2] : 0);
        o.push_back(t[(v >> 18) & 63]);
        o.push_back(t[(v >> 12) & 63]);
        o.push_back(i + 1 < n ? t[(v >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? t[v & 63] : '=');
    }
    return o;
}

std::string ws_accept(const std::string& key) {
    const std::string src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::uint8_t dig[20];
    sha1(reinterpret_cast<const std::uint8_t*>(src.data()), src.size(), dig);
    return b64(dig, 20);
}

std::string header_get(const std::map<std::string, std::string>& h, const std::string& key) {
    for (const auto& kv : h)
        if (strcasecmp(kv.first.c_str(), key.c_str()) == 0) return kv.second;
    return {};
}

void write_all(int fd, const char* p, std::size_t n) {
    while (n) {
        const ssize_t w = ::send(fd, p, n, 0);
        if (w <= 0) return;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
}

void send_ws(int fd, const std::string& text) {
    std::vector<std::uint8_t> frame;
    frame.push_back(0x81);
    if (text.size() < 126) frame.push_back(static_cast<std::uint8_t>(text.size()));
    else if (text.size() <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<std::uint8_t>((text.size() >> 8) & 0xFF));
        frame.push_back(static_cast<std::uint8_t>(text.size() & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<std::uint8_t>((text.size() >> (8 * i)) & 0xFF));
    }
    frame.insert(frame.end(), text.begin(), text.end());
    write_all(fd, reinterpret_cast<char*>(frame.data()), frame.size());
}

}  // namespace

void HttpResponse::set_text(const std::string& s, const std::string& type) {
    content_type = type;
    body.assign(s.begin(), s.end());
}

std::string query_get(const std::string& query, const std::string& key) {
    std::size_t i = 0;
    while (i < query.size()) {
        auto amp = query.find('&', i);
        if (amp == std::string::npos) amp = query.size();
        auto eq = query.find('=', i);
        if (eq != std::string::npos && eq < amp) {
            if (query.substr(i, eq - i) == key) return query.substr(eq + 1, amp - eq - 1);
        }
        i = amp + 1;
    }
    return {};
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start(int port, Handler handler) {
    port_ = port;
    handler_ = std::move(handler);
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw Error("socket failed");
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        throw Error("bind failed on port " + std::to_string(port));
    }
    if (listen(listen_fd_, 64) != 0) throw Error("listen failed");
    stop_ = false;
    thread_ = std::thread([this] { accept_loop(); });
}

void HttpServer::stop() {
    stop_ = true;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(ws_mu_);
    for (int fd : ws_) close(fd);
    ws_.clear();
}

void HttpServer::accept_loop() {
    while (!stop_) {
        const int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (stop_) break;
            continue;
        }
        std::thread(&HttpServer::serve, this, fd).detach();
    }
}

void HttpServer::add_ws(int fd) {
    std::lock_guard lock(ws_mu_);
    ws_.push_back(fd);
}

void HttpServer::drop_ws(int fd) {
    std::lock_guard lock(ws_mu_);
    ws_.erase(std::remove(ws_.begin(), ws_.end(), fd), ws_.end());
}

void HttpServer::broadcast_text(const std::string& text) {
    std::vector<int> dead;
    std::lock_guard lock(ws_mu_);
    for (int fd : ws_) {
        send_ws(fd, text);
    }
    (void)dead;
}

void HttpServer::serve(int fd) {
    std::string header;
    char buf[4096];
    while (header.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            close(fd);
            return;
        }
        header.append(buf, buf + n);
        if (header.size() > 1024 * 1024) {
            close(fd);
            return;
        }
    }
    const auto split = header.find("\r\n\r\n");
    const std::string head = header.substr(0, split);
    std::string rest = header.substr(split + 4);
    std::istringstream hs(head);
    std::string reqline;
    std::getline(hs, reqline);
    if (!reqline.empty() && reqline.back() == '\r') reqline.pop_back();
    HttpRequest req;
    {
        auto sp1 = reqline.find(' ');
        auto sp2 = reqline.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            close(fd);
            return;
        }
        req.method = reqline.substr(0, sp1);
        auto target = reqline.substr(sp1 + 1, sp2 - sp1 - 1);
        auto q = target.find('?');
        if (q == std::string::npos) req.path = target;
        else {
            req.path = target.substr(0, q);
            req.query = target.substr(q + 1);
        }
    }
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto c = line.find(':');
        if (c == std::string::npos) continue;
        auto key = trim(line.substr(0, c));
        auto val = trim(line.substr(c + 1));
        req.headers[key] = val;
    }
    const auto upgrade = header_get(req.headers, "Upgrade");
    if (req.method == "GET" && strcasecmp(upgrade.c_str(), "websocket") == 0 && req.path == "/api/v1/events") {
        const auto key = header_get(req.headers, "Sec-WebSocket-Key");
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                           ws_accept(key) + "\r\n\r\n";
        write_all(fd, resp.data(), resp.size());
        add_ws(fd);
        // Drain client frames until the socket closes. Payloads are ignored; the UI is push-only.
        std::vector<char> tmp(4096);
        while (!stop_) {
            const ssize_t n = recv(fd, tmp.data(), tmp.size(), 0);
            if (n <= 0) break;
        }
        drop_ws(fd);
        close(fd);
        return;
    }
    std::size_t content_length = 0;
    if (auto cl = header_get(req.headers, "Content-Length"); !cl.empty()) content_length = static_cast<std::size_t>(std::strtoull(cl.c_str(), nullptr, 10));
    if (content_length > (64ull << 20)) {
        const char* msg = "HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        write_all(fd, msg, std::strlen(msg));
        close(fd);
        return;
    }
    req.body.assign(rest.begin(), rest.end());
    while (req.body.size() < content_length) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.body.insert(req.body.end(), buf, buf + n);
    }
    if (req.body.size() > content_length) req.body.resize(content_length);
    HttpResponse res;
    try {
        if (handler_) handler_(req, res);
    } catch (const std::exception& ex) {
        res.status = 500;
        res.set_text(std::string("{\"error\":\"") + ex.what() + "\"}");
    }
    std::string status_text = "OK";
    if (res.status == 201) status_text = "Created";
    else if (res.status == 204) status_text = "No Content";
    else if (res.status == 400) status_text = "Bad Request";
    else if (res.status == 404) status_text = "Not Found";
    else if (res.status == 409) status_text = "Conflict";
    else if (res.status == 500) status_text = "Error";
    std::ostringstream os;
    os << "HTTP/1.1 " << res.status << " " << status_text << "\r\n";
    os << "Content-Type: " << res.content_type << "\r\n";
    os << "Content-Length: " << res.body.size() << "\r\n";
    os << "Access-Control-Allow-Origin: *\r\n";
    os << "Access-Control-Allow-Methods: GET,POST,PUT,PATCH,DELETE,OPTIONS\r\n";
    os << "Access-Control-Allow-Headers: Content-Type\r\n";
    os << "Connection: close\r\n";
    for (const auto& kv : res.headers) os << kv.first << ": " << kv.second << "\r\n";
    os << "\r\n";
    const auto hdr = os.str();
    write_all(fd, hdr.data(), hdr.size());
    if (!res.body.empty()) write_all(fd, reinterpret_cast<char*>(res.body.data()), res.body.size());
    close(fd);
}

}  // namespace mtp
