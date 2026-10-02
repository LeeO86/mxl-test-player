#include "nmos.hpp"

#include "process.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <sstream>

namespace mtp {
namespace {

std::string http_exchange(const std::string& host, int port, const std::string& method, const std::string& path, const std::string& body) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return {};
    const int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return {};
    }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return {};
    }
    freeaddrinfo(res);
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\nHost: " << host << "\r\nContent-Type: application/json\r\nContent-Length: " << body.size()
        << "\r\nConnection: close\r\n\r\n"
        << body;
    const auto s = req.str();
    ::send(fd, s.data(), s.size(), 0);
    std::string out;
    char buf[2048];
    while (true) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, buf + n);
    }
    close(fd);
    return out;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        o.push_back(c);
    }
    return o;
}

}  // namespace

std::string NmosNode::version() const {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000;
    return std::to_string(sec) + ":" + std::to_string(ns);
}

void NmosNode::update(NmosModel model) {
    std::lock_guard lock(mu_);
    model_ = std::move(model);
}

void NmosNode::start(const std::string& registry_host, int registry_port) {
    registry_host_ = registry_host;
    registry_port_ = registry_port;
    stop_ = false;
    if (!registry_host.empty()) reg_ = std::thread([this, registry_host, registry_port] { registry_loop(registry_host, registry_port); });
}

void NmosNode::stop() {
    stop_ = true;
    if (reg_.joinable()) reg_.join();
}

bool NmosNode::master_enabled(const std::string& sender_id) const {
    std::lock_guard lock(mu_);
    for (const auto& s : model_.senders)
        if (s.id == sender_id) return s.master_enable;
    return true;
}

