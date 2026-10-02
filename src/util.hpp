#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mtp {

inline void log_line(const char* level, const std::string& msg) {
    std::fprintf(stderr, "%s %s\n", level, msg.c_str());
    std::fflush(stderr);
}

inline void log_info(const std::string& m) { log_line("INFO", m); }
inline void log_warn(const std::string& m) { log_line("WARN", m); }
inline void log_error(const std::string& m) { log_line("ERROR", m); }

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline std::string hex_u64(std::uint64_t v) {
    char b[17];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
    return b;
}

inline std::uint64_t fnv1a64(const void* data, std::size_t n) {
    auto p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = 14695981039346656037ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

inline std::uint64_t fnv1a64(std::string_view s) { return fnv1a64(s.data(), s.size()); }

inline std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

inline std::string trim(std::string_view s) {
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    std::size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return std::string(s.substr(a, b - a));
}

inline bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline std::uint32_t xorshift32(std::uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

inline std::uint32_t hash_u64(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return static_cast<std::uint32_t>(x);
}

}  // namespace mtp
