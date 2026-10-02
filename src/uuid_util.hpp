#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace mtp {

struct Uuid {
    std::array<std::uint8_t, 16> b{};

    std::string str() const;
    static Uuid parse(std::string_view s);
    bool operator==(const Uuid& o) const { return b == o.b; }
};

// RFC 4122 name-based UUID (SHA-1).
Uuid uuid_v5(const Uuid& ns, std::string_view name);

// DNS namespace 6ba7b810-9dad-11d1-80b4-00c04fd430c8
Uuid uuid_namespace_dns();

Uuid uuid_from_seed(std::string_view seed);

}  // namespace mtp
