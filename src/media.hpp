#pragma once

#include "config.hpp"
#include "format.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mtp {

enum class ItemType { Video, Still, Sprite };

struct LibraryItem {
    std::string id;
    std::string name;
    ItemType type = ItemType::Video;
    std::vector<std::string> tags;
    nlohmann::json original = nlohmann::json::object();
    std::string fit = "fit";
    std::string fps_mode = "drop";
    std::string alpha_mode = "straight";
    bool loudness = false;
    int crossfade_ms = 0;
    int map_channels = 0;  // 0 = keep source, pad
    nlohmann::json conversions = nlohmann::json::object();
    bool in_use = false;
};

struct ConvertJob {
    std::string id;
    std::string item_id;
    std::string format;
    std::string state;  // queued, running, done, failed
    double progress = 0;
    std::string error;
    double duration_s = 0;
};

class Library {
public:
    explicit Library(Config cfg);
    ~Library();

    void start();
    void stop();

    std::vector<LibraryItem> list() const;
    LibraryItem get(const std::string& id) const;
    bool exists(const std::string& id) const;
    nlohmann::json to_json(const LibraryItem& it) const;

    // Assemble a finished upload into the library and queue conversion for `format`.
    LibraryItem ingest_file(const std::string& path, const std::string& name, const nlohmann::json& options, const VideoFormat& format);

    bool reconvert(const std::string& id, const nlohmann::json& options, const VideoFormat& format, std::string& error);
    bool remove(const std::string& id, std::string& error);
    bool update_meta(const std::string& id, const std::string& name, const std::vector<std::string>& tags, std::string& error);
    void mark_used(const std::string& id, bool used);
    void purge_uploads();

    void ensure_format(const std::string& id, const VideoFormat& format);

    std::vector<ConvertJob> jobs() const;
    std::string item_dir(const std::string& id) const;
    std::uint64_t total_bytes() const;
    int item_count() const;

    // Blocks until queued work for this item/format is finished or failed. Used by tests.
    bool wait_ready(const std::string& id, const std::string& format, int timeout_ms);

    std::function<void()> on_change;

private:
    void worker();
    void watch_import();
    void load_index();
    void save_index() const;
    void save_item(const LibraryItem& it) const;
    bool convert_one(LibraryItem& item, const VideoFormat& format, ConvertJob& job);
    void enqueue(const std::string& id, const VideoFormat& format);

    Config cfg_;
    mutable std::mutex mu_;
    std::vector<LibraryItem> items_;
    std::vector<ConvertJob> jobs_;
    std::vector<std::thread> workers_;
    std::thread watcher_;
    std::atomic<bool> stop_{false};
    std::uint64_t bytes_ = 0;
};

const char* item_type_name(ItemType t);
ItemType parse_item_type(std::string_view s);

}  // namespace mtp
