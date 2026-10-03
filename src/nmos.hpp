#pragma once

#include "http_server.hpp"
#include "ids.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mtp {

struct NmosSenderState {
    std::string id;
    std::string flow_id;
    std::string source_id;
    std::string label;
    std::string description;
    std::string format;  // urn:x-nmos:format:...
    std::string media_type;
    std::string group;
    bool master_enable = true;
    int index = 0;
};

struct NmosModel {
    std::string node_id;
    std::string device_id;
    std::string label = "MXL Test Player";
    std::string host;
    int api_port = 3282;
    std::string domain_id;
    nlohmann::json tags = nlohmann::json::object();
    std::vector<NmosSenderState> senders;
    std::vector<std::string> source_ids;
    std::vector<std::string> source_labels;
    std::vector<std::string> source_formats;
    std::vector<std::string> flow_ids;
    std::vector<std::string> flow_labels;
    std::vector<std::string> flow_formats;
    std::vector<std::string> flow_sources;
    std::vector<std::string> flow_media;
    std::vector<std::string> flow_json;  // full flow resource body pieces built by the node
};

class NmosNode {
public:
    void update(NmosModel model);
    void start(const std::string& registry_host, int registry_port, const std::string& query_host, int query_port);
    void stop();
    void deregister();
    bool registered() const { return registered_.load(); }
    bool registry_configured() const;
    void handle(const HttpRequest& req, HttpResponse& res);
    bool master_enabled(const std::string& sender_id) const;
    std::function<void(const std::string& sender_id, bool enabled)> on_master;

private:
    void registry_loop(std::string host, int port);
    std::string version() const;

    mutable std::mutex mu_;
    NmosModel model_;
    std::thread reg_;
    bool stop_ = false;
    std::string registry_host_;
    int registry_port_ = 0;
    std::string query_host_;
    int query_port_ = 0;
    std::atomic<bool> registered_{false};
    std::atomic<bool> saw_delete_{false};
};

}  // namespace mtp
