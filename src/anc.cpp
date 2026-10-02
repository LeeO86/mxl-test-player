#include "anc.hpp"

#include <algorithm>
#include <cstring>

namespace mtp {
namespace {

std::uint16_t with_parity(std::uint8_t b) {
    int ones = 0;
    for (int i = 0; i < 8; ++i) ones += (b >> i) & 1;
    const int b8 = ones & 1;  // even parity bit
    std::uint16_t w = b;
    if (b8) w |= 0x100;
    else w |= 0x200;
    return w;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void put(std::uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            const int bit = (value >> i) & 1;
            if ((bitpos_ & 7) == 0) out_.push_back(0);
            if (bit) out_.back() = static_cast<std::uint8_t>(out_.back() | (0x80 >> (bitpos_ & 7)));
            ++bitpos_;
        }
    }
    void align32() {
        while (bitpos_ & 31) put(0, 1);
    }
    std::size_t bits() const { return bitpos_; }

private:
    std::vector<std::uint8_t>& out_;
    std::size_t bitpos_ = 0;
};

class BitReader {
public:
    BitReader(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
    int get(int bits) {
        int v = 0;
        for (int i = 0; i < bits; ++i) {
            if (byte_ >= n_) return -1;
            const int bit = (p_[byte_] >> (7 - (bitpos_ & 7))) & 1;
            v = (v << 1) | bit;
            ++bitpos_;
            if ((bitpos_ & 7) == 0) ++byte_;
        }
        return v;
    }

private:
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t byte_ = 0;
    std::size_t bitpos_ = 0;
};

void put_udw_nibble(std::uint8_t udw[16], int index, int nibble) {
    udw[index] = static_cast<std::uint8_t>((udw[index] & 0x0F) | ((nibble & 0x0F) << 4));
}

void set_dbb1(std::uint8_t udw[16], std::uint8_t dbb) {
    for (int i = 0; i < 8; ++i) {
        if ((dbb >> i) & 1) udw[i] = static_cast<std::uint8_t>(udw[i] | 0x08);
        else udw[i] = static_cast<std::uint8_t>(udw[i] & ~0x08);
    }
}

std::uint8_t get_dbb1(const std::uint8_t udw[16]) {
    std::uint8_t d = 0;
    for (int i = 0; i < 8; ++i) {
        if (udw[i] & 0x08) d = static_cast<std::uint8_t>(d | (1u << i));
    }
    return d;
}

int nibble_of(const std::uint8_t udw[16], int index) { return (udw[index] >> 4) & 0x0F; }

}  // namespace

std::vector<std::uint8_t> encode_anc_grain(const AncPacket& pkt) {
    std::uint8_t udw[16] = {};
    const int fu = pkt.tc.ff % 10;
    const int ft = pkt.tc.ff / 10;
    const int su = pkt.tc.ss % 10;
    const int st = pkt.tc.ss / 10;
    const int mu = pkt.tc.mm % 10;
    const int mt = pkt.tc.mm / 10;
    const int hu = pkt.tc.hh % 10;
    const int ht = pkt.tc.hh / 10;
    // Even UDWs carry time-address nibbles (b7..b4), b4 is the LSB.
    put_udw_nibble(udw, 0, fu);
    int frame_tens = ft & 0x3;
    if (pkt.tc.drop) frame_tens |= 0x4;  // LTC bit 10, drop-frame flag
    put_udw_nibble(udw, 2, frame_tens);
    put_udw_nibble(udw, 4, su);
    put_udw_nibble(udw, 6, st & 0x7);
    put_udw_nibble(udw, 8, mu);
    put_udw_nibble(udw, 10, mt & 0x7);
    put_udw_nibble(udw, 12, hu);
    int hour_tens = ht & 0x3;
    if (pkt.tc.field) hour_tens |= 0x8;  // field flag in the hours-tens flag bit (LTC bit 59)
    put_udw_nibble(udw, 14, hour_tens);

    std::uint8_t dbb = 0;
    if (pkt.kind == AtcKind::Vitc1) dbb = 0x01;
    else if (pkt.kind == AtcKind::Vitc2) dbb = 0x02;
    set_dbb1(udw, dbb);

    std::vector<std::uint16_t> words;
    words.push_back(with_parity(0x60));
    words.push_back(with_parity(0x60));
    words.push_back(with_parity(16));
    std::uint32_t sum = 0;
    auto acc = [&](std::uint16_t w) { sum = (sum + (w & 0x1FF)) & 0x1FF; };
    acc(words[0]);
    acc(words[1]);
    acc(words[2]);
    for (int i = 0; i < 16; ++i) {
        // b2..b0 are zero; b3 is DBB; b7..b4 are the nibble. Parity covers b7..b0.
        const auto w = with_parity(udw[i]);
        words.push_back(w);
        acc(w);
    }
    const std::uint16_t cs = static_cast<std::uint16_t>((sum & 0x1FF) | (((sum & 0x100) ? 0 : 1) << 9));
    words.push_back(cs);

    std::vector<std::uint8_t> anc_bytes;
    BitWriter bw(anc_bytes);
    // C=0 (luma), line, horizontal offset 0xFFE (HANC), S=0, stream 0.
    const std::uint32_t header = (0u << 31) | ((static_cast<std::uint32_t>(pkt.line) & 0x7FFu) << 20) | ((0xFFEu & 0xFFFu) << 8) |
                                 (0u << 7) | 0u;
    bw.put(header, 32);
    for (auto w : words) bw.put(w, 10);
    bw.align32();

    const int f = !pkt.interlaced ? 0 : (pkt.tc.field ? 0x3 : 0x2);
    std::vector<std::uint8_t> grain(kAncGrainSize, 0);
    grain[0] = static_cast<std::uint8_t>(pkt.sequence >> 8);
    grain[1] = static_cast<std::uint8_t>(pkt.sequence);
    const std::uint16_t length = static_cast<std::uint16_t>(anc_bytes.size());
    grain[2] = static_cast<std::uint8_t>(length >> 8);
    grain[3] = static_cast<std::uint8_t>(length);
    grain[4] = 1;  // ANC_Count
    grain[5] = static_cast<std::uint8_t>(f << 6);
    std::memcpy(grain.data() + 8, anc_bytes.data(), std::min(anc_bytes.size(), grain.size() - 8));
    return grain;
}

bool decode_anc_timecode(const std::uint8_t* grain, std::size_t size, Timecode& out, AtcKind& kind) {
    if (size < 16) return false;
    const int count = grain[4];
    if (count < 1) return false;
    const std::uint16_t length = static_cast<std::uint16_t>((grain[2] << 8) | grain[3]);
    if (static_cast<std::size_t>(8 + length) > size) return false;
    BitReader br(grain + 8, length);
    if (br.get(32) < 0) return false;
    auto read10 = [&]() { return br.get(10); };
    const int did = read10();
    const int sdid = read10();
    const int dc = read10();
    if (did < 0 || sdid < 0 || dc < 0) return false;
    if ((did & 0xFF) != 0x60 || (sdid & 0xFF) != 0x60) return false;
    const int nwords = dc & 0xFF;
    if (nwords < 16) return false;
    std::uint8_t udw[16];
    for (int i = 0; i < 16; ++i) {
        const int w = read10();
        if (w < 0) return false;
        udw[i] = static_cast<std::uint8_t>(w & 0xFF);
    }
    const int dbb = get_dbb1(udw);
    if (dbb == 0x01) kind = AtcKind::Vitc1;
    else if (dbb == 0x02) kind = AtcKind::Vitc2;
    else kind = AtcKind::Ltc;

    const int fu = nibble_of(udw, 0);
    const int ft = nibble_of(udw, 2);
    const int su = nibble_of(udw, 4);
    const int st = nibble_of(udw, 6);
    const int mu = nibble_of(udw, 8);
    const int mt = nibble_of(udw, 10);
    const int hu = nibble_of(udw, 12);
    const int ht = nibble_of(udw, 14);
    out = {};
    out.ff = (ft & 0x3) * 10 + (fu & 0xF);
    out.ss = (st & 0x7) * 10 + (su & 0xF);
    out.mm = (mt & 0x7) * 10 + (mu & 0xF);
    out.hh = (ht & 0x3) * 10 + (hu & 0xF);
    out.drop = (ft & 0x4) != 0;
    out.field = (ht & 0x8) ? 1 : 0;
    const int f = (grain[5] >> 6) & 0x3;
    if (f == 0x3) out.field = 1;
    else if (f == 0x2) out.field = 0;
    return true;
}

}  // namespace mtp