void NmosNode::registry_loop(std::string host, int port) {
    while (!stop_) {
        NmosModel m;
        {
            std::lock_guard lock(mu_);
            m = model_;
        }
        if (!m.node_id.empty()) {
            const auto v = version();
            auto post = [&](const std::string& type, const std::string& data) {
                http_exchange(host, port, "POST", "/x-nmos/registration/v1.3/resource",
                              std::string("{\"type\":\"") + type + "\",\"data\":" + data + "}");
            };
            const std::string href = "http://" + m.host + ":" + std::to_string(m.api_port);
            post("node", "{\"id\":\"" + m.node_id + "\",\"version\":\"" + v + "\",\"label\":\"" + json_escape(m.label) +
                             "\",\"href\":\"" + href + "/x-nmos/node/v1.3/self\",\"hostname\":\"" + json_escape(m.host) +
                             "\",\"api\":{\"versions\":[\"v1.3\"],\"endpoints\":[{\"host\":\"" + json_escape(m.host) + "\",\"port\":" +
                             std::to_string(m.api_port) + ",\"protocol\":\"http\"}]},\"caps\":{},\"services\":[],\"clocks\":[{\"name\":\"clk0\",\"ref_type\":\"internal\"}]}");
            post("device", "{\"id\":\"" + m.device_id + "\",\"version\":\"" + v + "\",\"label\":\"" + json_escape(m.label) +
                              "\",\"type\":\"urn:x-nmos:device:generic\",\"node_id\":\"" + m.node_id +
                              "\",\"senders\":[],\"receivers\":[],\"controls\":[]}");
            for (std::size_t i = 0; i < m.senders.size(); ++i) {
                const auto& s = m.senders[i];
                post("source", "{\"id\":\"" + s.source_id + "\",\"version\":\"" + v + "\",\"label\":\"" + json_escape(s.label) +
                                   "\",\"description\":\"" + json_escape(s.description) + "\",\"format\":\"" + s.format + "\",\"device_id\":\"" +
                                   m.device_id + "\",\"parents\":[],\"clock_name\":\"clk0\",\"tags\":{}}");
                post("flow", "{\"id\":\"" + s.flow_id + "\",\"version\":\"" + v + "\",\"label\":\"" + json_escape(s.label) +
                                 "\",\"description\":\"" + json_escape(s.description) + "\",\"format\":\"" + s.format + "\",\"media_type\":\"" +
                                 s.media_type + "\",\"source_id\":\"" + s.source_id + "\",\"device_id\":\"" + m.device_id +
                                 "\",\"parents\":[],\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" + json_escape(s.group) + "\"]}}");
                post("sender", "{\"id\":\"" + s.id + "\",\"version\":\"" + v + "\",\"label\":\"" + json_escape(s.label) +
                                   "\",\"description\":\"" + json_escape(s.description) + "\",\"flow_id\":\"" + s.flow_id + "\",\"transport\":\"" +
                                   "urn:x-nmos:transport:mxl\",\"device_id\":\"" + m.device_id +
                                   "\",\"manifest_href\":null,\"interface_bindings\":[],\"subscription\":{\"receiver_id\":null,\"active\":" +
                                   std::string(s.master_enable ? "true" : "false") + "}}");
            }
            http_exchange(host, port, "POST", "/x-nmos/registration/v1.3/health/nodes/" + m.node_id, "");
        }
        for (int i = 0; i < 50 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void NmosNode::handle(const HttpRequest& req, HttpResponse& res) {
    if (req.method == "OPTIONS") {
        res.status = 204;
        res.body.clear();
        return;
    }
    NmosModel m;
    {
        std::lock_guard lock(mu_);
        m = model_;
    }
    const auto& path = req.path;
    auto text = [&](const std::string& s) { res.set_text(s); };
    const std::string base = "http://" + m.host + ":" + std::to_string(m.api_port);
    if (path == "/" || path == "/x-nmos" || path == "/x-nmos/") {
        text("[{\"name\":\"node\",\"href\":\"" + base + "/x-nmos/node/\"},{\"name\":\"connection\",\"href\":\"" + base + "/x-nmos/connection/\"}]");
        return;
    }
    if (path == "/x-nmos/node/" || path == "/x-nmos/node") {
        text("[\"v1.3/\"]");
        return;
    }
    if (path == "/x-nmos/connection/" || path == "/x-nmos/connection") {
        text("[\"v1.1/\"]");
        return;
    }
    if (path == "/x-nmos/node/v1.3/" || path == "/x-nmos/node/v1.3") {
        text("[\"self/\",\"sources/\",\"flows/\",\"devices/\",\"senders/\",\"receivers/\"]");
        return;
    }
    if (path == "/x-nmos/connection/v1.1/" || path == "/x-nmos/connection/v1.1") {
        text("[\"single/\"]");
        return;
    }
    if (path == "/x-nmos/connection/v1.1/single/" || path == "/x-nmos/connection/v1.1/single") {
        text("[\"senders/\",\"receivers/\"]");
        return;
    }
    const std::string v = version();
    if (path == "/x-nmos/node/v1.3/self" || path == "/x-nmos/node/v1.3/self/") {
        text("{\"id\":\"" + m.node_id + "\",\"version\":\"" + v + "\",\"label\":\"" + json_escape(m.label) + "\",\"description\":\"MXL test and demo source\",\"hostname\":\"" +
             json_escape(m.host) + "\",\"href\":\"" + base +
             "/x-nmos/node/v1.3/self\",\"api\":{\"versions\":[\"v1.3\"],\"endpoints\":[{\"host\":\"" + json_escape(m.host) + "\",\"port\":" +
             std::to_string(m.api_port) + ",\"protocol\":\"http\"}]},\"caps\":{},\"services\":[],\"clocks\":[{\"name\":\"clk0\",\"ref_type\":\"internal\"}],\"tags\":{}}");
        return;
    }
    if (path == "/x-nmos/node/v1.3/devices" || path == "/x-nmos/node/v1.3/devices/") {
        text("[{\"id\":\"" + m.device_id + "\",\"version\":\"" + v + "\",\"label\":\"" + json_escape(m.label) +
             "\",\"description\":\"MXL Test Player\",\"type\":\"urn:x-nmos:device:generic\",\"node_id\":\"" + m.node_id +
             "\",\"senders\":[],\"receivers\":[],\"controls\":[],\"tags\":{}}]");
        return;
    }
    auto list_senders = [&]() {
        std::string s = "[";
        for (std::size_t i = 0; i < m.senders.size(); ++i) {
            const auto& sd = m.senders[i];
            if (i) s += ",";
            s += "{\"id\":\"" + sd.id + "\",\"version\":\"" + v + "\",\"label\":\"" + json_escape(sd.label) + "\",\"description\":\"" +
                 json_escape(sd.description) + "\",\"flow_id\":\"" + sd.flow_id + "\",\"transport\":\"urn:x-nmos:transport:mxl\",\"device_id\":\"" +
                 m.device_id + "\",\"manifest_href\":null,\"interface_bindings\":[],\"subscription\":{\"receiver_id\":null,\"active\":" +
                 (sd.master_enable ? "true" : "false") + "},\"tags\":{}}";
        }
        s += "]";
        return s;
    };
    if (path == "/x-nmos/node/v1.3/senders" || path == "/x-nmos/node/v1.3/senders/" || path == "/x-nmos/connection/v1.1/single/senders" ||
        path == "/x-nmos/connection/v1.1/single/senders/") {
        text(list_senders());
        return;
    }
    if (path == "/x-nmos/node/v1.3/receivers" || path == "/x-nmos/node/v1.3/receivers/" || path == "/x-nmos/connection/v1.1/single/receivers" ||
        path == "/x-nmos/connection/v1.1/single/receivers/") {
        text("[]");
        return;
    }
    if (path == "/x-nmos/node/v1.3/sources" || path == "/x-nmos/node/v1.3/sources/") {
        std::string s = "[";
        for (std::size_t i = 0; i < m.senders.size(); ++i) {
            if (i) s += ",";
            const auto& sd = m.senders[i];
            s += "{\"id\":\"" + sd.source_id + "\",\"version\":\"" + v + "\",\"label\":\"" + json_escape(sd.label) + "\",\"description\":\"" +
                 json_escape(sd.description) + "\",\"format\":\"" + sd.format + "\",\"caps\":{},\"device_id\":\"" + m.device_id +
                 "\",\"parents\":[],\"clock_name\":\"clk0\",\"tags\":{}}";
        }
        s += "]";
        text(s);
        return;
    }
    if (path == "/x-nmos/node/v1.3/flows" || path == "/x-nmos/node/v1.3/flows/") {
        std::string s = "[";
        for (std::size_t i = 0; i < m.senders.size(); ++i) {
            if (i) s += ",";
            const auto& sd = m.senders[i];
            s += "{\"id\":\"" + sd.flow_id + "\",\"version\":\"" + v + "\",\"label\":\"" + json_escape(sd.label) + "\",\"description\":\"" +
                 json_escape(sd.description) + "\",\"format\":\"" + sd.format + "\",\"media_type\":\"" + sd.media_type + "\",\"source_id\":\"" +
                 sd.source_id + "\",\"device_id\":\"" + m.device_id + "\",\"parents\":[],\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" +
                 json_escape(sd.group) + "\"]}}";
        }
        s += "]";
        text(s);
        return;
    }
    const std::string pfx = "/x-nmos/connection/v1.1/single/senders/";
    if (path.rfind(pfx, 0) == 0) {
        auto rest = path.substr(pfx.size());
        if (!rest.empty() && rest.back() == '/') rest.pop_back();
        std::string sid = rest;
        std::string leaf;
        auto slash = rest.find('/');
        if (slash != std::string::npos) {
            sid = rest.substr(0, slash);
            leaf = rest.substr(slash + 1);
        }
        const NmosSenderState* sd = nullptr;
        for (const auto& s : m.senders)
            if (s.id == sid) sd = &s;
        if (!sd) {
            res.status = 404;
            text("{\"error\":\"unknown sender\"}");
            return;
        }
        auto params = [&]() {
            return std::string("{\"sender_id\":\"") + sd->id + "\",\"master_enable\":" + (sd->master_enable ? "true" : "false") +
                   ",\"activation\":{\"mode\":null,\"requested_time\":null,\"activation_time\":null},\"transport_file\":{\"data\":null,\"type\":null},\"transport_params\":[{\"mxl_domain_id\":\"" +
                   m.domain_id + "\",\"mxl_flow_id\":\"" + sd->flow_id + "\"}]}";
        };
        if (leaf.empty()) {
            text(std::string("{\"id\":\"") + sd->id + "\",\"device_id\":\"" + m.device_id + "\",\"transport\":\"urn:x-nmos:transport:mxl\",\"interface_bindings\":[]}");
            return;
        }
        if (leaf == "transportfile") {
            res.status = 404;
            text("{\"error\":\"MXL senders have no transport file\"}");
            return;
        }
        if (leaf == "constraints") {
            text("[{\"mxl_domain_id\":{\"enum\":[\"" + m.domain_id + "\"]},\"mxl_flow_id\":{\"enum\":[\"" + sd->flow_id + "\"]}}]");
            return;
        }
        if (leaf == "active" || leaf == "staged") {
            if (req.method == "GET") {
                text(params());
                return;
            }
            if ((req.method == "PATCH" || req.method == "PUT") && leaf == "staged") {
                const std::string body(req.body.begin(), req.body.end());
                const bool has_master = body.find("\"master_enable\"") != std::string::npos;
                bool enable = sd->master_enable;
                if (has_master) enable = body.find("\"master_enable\":true") != std::string::npos || body.find("\"master_enable\": true") != std::string::npos;
                const bool activate = body.find("activate_immediate") != std::string::npos || body.find("\"mode\":\"activate\"") != std::string::npos;
                if (has_master && (activate || body.find("activation") == std::string::npos)) {
                    {
                        std::lock_guard lock(mu_);
                        for (auto& s : model_.senders)
                            if (s.id == sid) s.master_enable = enable;
                    }
                    if (on_master) on_master(sid, enable);
                }
                // Reflect the updated value.
                NmosSenderState copy = *sd;
                copy.master_enable = enable;
                sd = nullptr;
                text(std::string("{\"sender_id\":\"") + copy.id + "\",\"master_enable\":" + (copy.master_enable ? "true" : "false") +
                     ",\"activation\":{\"mode\":\"activate_immediate\",\"requested_time\":null,\"activation_time\":\"" + v +
                     "\"},\"transport_file\":{\"data\":null,\"type\":null},\"transport_params\":[{\"mxl_domain_id\":\"" + m.domain_id +
                     "\",\"mxl_flow_id\":\"" + copy.flow_id + "\"}]}");
                return;
            }
        }
        res.status = 404;
        text("{\"error\":\"not found\"}");
        return;
    }
    res.status = 404;
    text("{\"error\":\"not found\"}");
}

}  // namespace mtp
