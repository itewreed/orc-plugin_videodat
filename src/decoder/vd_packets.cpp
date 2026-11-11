/*
 * File:        vd_packets.cpp
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Packet layer of the 1993 variant: find, descramble, repair
 *              and correct permutation errors
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "vd_packets.h"

#include "vd_crypto.h"
#include "vd_prng.h"

#include <set>

namespace videodat {

namespace {
constexpr uint8_t kSync0 = 0x56;
constexpr uint8_t kSync1 = 0xAE;

// Insertion-ordered grouping (Python defaultdict(list) iteration order).
template <typename K, typename V>
class OrderedGroups {
public:
    std::vector<V>& operator[](const K& k)
    {
        auto it = index_.find(k);
        if (it == index_.end()) {
            index_.emplace(k, groups_.size());
            groups_.emplace_back(k, std::vector<V>{});
            return groups_.back().second;
        }
        return groups_[it->second].second;
    }
    std::vector<std::pair<K, std::vector<V>>>& groups() { return groups_; }

private:
    std::map<K, size_t> index_;
    std::vector<std::pair<K, std::vector<V>>> groups_;
};

Bytes slice(const Bytes& b, size_t from, size_t to)
{
    from = std::min(from, b.size());
    to = std::min(std::max(to, from), b.size());
    return Bytes(b.begin() + static_cast<long>(from), b.begin() + static_cast<long>(to));
}

struct Unstuffed {
    Bytes raw;
    std::vector<size_t> indexes;
    size_t end{0};
};

Unstuffed unstuff_packet(const Bytes& stream, size_t start)
{
    Unstuffed u;
    u.raw = slice(stream, start, start + 2);
    u.indexes = {start, start + 1};
    size_t pos = start + 2;
    long length = -1;
    while (pos < stream.size() && (length < 0 || static_cast<long>(u.raw.size()) < length)) {
        u.raw.push_back(stream[pos]);
        u.indexes.push_back(pos);
        ++pos;
        if (u.raw.size() == 4) {
            length = (1L << (u.raw[3] & 15)) + 11;
        }
        const size_t r = u.raw.size();
        if (r > 3 && u.raw[r - 2] == kSync0 && u.raw[r - 1] == kSync1 && pos < stream.size() &&
            stream[pos] == 255) {
            ++pos;
        }
    }
    u.end = pos;
    return u;
}

Bytes soft_to_raw(const std::vector<float>& soft)
{
    Bytes out(soft.size() / 8);
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned v = 0;
        for (int b = 0; b < 8; ++b) {
            if (soft[8 * i + b] >= 0.0f) v |= 1u << b;
        }
        out[i] = static_cast<uint8_t>(v ^ 0xFF);
    }
    return out;
}

// Soft decision over the copies of one packet (videodat_decoder.soft_fix).
std::optional<Bytes> soft_fix(const std::vector<Pkt*>& all, size_t maxvar = 12)
{
    std::vector<Pkt*> xs;
    for (Pkt* x : all) {
        if (x->has_soft && x->soft.size() == 8 * x->raw.size()) xs.push_back(x);
    }
    if (xs.empty()) return std::nullopt;
    const size_t nb = xs[0]->soft.size();
    std::vector<float> m(nb, 0.0f);
    for (Pkt* x : xs) {
        for (size_t i = 0; i < nb; ++i) m[i] = m[i] + x->soft[i];
    }
    Bytes raw = soft_to_raw(m);
    for (size_t i = 0; i < 9 && i < raw.size(); ++i) raw[i] = xs[0]->raw[i];
    if (sum_ok(raw)) return raw;
    if (xs.size() < 2) return std::nullopt;
    std::vector<size_t> dis;
    for (size_t b = 72; b < nb; ++b) {
        bool lo = true, hi = false;
        for (Pkt* x : xs) {
            const bool h = x->soft[b] >= 0.0f;
            lo = lo && h;
            hi = hi || h;
        }
        if (lo != hi) dis.push_back(b);
    }
    if (dis.empty() || dis.size() > maxvar) return std::nullopt;
    std::optional<Bytes> sol;
    const size_t nd = dis.size();
    // itertools.combinations order for k = 1..nd
    for (size_t k = 1; k <= nd; ++k) {
        std::vector<size_t> c(k);
        for (size_t i = 0; i < k; ++i) c[i] = i;
        while (true) {
            Bytes r = raw;
            for (size_t i = 0; i < k; ++i) {
                const size_t bit = dis[c[i]];
                r[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
            }
            if (sum_ok(r)) {
                if (sol && *sol != r) return std::nullopt;
                sol = r;
            }
            // next combination
            long i = static_cast<long>(k) - 1;
            while (i >= 0 && c[static_cast<size_t>(i)] == static_cast<size_t>(i) + nd - k) --i;
            if (i < 0) break;
            ++c[static_cast<size_t>(i)];
            for (size_t j = static_cast<size_t>(i) + 1; j < k; ++j) c[j] = c[j - 1] + 1;
        }
    }
    return sol;
}

std::optional<Bytes> repair_pair(const Bytes& a, const Bytes& b)
{
    const size_t L = a.size();
    const size_t n = size_t{1} << (a[3] & 15);
    const int64_t k = a[8] ^ 0x58;
    const int64_t base = a[2] + a[3] + a[4];
    std::vector<int64_t> ca(n + 1, 0), cb(n + 1, 0);
    for (size_t i = 0; i < n; ++i) {
        ca[i + 1] = ca[i] + (a[9 + i] ^ k);
        cb[i + 1] = cb[i] + (b[9 + i] ^ k);
    }
    const size_t lo = 9 + n, hi = 10 + n;
    std::set<Bytes> sols;
    for (size_t s = 9; s < L; ++s) {
        const size_t j = std::min(s - 9, n);
        const int64_t tot = base + ca[j] + (cb[n] - cb[j]);
        const int64_t c = (lo < s ? a[lo] : b[lo]) | (hi < s ? a[hi] : b[hi]) << 8;
        if ((tot & 0xFFFF) == c) {
            Bytes r(a.begin(), a.begin() + static_cast<long>(s));
            r.insert(r.end(), b.begin() + static_cast<long>(s), b.end());
            if (sum_ok(r)) sols.insert(r);
        }
    }
    if (L > 70) {
        std::vector<int64_t> cas(n + 1, 0);
        for (size_t i = 0; i < n; ++i) {
            const size_t p = 9 + i;
            const int64_t v = p >= 60 ? a[p - 60] : 0;
            cas[i + 1] = cas[i] + (v ^ k);
        }
        for (size_t s = 9; s + 60 < L; ++s) {
            const size_t j1 = std::min(s - 9, n);
            const size_t j2 = std::min(s + 60 - 9, n);
            const int64_t tot = base + ca[j1] + (cb[j2] - cb[j1]) + (cas[n] - cas[j2]);
            auto pick = [&](size_t p) -> int64_t {
                if (p < s) return a[p];
                if (p < s + 60) return b[p];
                return a[p - 60];
            };
            if ((tot & 0xFFFF) == (pick(lo) | pick(hi) << 8)) {
                Bytes r(a.begin(), a.begin() + static_cast<long>(s));
                r.insert(r.end(), b.begin() + static_cast<long>(s), b.begin() + static_cast<long>(s + 60));
                r.insert(r.end(), a.begin() + static_cast<long>(s), a.begin() + static_cast<long>(L - 60));
                if (sum_ok(r)) sols.insert(r);
            }
        }
    }
    if (sols.size() == 1) return *sols.begin();
    return std::nullopt;
}

bool in(uint8_t t, std::initializer_list<int> v)
{
    for (int x : v) {
        if (t == x) return true;
    }
    return false;
}

bool typ_plausibel(const Pkt& x, const Bytes& pay)
{
    const uint8_t t = pay[0];
    const size_t n = pay.size();
    if (x.A == 0x00) return in(t, {0x00, 0x01, 0x02, 0x0B, 0x80, 0x81, 0x82});
    if (x.A == 0x01 && !(x.B & 0x80) && in(t, {0x05, 0x06})) return (t == 0x06) == (n == 8);
    if (x.A == 0x03 && !(x.B & 0x80) && in(t, {0x07, 0x08, 0x0A})) return true;
    if (n == 8) return in(t, {0x01, 0x03, 0x04, 0xFF});
    if (n == 256) return in(t, {0x02, 0x04, 0x0C});
    if (x.B & 0x80) return in(t, {0x00, 0x01, 0x02, 0x03, 0x04, 0x09, 0x0C});
    return in(t, {0x02, 0x03, 0x04, 0x0C});
}

bool inhalt_bestaetigt(const Pkt& x, const Bytes& pay)
{
    const uint8_t t = pay[0];
    const size_t n = pay.size();
    if (x.A == 0x01 && !(x.B & 0x80)) {
        if (t == 0x05 && n != 8) {
            return (n == 16 || n == 32 || n == 64 || n == 128 || n == 256) &&
                   identify_type05(pay).matches_prng;
        }
        if (t == 0x06 && n == 8) {
            if (!DateTime::make(1900 + pay[1], pay[2], pay[3], pay[4], pay[5], pay[6])) return false;
            return 88 <= pay[1] && pay[1] <= 99;
        }
    }
    if (x.A == 0x03 && !(x.B & 0x80) && in(t, {0x07, 0x08, 0x0A})) {
        const auto rec = decrypt_service_payload(3, pay);
        return rec.valid && rec.checksum_ok;
    }
    if (n == 8 && t == 0xFF) return matches_type_ff(pay);
    return false;
}

std::set<Bytes> lesarten(const Bytes& raw)
{
    const size_t n = size_t{1} << (raw[3] & 15);
    std::set<Bytes> out;
    for (int d = 0; d < 8; ++d) {
        for (int e = 0; e < 2; ++e) {
            for (int f = 0; f < 4; ++f) {
                out.insert(entwuerfeln(raw, qperm(static_cast<uint8_t>(d), static_cast<uint8_t>(e << 2),
                                                  static_cast<uint8_t>(f << 1)),
                                       n));
            }
        }
    }
    return out;
}

} // namespace

std::array<int, 8> qperm(uint8_t D, uint8_t E, uint8_t F)
{
    const int e = (E >> 2) & 1;
    const int d = D & 7;
    const int f = (F >> 1) & 3;
    const int m = (2 * f + 1 + 6 * e) % 8;
    std::array<int, 8> q{};
    for (int c = 0; c < 8; ++c) q[static_cast<size_t>(c)] = (((c ^ e) + (d ^ e) + 1) * m) % 8;
    return q;
}

Bytes entwuerfeln(const Bytes& raw, const std::array<int, 8>& q, size_t n)
{
    const uint8_t k = raw[8] ^ 0x58;
    Bytes pay(n, 0);
    if (n < 8) {
        for (size_t i = 0; i < n; ++i) pay[i] = (9 + i < raw.size() ? raw[9 + i] : 0) ^ k;
        // Python: bytearray(b ^ k for b in raw[9:9 + n]) -> may be shorter
        if (raw.size() < 9 + n) pay.resize(raw.size() > 9 ? raw.size() - 9 : 0);
        return pay;
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t g = i / 8, c = i % 8;
        const size_t pos = 9 + 8 * g + static_cast<size_t>((q[c] - 1 + 8) % 8);
        pay[i] = pos < 9 + n ? static_cast<uint8_t>(raw[pos] ^ k) : 0;
    }
    return pay;
}

Bytes descramble(const Bytes& raw, bool& ok)
{
    ok = false;
    const uint8_t A = raw[2], B = raw[3], C = raw[4], D = raw[5], E = raw[6], F = raw[7];
    const size_t n = size_t{1} << (B & 15);
    if (raw.size() < n + 11) return {};
    Bytes pay = entwuerfeln(raw, qperm(D, E, F), n);
    const unsigned ck = raw[9 + n] | raw[10 + n] << 8;
    uint64_t s = static_cast<uint64_t>(A) + B + C;
    for (uint8_t b : pay) s += b;
    ok = (s & 0xFFFF) == ck;
    return pay;
}

bool sum_ok(const Bytes& raw)
{
    if (raw.size() < 5) return false;
    const size_t n = size_t{1} << (raw[3] & 15);
    if (raw.size() < n + 11) return false;
    const uint8_t k = raw[8] ^ 0x58;
    uint64_t s = static_cast<uint64_t>(raw[2]) + raw[3] + raw[4];
    for (size_t i = 9; i < 9 + n; ++i) s += raw[i] ^ k;
    return (s & 0xFFFF) == (raw[9 + n] | raw[10 + n] << 8);
}

namespace {
std::vector<Pkt> find_packets_impl(const Bytes& stream, const std::vector<float>* soft)
{
    std::vector<Pkt> pk;
    size_t valid_end = 0;
    const size_t len = stream.size();
    for (size_t p = 0; p + 1 < len; ++p) {
        if (stream[p] != kSync0 || stream[p + 1] != kSync1) continue;
        if (p < valid_end) continue;
        if (p + 9 > len) break;
        const uint8_t B = stream[p + 3];
        if ((B & 15) > kMaxPayBits) continue;
        const size_t L = (size_t{1} << (B & 15)) + 11;
        if (p + L > len) continue;
        Pkt x;
        x.pos = p;
        x.raw = slice(stream, p, p + L);
        x.A = x.raw[2];
        x.B = x.raw[3];
        x.C = x.raw[4];
        x.pay = descramble(x.raw, x.ok);
        if (soft) {
            x.has_soft = true;
            x.soft.assign(soft->begin() + static_cast<long>(8 * p),
                          soft->begin() + static_cast<long>(std::min(soft->size(), 8 * (p + L))));
        }
        x.how = x.ok ? "ok" : "";
        if (!x.ok) {
            Unstuffed u = unstuff_packet(stream, p);
            if (u.end - p > u.raw.size()) {
                bool ok2 = false;
                Bytes pay2 = descramble(u.raw, ok2);
                if (ok2) {
                    x.raw = u.raw;
                    x.pay = pay2;
                    x.ok = true;
                    x.how = "unstuffed";
                    x.A = u.raw[2];
                    x.B = u.raw[3];
                    x.C = u.raw[4];
                    if (soft) {
                        x.soft.clear();
                        for (size_t idx : u.indexes) {
                            for (size_t b = 0; b < 8; ++b) x.soft.push_back((*soft)[idx * 8 + b]);
                        }
                    }
                    valid_end = u.end;
                }
            }
        } else {
            valid_end = p + L;
        }
        pk.push_back(std::move(x));
    }
    return pk;
}
} // namespace

std::vector<Pkt> find_packets(const Stream& stream)
{
    return find_packets_impl(stream.data, stream.soft.empty() && !stream.data.empty() ? nullptr : &stream.soft);
}

std::vector<Pkt> find_packets(const Bytes& data)
{
    return find_packets_impl(data, nullptr);
}

std::map<std::string, int> repair(std::vector<Pkt>& pk, size_t max_diff)
{
    OrderedGroups<Bytes, Pkt*> groups;
    for (Pkt& x : pk) groups[slice(x.raw, 2, 9)].push_back(&x);
    std::map<std::string, int> fixed;
    for (auto& [key, all] : groups.groups()) {
        bool any_ok = false;
        for (Pkt* x : all) any_ok = any_ok || x->ok;
        if (any_ok) continue;
        size_t L = 0;
        for (Pkt* x : all) L = std::max(L, x->raw.size());
        std::vector<Pkt*> xs;
        for (Pkt* x : all) {
            if (x->raw.size() == L) xs.push_back(x);
        }
        std::string how = "soft";
        std::optional<Bytes> good = soft_fix(xs);
        if (!good && xs.size() < 2) continue;
        if (!good) how = "kopien";
        if (!good) {
            // 1) single byte errors: combinations of the differing bytes
            std::vector<std::vector<uint8_t>> cols(L);
            for (size_t i = 0; i < L; ++i) {
                std::set<uint8_t> s;
                for (Pkt* x : xs) s.insert(x->raw[i]);
                cols[i].assign(s.begin(), s.end());
            }
            std::vector<size_t> var;
            bool all_two = true;
            for (size_t i = 0; i < L; ++i) {
                if (cols[i].size() > 1) {
                    var.push_back(i);
                    all_two = all_two && cols[i].size() == 2;
                }
            }
            if (!var.empty() && var.size() <= max_diff && all_two) {
                const size_t n = size_t{1} << (xs[0]->raw[3] & 15);
                const int64_t k = xs[0]->raw[8] ^ 0x58;
                Bytes base = xs[0]->raw;
                for (size_t i : var) base[i] = cols[i][0];
                int64_t base_tot = base[2] + base[3] + base[4];
                for (size_t i = 9; i < 9 + n; ++i) base_tot += base[i] ^ k;
                const size_t nv = var.size();
                long hit = -1;
                size_t nhits = 0;
                for (size_t combo = 0; combo < (size_t{1} << nv); ++combo) {
                    int64_t tot = base_tot;
                    int64_t cklo = base[9 + n], ckhi = base[10 + n];
                    for (size_t j = 0; j < nv; ++j) {
                        const size_t i = var[j];
                        const int64_t val = (combo >> j & 1) ? cols[i][1] : cols[i][0];
                        if (i >= 9 && i < 9 + n) {
                            tot += (val ^ k) - (base[i] ^ k);
                        } else if (i == 9 + n) {
                            cklo = val;
                        } else if (i == 10 + n) {
                            ckhi = val;
                        }
                    }
                    if ((tot & 0xFFFF) == (cklo | ckhi << 8)) {
                        ++nhits;
                        hit = static_cast<long>(combo);
                    }
                }
                if (nhits == 1) {
                    for (size_t j = 0; j < nv; ++j) {
                        const size_t i = var[j];
                        base[i] = (static_cast<size_t>(hit) >> j & 1) ? cols[i][1] : cols[i][0];
                    }
                    good = base;
                }
            }
        }
        if (!good) {
            for (Pkt* a : xs) {
                for (Pkt* b : xs) {
                    if (a == b || (good && !good->empty())) continue;
                    good = repair_pair(a->raw, b->raw);
                }
            }
        }
        if (good && !good->empty()) {
            bool ok = false;
            Bytes pay = descramble(*good, ok);
            if (!ok) continue;
            for (Pkt* x : xs) {
                x->raw = *good;
                x->pay = pay;
                x->ok = true;
                x->how = how;
            }
            fixed[how] += 1;
        }
    }
    return fixed;
}

std::map<std::string, int> korrigiere_permutation(std::vector<Pkt>& pk)
{
    std::map<std::string, int> stat;
    std::vector<Pkt*> ok;
    for (Pkt& x : pk) {
        if (x.ok && x.pay.size() >= 8) ok.push_back(&x);
    }
    OrderedGroups<std::pair<Bytes, Bytes>, Pkt*> gruppen;
    for (Pkt* x : ok) gruppen[{slice(x->raw, 2, 5), slice(x->raw, 9, x->raw.size())}].push_back(x);
    for (auto& [key, xs] : gruppen.groups()) {
        std::set<Bytes> distinct;
        for (Pkt* x : xs) distinct.insert(x->pay);
        if (distinct.size() < 2) continue;
        Counter<Bytes> stimmen;
        for (Pkt* x : xs) {
            if (typ_plausibel(*x, x->pay)) stimmen.add(x->pay);
        }
        const auto best = stimmen.most_common();
        if (best.empty() || (best.size() > 1 && best[0].second == best[1].second)) continue;
        for (Pkt* x : xs) {
            if (x->pay != best[0].first) {
                x->pay = best[0].first;
                x->how = "permutation";
                stat["kopien"] += 1;
            }
        }
    }
    for (Pkt* x : ok) {
        if (typ_plausibel(*x, x->pay)) continue;
        std::vector<Bytes> belegt;
        for (const Bytes& p : lesarten(x->raw)) {
            if (inhalt_bestaetigt(*x, p)) belegt.push_back(p);
        }
        if (belegt.size() == 1) {
            x->pay = belegt[0];
            x->how = "permutation";
            stat["inhalt"] += 1;
        } else if (x->how == "kopien") {
            x->ok = false;
            stat["verworfen"] += 1;
        }
    }
    return stat;
}

} // namespace videodat
