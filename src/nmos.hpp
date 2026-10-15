#pragma once

#include "http_server.hpp"
#include "ids.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mtp {

struct NmosSenderState {
    std::string id;
    std::string flow_id;
    std::string source_id;
    std::string label;
    std::string description;
    std::string format;  // urn:x-nmos:format:...
    nlohmann::json flow;  // the flow's flow_def.json, registered as the IS-04 flow
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

// IS-04 v1.3 resources. The registry and the Node API serve the same bodies.
nlohmann::json nmos_node_json(const NmosModel& m, const std::string& version);
nlohmann::json nmos_device_json(const NmosModel& m, const std::string& version);
nlohmann::json nmos_source_json(const NmosModel& m, const NmosSenderState& s, const std::string& version);
nlohmann::json nmos_flow_json(const NmosModel& m, const NmosSenderState& s, const std::string& version);
nlohmann::json nmos_sender_json(const NmosModel& m, const NmosSenderState& s, const std::string& version);

// Registered resources by id: the Registration API path type ("sources", "flows", "senders").
using NmosRegistered = std::map<std::string, std::string>;

// The model's sources, flows and senders by id, as NmosRegistered.
NmosRegistered nmos_resources(const NmosModel& m);

// What is registered but no longer in the model (an output changed its format, dropped
// its ANC or key flow): {type, id}, senders first, then flows, then sources, so no
// sender is left pointing at a deleted flow.
std::vector<std::pair<std::string, std::string>> nmos_stale_resources(const NmosRegistered& registered, const NmosModel& m);

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
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    bool dirty_ = false;  // the model changed since the registry loop last posted it
    std::string registry_host_;
    int registry_port_ = 0;
    std::string query_host_;
    int query_port_ = 0;
    std::atomic<bool> registered_{false};
    std::atomic<bool> saw_delete_{false};
};

}  // namespace mtp
