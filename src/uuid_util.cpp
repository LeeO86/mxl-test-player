#include "uuid_util.hpp"

#include "util.hpp"

#include <cstdio>

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
            w[i] = (std::uint32_t(buf[off + i * 4]) << 24) | (std::uint32_t(buf[off + i * 4 + 1]) << 16) |
                   (std::uint32_t(buf[off + i * 4 + 2]) << 8) | std::uint32_t(buf[off + i * 4 + 3]);
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
            const std::uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    const std::uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(hs[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(hs[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(hs[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(hs[i]);
    }
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string Uuid::str() const {
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6],
                  b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return buf;
}

Uuid Uuid::parse(std::string_view s) {
    Uuid u;
    std::string hex;
    hex.reserve(32);
    for (char c : s) {
        if (c != '-') hex.push_back(c);
    }
    if (hex.size() != 32) throw Error("invalid uuid");
    for (int i = 0; i < 16; ++i) {
        const int hi = hex_nibble(hex[i * 2]);
        const int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) throw Error("invalid uuid");
        u.b[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return u;
}

Uuid uuid_v5(const Uuid& ns, std::string_view name) {
    std::vector<std::uint8_t> msg(16 + name.size());
    std::memcpy(msg.data(), ns.b.data(), 16);
    std::memcpy(msg.data() + 16, name.data(), name.size());
    std::uint8_t dig[20];
    sha1(msg.data(), msg.size(), dig);
    Uuid u;
    std::memcpy(u.b.data(), dig, 16);
    u.b[6] = static_cast<std::uint8_t>((u.b[6] & 0x0F) | 0x50);
    u.b[8] = static_cast<std::uint8_t>((u.b[8] & 0x3F) | 0x80);
    return u;
}

Uuid uuid_namespace_dns() { return Uuid::parse("6ba7b810-9dad-11d1-80b4-00c04fd430c8"); }

Uuid uuid_from_seed(std::string_view seed) { return uuid_v5(uuid_namespace_dns(), std::string("mxl-test-player/") + std::string(seed)); }

}  // namespace mtp
