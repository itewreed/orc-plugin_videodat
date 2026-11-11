/*
 * File:        vd_1990.cpp
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Packet and file layer of the 1990 variant
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "vd_1990.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>

namespace videodat {

namespace {

constexpr uint8_t kSOH = 0x01;
constexpr uint8_t kESC = 0x18;
constexpr uint8_t kHEADER = 0x20;
constexpr int64_t kNSEQ = 223;
constexpr uint8_t kFILL = 0x2A;

Bytes sub(const Bytes& b, size_t from, size_t to)
{
    from = std::min(from, b.size());
    to = std::min(std::max(from, to), b.size());
    return Bytes(b.begin() + static_cast<long>(from), b.begin() + static_cast<long>(to));
}

Bytes unescape(const Bytes& raw)
{
    Bytes out;
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == kESC && i + 1 < raw.size()) {
            out.push_back(raw[i + 1] ^ 0x20);
            i += 2;
        } else {
            out.push_back(raw[i]);
            ++i;
        }
    }
    return out;
}

bool check_ok(const Bytes& data_raw, uint8_t ck)
{
    unsigned s = 0;
    for (uint8_t b : data_raw) s += b;
    s &= 0xFF;
    return ck == s || (s < 0x20 && ck == (s ^ 0xFF));
}

struct Pkt90 {
    size_t pos{0};
    uint8_t A{0}, S{0};
    Bytes raw;
    Bytes data;
    bool ok{false};
    bool has_soft{false};
    std::vector<float> soft;
    std::string how;
};

std::optional<Pkt90> make(const Bytes& stream, const std::vector<float>* soft, size_t p)
{
    if (p + 5 > stream.size()) return std::nullopt;
    const uint8_t A = stream[p + 1], S = stream[p + 2], Lb = stream[p + 3];
    const int n = Lb - 0x20;
    if (n < 0 || A < 0x20 || S < 0x20 || p + 5 + static_cast<size_t>(n) > stream.size()) return std::nullopt;
    Pkt90 x;
    x.pos = p;
    x.raw = sub(stream, p, p + 5 + static_cast<size_t>(n));
    x.A = A;
    x.S = S;
    x.ok = check_ok(sub(x.raw, 4, 4 + static_cast<size_t>(n)), x.raw[4 + static_cast<size_t>(n)]);
    x.data = unescape(sub(x.raw, 4, 4 + static_cast<size_t>(n)));
    if (soft) {
        x.has_soft = true;
        const size_t a = std::min(soft->size(), 8 * p), b = std::min(soft->size(), 8 * (p + 5 + static_cast<size_t>(n)));
        x.soft.assign(soft->begin() + static_cast<long>(a), soft->begin() + static_cast<long>(b));
    }
    x.how = x.ok ? "ok" : "";
    return x;
}

std::vector<Pkt90> find_packets90(const Bytes& stream, const std::vector<float>* soft)
{
    std::vector<Pkt90> pk;
    size_t p = 0;
    auto find = [&](size_t from) -> long {
        for (size_t i = from; i < stream.size(); ++i) {
            if (stream[i] == kSOH) return static_cast<long>(i);
        }
        return -1;
    };
    long q = find(0);
    while (q >= 0) {
        p = static_cast<size_t>(q);
        auto x = make(stream, soft, p);
        size_t nxt = p + 1;
        if (x) {
            if (x->ok) nxt = p + x->raw.size();
            pk.push_back(std::move(*x));
        }
        q = find(nxt);
    }
    return pk;
}

int repair90(std::vector<Pkt90>& pk, int64_t window = 8000)
{
    std::vector<std::pair<Bytes, std::vector<Pkt90*>>> groups;
    std::map<Bytes, size_t> gi;
    for (Pkt90& x : pk) {
        const Bytes k = sub(x.raw, 1, 4);
        auto it = gi.find(k);
        if (it == gi.end()) {
            gi.emplace(k, groups.size());
            groups.push_back({k, {&x}});
        } else {
            groups[it->second].second.push_back(&x);
        }
    }
    int fixed = 0;
    for (auto& [key, xs] : groups) {
        for (Pkt90* x : xs) {
            if (x->ok) continue;
            std::vector<Pkt90*> near;
            for (Pkt90* y : xs) {
                if (std::llabs(static_cast<long long>(y->pos) - static_cast<long long>(x->pos)) <= window) near.push_back(y);
            }
            bool any_ok = false;
            for (Pkt90* y : near) any_ok = any_ok || y->ok;
            if (any_ok) continue;
            std::vector<Pkt90*> cop;
            for (Pkt90* y : near) {
                if (y->has_soft && y->soft.size() == 8 * y->raw.size() && y->raw.size() == x->raw.size()) cop.push_back(y);
            }
            if (cop.size() < 2) continue;
            std::vector<float> m(cop[0]->soft.size(), 0.0f);
            for (Pkt90* y : cop) {
                for (size_t i = 0; i < m.size(); ++i) m[i] = m[i] + y->soft[i];
            }
            Bytes raw(m.size() / 8);
            for (size_t i = 0; i < raw.size(); ++i) {
                unsigned v = 0;
                for (int b = 0; b < 8; ++b) {
                    if (m[8 * i + static_cast<size_t>(b)] >= 0.0f) v |= 1u << b;
                }
                raw[i] = static_cast<uint8_t>(v ^ 0xFF);
            }
            for (size_t i = 0; i < 4; ++i) raw[i] = x->raw[i];
            const size_t n = raw.size() - 5;
            if (!check_ok(sub(raw, 4, 4 + n), raw[4 + n])) continue;
            const Bytes data = unescape(sub(raw, 4, 4 + n));
            for (Pkt90* y : cop) {
                y->raw = raw;
                y->data = data;
                y->ok = true;
                y->how = "soft";
                ++fixed;
            }
        }
    }
    return fixed;
}

struct Header90 {
    std::string name;
    std::optional<uint32_t> size;
    int group;
};

std::optional<Header90> parse_header(const Bytes& data)
{
    if (data.size() < 6) return std::nullopt;
    std::string name;
    for (size_t i = 5; i < data.size() && data[i] != 0; ++i) {
        if (data[i] >= 0x21 && data[i] <= 0x7e) name.push_back(static_cast<char>(data[i]));
    }
    if (name.empty()) return std::nullopt;
    Header90 h;
    h.name = name;
    if (!(data[0] == ' ' && data[1] == ' ' && data[2] == ' ' && data[3] == ' ')) {
        h.size = static_cast<uint32_t>(data[0]) << 24 | static_cast<uint32_t>(data[1]) << 16 |
                 static_cast<uint32_t>(data[2]) << 8 | data[3];
    }
    h.group = data[4];
    return h;
}

std::string safe90(const std::string& name)
{
    std::string n;
    for (char c : name) n.push_back(std::string("\\/:*?\"<>|").find(c) != std::string::npos ? '_' : c);
    size_t a = 0, b = n.size();
    while (a < b && (n[a] == ' ' || n[a] == '.')) ++a;
    while (b > a && (n[b - 1] == ' ' || n[b - 1] == '.')) --b;
    n = n.substr(a, b - a);
    return n.empty() ? "unbenannt" : n;
}

int64_t unwrap(std::optional<int64_t> last, int64_t s)
{
    if (!last) return s;
    int64_t d = ((s - *last) % kNSEQ + kNSEQ) % kNSEQ;
    if (d > kNSEQ - 40) d -= kNSEQ;
    return *last + d;
}

bool is_fill(const Bytes& b)
{
    for (uint8_t c : b) {
        if (c != kFILL) return false;
    }
    return true;
}

std::string lower(std::string s)
{
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// --- text_view -------------------------------------------------------------

std::optional<std::pair<int, std::string>> text_view(const Bytes& data)
{
    if (data.size() < 16) return std::nullopt;
    auto weight = [](int c) -> double {
        if (c >= 'a' && c <= 'z') return 1.0;
        if (c >= 'A' && c <= 'Z') return 0.5;
        if (c == 0x20) return 3.0;
        if (c == 0x0D || c == 0x0A) return 1.0;
        return 0.0;
    };
    Counter<uint8_t> cnt;
    for (uint8_t b : data) cnt.add(b);
    int bestk = 0;
    double bestv = 0;
    for (int k = 0; k < 256; ++k) {
        double s = 0;
        for (const auto& [b, n] : cnt.items()) s += static_cast<double>(n) * weight(b ^ k);
        if (k == 0 || s > bestv) {
            bestv = s;
            bestk = k;
        }
    }
    Bytes t;
    size_t good = 0;
    for (uint8_t b : data) {
        const uint8_t c = static_cast<uint8_t>(b ^ bestk);
        t.push_back(c);
        if ((c >= 32 && c < 127) || c == 1 || c == 2 || c == 3 || c == 4 || c == 9 || c == 10 || c == 13) ++good;
    }
    if (static_cast<double>(good) < 0.95 * static_cast<double>(t.size())) return std::nullopt;
    std::string s;
    for (uint8_t c : t) {
        switch (c) {
        case '[': s += "\xC3\x84"; break;
        case '\\': s += "\xC3\x96"; break;
        case ']': s += "\xC3\x9C"; break;
        case '{': s += "\xC3\xA4"; break;
        case '|': s += "\xC3\xB6"; break;
        case '}': s += "\xC3\xBC"; break;
        case '~': s += "\xC3\x9F"; break;
        case '@': s += "\xC2\xA7"; break;
        default:
            if (c >= 0x80) {
                s += "\xEF\xBF\xBD";
            } else {
                s.push_back(static_cast<char>(c));
            }
        }
    }
    auto replace_all = [](std::string& str, const std::string& from, const std::string& to) {
        std::string out;
        size_t i = 0;
        while (true) {
            const size_t j = str.find(from, i);
            if (j == std::string::npos) break;
            out += str.substr(i, j - i) + to;
            i = j + from.size();
        }
        str = out + str.substr(i);
    };
    replace_all(s, "\r\r\n", "\n");
    replace_all(s, "\r\n", "\n");
    std::string r;
    for (char c : s) {
        if (c < 1 || c > 4) r.push_back(c);
    }
    return std::make_pair(bestk, r);
}

// --- ZIP check ---------------------------------------------------------------

uint32_t crc32z(const Bytes& b)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t c : b) crc = table[(crc ^ c) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t rd32(const Bytes& b, size_t i)
{
    return static_cast<uint32_t>(b[i]) | static_cast<uint32_t>(b[i + 1]) << 8 | static_cast<uint32_t>(b[i + 2]) << 16 |
           static_cast<uint32_t>(b[i + 3]) << 24;
}
uint32_t rd16(const Bytes& b, size_t i) { return b[i] | b[i + 1] << 8; }

struct ZipCheck {
    std::vector<std::string> names;
    std::optional<std::string> err;
};

std::optional<ZipCheck> check_zip(const Bytes& data)
{
    // end of central directory record
    if (data.size() < 22) return std::nullopt;
    long eocd = -1;
    for (long i = static_cast<long>(data.size()) - 22; i >= 0 && i >= static_cast<long>(data.size()) - 22 - 65535; --i) {
        if (rd32(data, static_cast<size_t>(i)) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return std::nullopt;
    const size_t e = static_cast<size_t>(eocd);
    const uint32_t cd_size = rd32(data, e + 12);
    const uint32_t cd_off_rec = rd32(data, e + 16);
    // Python: concat = endrec offset - size_cd - offset_cd (archives with a
    // prefix); positions below are relative to that.
    const int64_t concat = static_cast<int64_t>(e) - cd_size - cd_off_rec;
    if (concat < 0) return std::nullopt;
    struct Entry {
        std::string name;
        uint16_t flags, method;
        uint32_t crc, csize, usize, hoff;
    };
    std::vector<Entry> entries;
    size_t p = static_cast<size_t>(concat) + cd_off_rec;
    const size_t end = p + cd_size;
    while (p < end) {
        if (p + 46 > data.size() || rd32(data, p) != 0x02014b50) return std::nullopt;
        Entry en;
        en.flags = static_cast<uint16_t>(rd16(data, p + 8));
        en.method = static_cast<uint16_t>(rd16(data, p + 10));
        en.crc = rd32(data, p + 16);
        en.csize = rd32(data, p + 20);
        en.usize = rd32(data, p + 24);
        const uint32_t nl = rd16(data, p + 28), xl = rd16(data, p + 30), cl = rd16(data, p + 32);
        en.hoff = rd32(data, p + 42);
        if (p + 46 + nl > data.size()) return std::nullopt;
        if (en.flags & 0x800) {
            en.name.assign(data.begin() + static_cast<long>(p + 46), data.begin() + static_cast<long>(p + 46 + nl));
        } else {
            en.name = cp437_to_utf8(data.data() + p + 46, nl);
        }
        entries.push_back(en);
        p += 46 + nl + xl + cl;
    }
    ZipCheck z;
    for (const auto& en : entries) z.names.push_back(en.name);
    for (const auto& en : entries) {
        const size_t h = static_cast<size_t>(concat) + en.hoff;
        if (h + 30 > data.size() || rd32(data, h) != 0x04034b50) {
            z.err = "CRC falsch in " + en.name;  // BadZipFile -> testzip returns the name
            return z;
        }
        if (en.flags & 0x1) {
            z.err = "File " + std::string("'") + en.name + "' is encrypted, password required for extraction";
            return z;
        }
        if (en.method != 0 && en.method != 8) {
            z.err = "Verzeichnis ok; Kompression nicht pruefbar (unzip -t)";
            return z;
        }
        const size_t d0 = h + 30 + rd16(data, h + 26) + rd16(data, h + 28);
        if (d0 + en.csize > data.size()) {
            z.err = "CRC falsch in " + en.name;
            return z;
        }
        Bytes content;
        if (en.method == 0) {
            content.assign(data.begin() + static_cast<long>(d0), data.begin() + static_cast<long>(d0 + en.csize));
        } else if (!inflate_raw(data.data() + d0, en.csize, content)) {
            z.err = "CRC falsch in " + en.name;
            return z;
        }
        if (crc32z(content) != en.crc) {
            z.err = "CRC falsch in " + en.name;
            return z;
        }
    }
    return z;
}

} // namespace

// --- inflate (after Mark Adler's puff) --------------------------------------

namespace {
struct InState {
    const uint8_t* in;
    size_t inlen, incnt;
    uint32_t bitbuf;
    int bitcnt;
    Bytes* out;
    bool err{false};
};

int bits(InState& s, int need)
{
    long val = s.bitbuf;
    while (s.bitcnt < need) {
        if (s.incnt == s.inlen) {
            s.err = true;
            return 0;
        }
        val |= static_cast<long>(s.in[s.incnt++]) << s.bitcnt;
        s.bitcnt += 8;
    }
    s.bitbuf = static_cast<uint32_t>(val >> need);
    s.bitcnt -= need;
    return static_cast<int>(val & ((1L << need) - 1));
}

struct Huffman {
    short count[16];
    short symbol[320];
};

int decode(InState& s, const Huffman& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= bits(s, 1);
        if (s.err) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -10;
}

int construct(Huffman& h, const short* length, int n)
{
    for (int len = 0; len <= 15; ++len) h.count[len] = 0;
    for (int sym = 0; sym < n; ++sym) h.count[length[sym]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
    for (int sym = 0; sym < n; ++sym) {
        if (length[sym] != 0) h.symbol[offs[length[sym]]++] = static_cast<short>(sym);
    }
    return left;
}

bool codes(InState& s, const Huffman& lencode, const Huffman& distcode)
{
    static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int symbol;
    do {
        symbol = decode(s, lencode);
        if (symbol < 0 || s.err) return false;
        if (symbol < 256) {
            s.out->push_back(static_cast<uint8_t>(symbol));
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29) return false;
            const int len = lbase[symbol] + bits(s, lext[symbol]);
            const int ds = decode(s, distcode);
            if (ds < 0 || ds >= 30 || s.err) return false;
            const size_t dist = static_cast<size_t>(dbase[ds] + bits(s, dext[ds]));
            if (s.err || dist > s.out->size()) return false;
            for (int i = 0; i < len; ++i) s.out->push_back((*s.out)[s.out->size() - dist]);
        }
    } while (symbol != 256);
    return true;
}
} // namespace

bool inflate_raw(const uint8_t* in, size_t n, Bytes& out)
{
    InState s{in, n, 0, 0, 0, &out};
    int last;
    do {
        last = bits(s, 1);
        const int type = bits(s, 2);
        if (s.err) return false;
        if (type == 0) {
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.incnt + 4 > s.inlen) return false;
            unsigned len = s.in[s.incnt] | s.in[s.incnt + 1] << 8;
            const unsigned nlen = s.in[s.incnt + 2] | s.in[s.incnt + 3] << 8;
            s.incnt += 4;
            if (len != (~nlen & 0xffff) || s.incnt + len > s.inlen) return false;
            out.insert(out.end(), s.in + s.incnt, s.in + s.incnt + len);
            s.incnt += len;
        } else if (type == 1) {
            static Huffman lencode, distcode;
            static bool built = false;
            if (!built) {
                short lengths[288];
                int sym = 0;
                for (; sym < 144; ++sym) lengths[sym] = 8;
                for (; sym < 256; ++sym) lengths[sym] = 9;
                for (; sym < 280; ++sym) lengths[sym] = 7;
                for (; sym < 288; ++sym) lengths[sym] = 8;
                construct(lencode, lengths, 288);
                for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
                construct(distcode, lengths, 30);
                built = true;
            }
            if (!codes(s, lencode, distcode)) return false;
        } else if (type == 2) {
            static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            short lengths[320];
            const int nlen = bits(s, 5) + 257;
            const int ndist = bits(s, 5) + 1;
            const int ncode = bits(s, 4) + 4;
            if (s.err || nlen > 286 || ndist > 30) return false;
            int index = 0;
            for (; index < ncode; ++index) lengths[order[index]] = static_cast<short>(bits(s, 3));
            for (; index < 19; ++index) lengths[order[index]] = 0;
            Huffman lencode, distcode;
            if (construct(lencode, lengths, 19) != 0) return false;
            index = 0;
            while (index < nlen + ndist) {
                int symbol = decode(s, lencode);
                if (symbol < 0 || s.err) return false;
                if (symbol < 16) {
                    lengths[index++] = static_cast<short>(symbol);
                } else {
                    short len = 0;
                    if (symbol == 16) {
                        if (index == 0) return false;
                        len = lengths[index - 1];
                        symbol = 3 + bits(s, 2);
                    } else if (symbol == 17) {
                        symbol = 3 + bits(s, 3);
                    } else {
                        symbol = 11 + bits(s, 7);
                    }
                    if (index + symbol > nlen + ndist) return false;
                    while (symbol--) lengths[index++] = len;
                }
            }
            if (lengths[256] == 0) return false;
            int err = construct(lencode, lengths, nlen);
            if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return false;
            err = construct(distcode, lengths + nlen, ndist);
            if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return false;
            if (!codes(s, lencode, distcode)) return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}

// --- assemble ------------------------------------------------------------------

namespace {

std::vector<FileResult> assemble90(std::vector<Pkt90>& pk, const std::string& outdir, bool keep_partial,
                                   const LogFn& log)
{
    struct File90 {
        int group{0};
        int64_t count{0};
        std::map<int64_t, Counter<Bytes>> blocks;
    };
    struct Key {
        int A;
        std::string name;
        std::optional<uint32_t> size;
        int64_t ntx{0};
        bool operator==(const Key& o) const { return A == o.A && name == o.name && size == o.size && ntx == o.ntx; }
    };
    std::vector<std::pair<Key, File90>> files;
    auto find_file = [&](const Key& k) -> File90* {
        for (auto& f : files) {
            if (f.first == k) return &f.second;
        }
        return nullptr;
    };
    struct Orphan {
        int A;
        std::optional<int64_t> last;
        std::map<int64_t, Counter<Bytes>> blocks;
    };
    std::vector<Orphan> orphans;
    struct Cur {
        Key key;
        std::optional<int64_t> last;
    };
    std::optional<Cur> cur;
    std::vector<std::pair<Key, int64_t>> ntx;
    for (Pkt90& x : pk) {
        if (!x.ok) continue;
        if (x.S == kHEADER) {
            if (x.data.empty()) {
                cur.reset();
                continue;
            }
            const auto h = parse_header(x.data);
            if (!h) continue;
            Key key{x.A, h->name, h->size, 0};
            if (cur && cur->key.A == key.A && cur->key.name == key.name && cur->key.size == key.size && !cur->last) {
                continue;
            }
            if (!h->size) {
                int64_t* n = nullptr;
                for (auto& e : ntx) {
                    if (e.first == key) n = &e.second;
                }
                if (!n) {
                    ntx.push_back({key, 0});
                    n = &ntx.back().second;
                }
                *n += 1;
                key.ntx = *n;
            }
            File90* f = find_file(key);
            if (!f) {
                files.push_back({key, File90{h->group, 0, {}}});
                f = &files.back().second;
            }
            f->count += 1;
            cur = Cur{key, std::nullopt};
            continue;
        }
        const int64_t s = static_cast<int64_t>(x.S) - 0x21;
        std::map<int64_t, Counter<Bytes>>* blocks;
        std::optional<int64_t>* last;
        if (!cur) {
            Orphan* o = orphans.empty() ? nullptr : &orphans.back();
            if (!o || o->A != x.A || std::llabs(unwrap(o->last, s) - *o->last) > 40) {
                orphans.push_back(Orphan{x.A, std::nullopt, {}});
                o = &orphans.back();
            }
            last = &o->last;
            blocks = &o->blocks;
        } else {
            last = &cur->last;
            blocks = &find_file(cur->key)->blocks;
        }
        const int64_t k = unwrap(*last, s);
        *last = k;
        (*blocks)[k].add(x.data);
    }

    std::vector<FileResult> results;
    Counter<std::pair<int, std::string>> used;
    for (auto& [key, f] : files) {
        std::map<int64_t, Bytes> blocks;
        for (const auto& [k, c] : f.blocks) {
            if (k >= 0) blocks[k] = c.most_common()[0].first;
        }
        std::vector<double> lens;
        for (const auto& [k, b] : blocks) {
            if (!is_fill(b)) lens.push_back(static_cast<double>(b.size()));
        }
        const int64_t est = lens.empty() ? 210 : static_cast<int64_t>(np_median(lens));
        Bytes data;
        int64_t missing = 0, k = 0;
        const int64_t last = blocks.empty() ? -1 : blocks.rbegin()->first;
        while (true) {
            if (key.size && static_cast<int64_t>(data.size()) >= *key.size) break;
            auto it = blocks.find(k);
            const bool have = it != blocks.end();
            if (!key.size && (k > last || (have && is_fill(it->second)))) break;
            Bytes b;
            if (!have) {
                if (key.size && k > last) {
                    const int64_t rem = static_cast<int64_t>(*key.size) - static_cast<int64_t>(data.size());
                    const int64_t e = std::max<int64_t>(est, 1);
                    const int64_t rest = (rem + e - 1) / e;
                    missing += rest;
                    k += rest;
                    data.insert(data.end(), static_cast<size_t>(rem), 0);
                    break;
                }
                missing += 1;
                b.assign(static_cast<size_t>(est), 0);
            } else {
                b = it->second;
            }
            data.insert(data.end(), b.begin(), b.end());
            k += 1;
        }
        if (key.size && data.size() > *key.size) data.resize(*key.size);
        const bool complete = missing == 0;
        const auto ukey = std::make_pair(key.A, lower(key.name));
        used.add(ukey);
        std::string fname = safe90(key.name);
        if (used.get(ukey) > 1) {
            const auto [stem, ext] = splitext(fname);
            fname = stem + "_" + std::to_string(used.get(ukey)) + ext;
        }
        std::string path;
        if (complete || keep_partial) {
            const std::string sd = join_path(outdir, strf("kanal_%02X", key.A));
            make_dirs(sd);
            path = join_path(sd, fname + (complete ? "" : ".unvollstaendig"));
            write_file(path, data);
        }
        results.push_back({key.A, fname, key.size ? static_cast<int64_t>(*key.size) : static_cast<int64_t>(data.size()),
                           missing, k, path, f.count});
        if (!complete) continue;
        if (data.size() >= 2 && data[0] == 'P' && data[1] == 'K') {
            if (auto z = check_zip(data)) {
                std::string names;
                for (const auto& n : z->names) names += (names.empty() ? "" : ", ") + n;
                log(strf("  Kanal %02X: %s ist ein ZIP-Archiv mit %s%s", key.A, fname.c_str(), names.c_str(),
                         z->err ? (" (" + *z->err + ")").c_str() : ""));
            }
        }
        const auto tv = text_view(data);
        if (tv && !path.empty()) {
            write_text(path + ".txt", tv->second);
            const std::string base = std::filesystem::u8path(path).filename().u8string();
            log(strf("  Kanal %02X: %s ist Text (xor %02X) -> %s", key.A, fname.c_str(), tv->first, (base + ".txt").c_str()));
        }
    }
    if (keep_partial) {
        for (size_t i = 0; i < orphans.size(); ++i) {
            const auto& o = orphans[i];
            std::map<int64_t, Bytes> blocks;
            for (const auto& [k, c] : o.blocks) blocks[k] = c.most_common()[0].first;
            const std::string sd = join_path(outdir, strf("kanal_%02X", o.A));
            make_dirs(sd);
            const std::string name = strf("ohne_dateikopf_%zu.bin.unvollstaendig", i);
            Bytes data;
            for (int64_t k = blocks.begin()->first; k <= blocks.rbegin()->first; ++k) {
                auto it = blocks.find(k);
                const Bytes b = it != blocks.end() ? it->second : Bytes(210, 0);
                if (is_fill(b)) break;
                data.insert(data.end(), b.begin(), b.end());
            }
            write_file(join_path(sd, name), data);
            log(strf("  Kanal %02X: %zu Datenbloecke ohne Dateikopf -> %s", o.A, blocks.size(), name.c_str()));
        }
    }
    return results;
}

// --- text between the packets ---------------------------------------------------

std::vector<std::pair<size_t, std::string>> zwischentexte(const Bytes& data, const std::vector<Pkt90>& pk,
                                                         size_t min_len = 8)
{
    std::vector<std::pair<size_t, size_t>> spans;
    for (const auto& x : pk) {
        if (x.ok) spans.emplace_back(x.pos, x.pos + x.raw.size());
    }
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<size_t, size_t>> gaps;
    size_t p = 0;
    for (const auto& [a, b] : spans) {
        if (a > p) gaps.emplace_back(p, a);
        p = std::max(p, b);
    }
    gaps.emplace_back(p, data.size());
    std::vector<std::pair<size_t, std::string>> out;
    std::set<std::string> seen;
    auto is_clock = [](const std::string& t, size_t i) {
        // ([0-2]\d):([0-5]\d):([0-5]\d)\r\n
        if (i + 10 > t.size()) return false;
        auto dg = [&](size_t k) { return t[k] >= '0' && t[k] <= '9'; };
        return t[i] >= '0' && t[i] <= '2' && dg(i + 1) && t[i + 2] == ':' && t[i + 3] >= '0' && t[i + 3] <= '5' &&
               dg(i + 4) && t[i + 5] == ':' && t[i + 6] >= '0' && t[i + 6] <= '5' && dg(i + 7) && t[i + 8] == '\r' &&
               t[i + 9] == '\n';
    };
    for (const auto& [a, b] : gaps) {
        std::string seg;
        for (size_t i = a; i < std::min(b, data.size()); ++i) {
            if (data[i] != 0xFF) seg.push_back(static_cast<char>(data[i]));
        }
        size_t i = 0;
        while (i < seg.size()) {
            auto ok = [&](size_t k) {
                const unsigned char c = static_cast<unsigned char>(seg[k]);
                return (c >= 0x20 && c <= 0x7e) || c == '\r' || c == '\n';
            };
            if (!ok(i)) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < seg.size() && ok(j)) ++j;
            if (j - i >= min_len) {
                std::string t = seg.substr(i, j - i);
                std::string rest;
                size_t k = 0;
                while (k < t.size()) {
                    if (is_clock(t, k)) {
                        out.emplace_back(a, "Uhrzeit " + t.substr(k, 2) + ":" + t.substr(k + 3, 2) + ":" + t.substr(k + 6, 2));
                        k += 10;
                    } else {
                        rest.push_back(t[k]);
                        ++k;
                    }
                }
                t = rest;
                size_t fill = 0;
                for (char ch : t) {
                    if (ch == ' ' || ch == '*' || ch == '.' || ch == '\r' || ch == '\n') ++fill;
                }
                if (static_cast<double>(fill) >= 0.8 * static_cast<double>(t.size())) {
                    i = j;
                    continue;
                }
                if (t.find("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos ||
                    t.find("abcdefghijklmnopqrstuvwxyz") != std::string::npos) {
                    t = "Testmuster ABC...xyz";
                }
                t = py_strip(t);
                if (t.size() >= min_len && !seen.count(t)) {
                    seen.insert(t);
                    out.emplace_back(a, t);
                }
            }
            i = j;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

std::vector<std::pair<std::string, Stream>> split_channels(const Capture& cap, const LogFn& log)
{
    const auto slots = cap.slots();
    const size_t n = slots.size();
    struct Best {
        long score;
        long neg_groups;
        std::vector<std::vector<int>> groups;
    };
    std::optional<Best> best;
    for (size_t mask = 0; mask < (size_t{1} << (n > 0 ? n - 1 : 0)); ++mask) {
        std::vector<std::vector<int>> groups;
        std::vector<int> g;
        if (!slots.empty()) g.push_back(slots[0]);
        for (size_t i = 1; i < n; ++i) {
            if ((mask >> (i - 1)) & 1) {
                groups.push_back(g);
                g.clear();
            }
            g.push_back(slots[i]);
        }
        if (!g.empty()) groups.push_back(g);
        long score = 0;
        for (const auto& gr : groups) {
            const std::set<int> sset(gr.begin(), gr.end());
            Bytes data;
            for (const auto& it : cap.items) {
                if (sset.count(it.slot)) data.insert(data.end(), it.bits.bytes.begin(), it.bits.bytes.end());
            }
            for (const auto& x : find_packets90(data, nullptr)) score += x.ok ? 1 : 0;
        }
        const long ng = -static_cast<long>(groups.size());
        if (!best || std::make_pair(score, ng) > std::make_pair(best->score, best->neg_groups)) {
            best = Best{score, ng, groups};
        }
    }
    std::vector<std::pair<std::string, Stream>> out;
    std::string labels;
    for (const auto& g : (best ? best->groups : std::vector<std::vector<int>>{})) {
        std::string label = "zeilen_";
        for (size_t i = 0; i < g.size(); ++i) {
            auto it = cap.slot_names.find(g[i]);
            label += (i ? "+" : "") + (it != cap.slot_names.end() ? it->second : std::to_string(g[i] + 1));
        }
        out.emplace_back(label, cap.stream(std::set<int>(g.begin(), g.end())));
        labels += (labels.empty() ? "" : ", ") + label;
    }
    log("Kanaele: " + labels);
    return out;
}

std::vector<FileResult> run_1990(std::vector<std::pair<std::string, Stream>>& channels, const std::string& outdir,
                                 bool partial, bool skip_services, const LogFn& log)
{
    std::vector<FileResult> res_all;
    for (auto& [label, st] : channels) {
        const std::vector<float>* soft = st.soft.empty() ? nullptr : &st.soft;
        auto pk = find_packets90(st.data, soft);
        size_t n_ok = 0;
        for (const auto& x : pk) n_ok += x.ok ? 1 : 0;
        const int fixed = repair90(pk);
        std::set<int> addrs;
        for (const auto& x : pk) {
            if (x.ok) addrs.insert(x.A);
        }
        std::string a;
        for (int v : addrs) a += (a.empty() ? "" : ", ") + strf("%02X", v);
        log(strf("Kanal %s: %zu Bytes, %zu Pakete, %zu Pruefsumme ok, %d per Soft-Decision repariert, Adressen %s",
                 label.c_str(), st.data.size(), pk.size(), n_ok, fixed, a.empty() ? "-" : a.c_str()));
        auto r = assemble90(pk, outdir, partial, log);
        res_all.insert(res_all.end(), r.begin(), r.end());
        if (!skip_services) {
            const auto tx = zwischentexte(st.data, pk);
            if (!tx.empty()) {
                const std::string sd = join_path(outdir, "dienste");
                make_dirs(sd);
                const std::string fname = "zwischentext_" + label + ".txt";
                std::string text;
                std::string clocks;
                for (const auto& [p, t] : tx) {
                    std::string e;
                    for (char c : t) {
                        if (c == '\r') e += "\\r";
                        else if (c == '\n') e += "\\n";
                        else e.push_back(c);
                    }
                    text += strf("%9zu  %s\n", p, e.c_str());
                    if (t.rfind("Uhrzeit ", 0) == 0) clocks += (clocks.empty() ? "" : ", ") + t.substr(8);
                }
                write_text(join_path(sd, fname), text);
                log(strf("  Kanal %s: %zu Zwischentexte -> %s%s", label.c_str(), tx.size(),
                         join_path("dienste", fname).c_str(), clocks.empty() ? "" : ("; Uhrzeit " + clocks).c_str()));
            }
        }
    }
    return res_all;
}

} // namespace videodat
