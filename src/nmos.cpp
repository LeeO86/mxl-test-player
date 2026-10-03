#include "nmos.hpp"

#include "process.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
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
    timeval tv{};
    tv.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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

int http_status(const std::string& response) {
    const auto sp = response.find(' ');
    if (sp == std::string::npos) return 0;
    return std::atoi(response.c_str() + sp + 1);
}

std::string base_href(const NmosModel& m) { return "http://" + m.host + ":" + std::to_string(m.api_port); }

}  // namespace

nlohmann::json nmos_node_json(const NmosModel& m, const std::string& version) {
    return {{"id", m.node_id},
            {"version", version},
            {"label", m.label},
            {"description", "MXL test and demo source"},
            {"tags", m.tags},
            {"href", base_href(m) + "/x-nmos/node/v1.3/self"},
            {"hostname", m.host},
            {"api", {{"versions", {"v1.3"}}, {"endpoints", {{{"host", m.host}, {"port", m.api_port}, {"protocol", "http"}}}}}},
            {"caps", nlohmann::json::object()},
            {"services", nlohmann::json::array()},
            {"clocks", {{{"name", "clk0"}, {"ref_type", "internal"}}}},
            {"interfaces", nlohmann::json::array()}};
}

nlohmann::json nmos_device_json(const NmosModel& m, const std::string& version) {
    return {{"id", m.device_id},
            {"version", version},
            {"label", m.label},
            {"description", "MXL Test Player"},
            {"tags", m.tags},
            {"type", "urn:x-nmos:device:generic"},
            {"node_id", m.node_id},
            {"senders", nlohmann::json::array()},
            {"receivers", nlohmann::json::array()},
            {"controls", {{{"href", base_href(m) + "/x-nmos/connection/v1.1/"}, {"type", "urn:x-nmos:control:sr-ctrl/v1.1"}}}}};
}

nlohmann::json nmos_source_json(const NmosModel& m, const NmosSenderState& s, const std::string& version) {
    nlohmann::json j{{"id", s.source_id},
                     {"version", version},
                     {"label", s.label},
                     {"description", s.description},
                     {"tags", nlohmann::json::object()},
                     {"format", s.format},
                     {"caps", nlohmann::json::object()},
                     {"device_id", m.device_id},
                     {"parents", nlohmann::json::array()},
                     {"clock_name", "clk0"}};
    // Audio sources must list their channels (source_audio.json).
    if (s.format == "urn:x-nmos:format:audio") {
        auto channels = nlohmann::json::array();
        for (int c = 1; c <= s.flow.value("channel_count", 0); ++c) channels.push_back({{"label", "Channel " + std::to_string(c)}});
        j["channels"] = channels;
    }
    return j;
}

nlohmann::json nmos_flow_json(const NmosModel& m, const NmosSenderState& s, const std::string& version) {
    nlohmann::json j = s.flow;
    j["version"] = version;
    j["device_id"] = m.device_id;
    return j;
}

nlohmann::json nmos_sender_json(const NmosModel& m, const NmosSenderState& s, const std::string& version) {
    return {{"id", s.id},
            {"version", version},
            {"label", s.label},
            {"description", s.description},
            {"tags", nlohmann::json::object()},
            {"flow_id", s.flow_id},
            {"transport", "urn:x-nmos:transport:mxl"},
            {"device_id", m.device_id},
            {"manifest_href", nullptr},
            {"interface_bindings", nlohmann::json::array()},
            {"subscription", {{"receiver_id", nullptr}, {"active", s.master_enable}}}};
}

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

void NmosNode::start(const std::string& registry_host, int registry_port, const std::string& query_host, int query_port) {
    registry_host_ = registry_host;
    registry_port_ = registry_port;
    query_host_ = query_host.empty() ? registry_host : query_host;
    query_port_ = query_port;
    stop_ = false;
    registered_ = false;
    if (!registry_host.empty()) reg_ = std::thread([this, registry_host, registry_port] { registry_loop(registry_host, registry_port); });
}

void NmosNode::stop() {
    stop_ = true;
    if (reg_.joinable()) reg_.join();
}

bool NmosNode::registry_configured() const { return !registry_host_.empty(); }

void NmosNode::deregister() {
    stop_ = true;
    if (reg_.joinable()) reg_.join();
    std::string node_id;
    {
        std::lock_guard lock(mu_);
        node_id = model_.node_id;
    }
    if (registry_host_.empty() || node_id.empty()) return;
    const auto resp = http_exchange(registry_host_, registry_port_, "DELETE", "/x-nmos/registration/v1.3/resource/nodes/" + node_id, "");
    const int code = http_status(resp);
    if (code >= 200 && code < 300) log_info("deregistered NMOS node " + node_id);
    else log_error("NMOS deregister of " + node_id + " returned " + std::to_string(code));
    registered_ = false;
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
            auto post = [&](const std::string& type, const nlohmann::json& data) {
                return http_exchange(host, port, "POST", "/x-nmos/registration/v1.3/resource",
                                     nlohmann::json{{"type", type}, {"data", data}}.dump());
            };
            const auto node_resp = post("node", nmos_node_json(m, v));
            post("device", nmos_device_json(m, v));
            for (const auto& s : m.senders) {
                post("source", nmos_source_json(m, s, v));
                post("flow", nmos_flow_json(m, s, v));
                post("sender", nmos_sender_json(m, s, v));
            }
            const auto health = http_exchange(host, port, "POST", "/x-nmos/registration/v1.3/health/nodes/" + m.node_id, "");
            const int posted = http_status(node_resp);
            const int health_code = http_status(health);
            bool ok = (posted >= 200 && posted < 300) || (health_code >= 200 && health_code < 300);
            if (ok && !query_host_.empty()) {
                const auto q = http_exchange(query_host_, query_port_, "GET", "/x-nmos/query/v1.3/nodes/" + m.node_id, "");
                const int qc = http_status(q);
                ok = qc >= 200 && qc < 300;
            }
            registered_ = ok;
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
        text(nmos_node_json(m, v).dump());
        return;
    }
    if (path == "/x-nmos/node/v1.3/devices" || path == "/x-nmos/node/v1.3/devices/") {
        text(nlohmann::json::array({nmos_device_json(m, v)}).dump());
        return;
    }
    auto list = [&](auto build) {
        auto a = nlohmann::json::array();
        for (const auto& sd : m.senders) a.push_back(build(m, sd, v));
        return a.dump();
    };
    if (path == "/x-nmos/node/v1.3/senders" || path == "/x-nmos/node/v1.3/senders/" || path == "/x-nmos/connection/v1.1/single/senders" ||
        path == "/x-nmos/connection/v1.1/single/senders/") {
        text(list(nmos_sender_json));
        return;
    }
    if (path == "/x-nmos/node/v1.3/receivers" || path == "/x-nmos/node/v1.3/receivers/" || path == "/x-nmos/connection/v1.1/single/receivers" ||
        path == "/x-nmos/connection/v1.1/single/receivers/") {
        text("[]");
        return;
    }
    if (path == "/x-nmos/node/v1.3/sources" || path == "/x-nmos/node/v1.3/sources/") {
        text(list(nmos_source_json));
        return;
    }
    if (path == "/x-nmos/node/v1.3/flows" || path == "/x-nmos/node/v1.3/flows/") {
        text(list(nmos_flow_json));
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
