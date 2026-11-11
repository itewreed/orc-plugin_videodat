/*
 * File:        vd_prng.cpp
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     The 15-bit PRNG of stream 01 type 05 and type FF packets
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "vd_prng.h"

#include <set>

namespace videodat {

namespace {
constexpr uint64_t kMul = 0x015A4E35;
constexpr uint64_t kMask = 0x7FFFFFFF;
constexpr size_t kNearMinLength = 64;

uint64_t inverse()
{
    // Newton iteration for the inverse modulo 2^31 (kMul is odd).
    uint64_t x = kMul;
    for (int i = 0; i < 6; ++i) x = (x * (2 - kMul * x)) & kMask;
    return x & kMask;
}
const uint64_t kInverse = inverse();

struct LowIndex {
    std::vector<uint32_t> residues;
    std::vector<uint32_t> lows;
    LowIndex()
    {
        std::vector<std::pair<uint32_t, uint32_t>> pairs;
        pairs.reserve(65536);
        for (uint32_t low = 0; low < 65536; ++low) {
            pairs.emplace_back(static_cast<uint32_t>((kMul * low) & kMask), low);
        }
        std::sort(pairs.begin(), pairs.end());
        for (const auto& p : pairs) {
            residues.push_back(p.first);
            lows.push_back(p.second);
        }
    }
};

const LowIndex& low_index()
{
    static const LowIndex idx;
    return idx;
}

std::vector<uint32_t> candidate_low_bits(uint32_t first, uint32_t second)
{
    const auto& ix = low_index();
    const uint64_t base = (kMul * (static_cast<uint64_t>(first) << 16) + 1) & kMask;
    const uint64_t start = ((static_cast<uint64_t>(second) << 16) - base) & kMask;
    const uint64_t end = (start + 65535) & kMask;
    std::vector<std::pair<uint64_t, uint64_t>> intervals;
    if (start <= end) {
        intervals = {{start, end}};
    } else {
        intervals = {{start, kMask}, {0, end}};
    }
    std::vector<uint32_t> out;
    for (const auto& [lo, hi] : intervals) {
        auto a = std::lower_bound(ix.residues.begin(), ix.residues.end(), lo);
        auto b = std::upper_bound(ix.residues.begin(), ix.residues.end(), hi);
        for (auto it = a; it < b; ++it) out.push_back(ix.lows[static_cast<size_t>(it - ix.residues.begin())]);
    }
    return out;
}

bool valid_length(size_t n)
{
    return n == 16 || n == 32 || n == 64 || n == 128 || n == 256;
}

std::vector<uint32_t> words_of(const Bytes& p)
{
    std::vector<uint32_t> w;
    for (size_t i = 2; i + 1 < p.size(); i += 2) w.push_back(p[i] | p[i + 1] << 8);
    return w;
}

Bytes generate_type05(uint64_t first_state, size_t length)
{
    uint64_t state = first_state;
    Bytes out;
    for (size_t i = 0; i < length / 2; ++i) {
        const uint32_t v = static_cast<uint32_t>(state >> 16);
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        state = (kMul * state + 1) & kMask;
    }
    out[0] = 5;
    return out;
}
} // namespace

PrngResult identify_type05(const Bytes& payload)
{
    PrngResult r;
    if (!valid_length(payload.size()) || payload[0] != 5) {
        r.valid = false;
        r.error = "expected a type 05 payload of 16, 32, 64, 128, 256 bytes";
        return r;
    }
    const auto words = words_of(payload);
    r.record = Json::object();
    r.record["kind"] = "unknown";
    r.record["matches_prng"] = false;
    r.record["length"] = payload.size();
    if (payload[1] & 0x80) return r;
    for (uint32_t w : words) {
        if (w & 0x8000) return r;
    }
    std::vector<std::pair<uint64_t, uint64_t>> matches;
    const uint64_t high = static_cast<uint64_t>(words[0]) << 16;
    for (uint32_t low : candidate_low_bits(words[0], words[1])) {
        const uint64_t state1 = high | low;
        uint64_t state = (kMul * state1 + 1) & kMask;
        if ((state >> 16) != words[1]) continue;
        const uint64_t first_state = ((state1 - 1) * kInverse) & kMask;
        if ((first_state >> 24) != payload[1]) continue;
        bool all = true;
        for (size_t i = 2; i < words.size(); ++i) {
            state = (kMul * state + 1) & kMask;
            if ((state >> 16) != words[i]) {
                all = false;
                break;
            }
        }
        if (all) matches.emplace_back(first_state, (kMul * state + 1) & kMask);
    }
    if (matches.size() != 1) return r;
    const auto [first_state, next_state] = matches[0];
    r.matches_prng = true;
    r.record["kind"] = "pseudorandom_sequence";
    r.record["matches_prng"] = true;
    r.record["multiplier"] = kMul;
    r.record["increment"] = 1;
    r.record["state_modulus"] = uint64_t{1} << 31;
    r.record["first_state_mod_2_31"] = first_state;
    r.record["next_state_mod_2_31"] = next_state;
    r.record["overwritten_low_byte"] = (first_state >> 16) & 255;
    r.record["observed_bytes_matched"] = payload.size();
    return r;
}

bool matches_type_ff(const Bytes& payload)
{
    if (payload.size() != 8 || payload[0] != 0xFF) return false;
    const uint32_t first = payload[2] | payload[3] << 8;
    const uint32_t second = payload[4] | payload[5] << 8;
    const uint32_t third = payload[6] | payload[7] << 8;
    if ((first | second | third) & 0x8000) return false;
    for (uint32_t low : candidate_low_bits(first, second)) {
        const uint64_t state = (kMul * ((static_cast<uint64_t>(first) << 16) | low) + 1) & kMask;
        if ((state >> 16) == second && (((kMul * state + 1) & kMask) >> 16) == third) return true;
    }
    return false;
}

PrngResult analyse_type05(const Bytes& payload)
{
    PrngResult result = identify_type05(payload);
    if (!result.valid || result.matches_prng) return result;
    std::vector<std::pair<std::vector<int>, PrngResult>> perms;
    for (int mult : {3, 5, 7}) {
        std::vector<int> order;
        for (int c = 0; c < 8; ++c) order.push_back((mult * c) % 8);
        Bytes cand;
        for (size_t base = 0; base < payload.size(); base += 8) {
            for (int c : order) cand.push_back(payload[base + static_cast<size_t>(c)]);
        }
        PrngResult found = identify_type05(cand);
        if (found.valid && found.matches_prng) perms.emplace_back(order, found);
    }
    if (perms.size() == 1) {
        Json order = Json::array();
        for (int c : perms[0].first) order.push_back(c);
        result.record["kind"] = "prng_permutation_mismatch";
        result.record["permutation"] = order;
        result.record["candidate_first_state_mod_2_31"] =
            *perms[0].second.record.find("first_state_mod_2_31");
        result.record["sum_preserved"] = true;
        return result;
    }
    if (payload.size() < kNearMinLength) return result;
    const auto words = words_of(payload);
    std::set<uint64_t> seeds;
    for (size_t i = 0; i + 1 < words.size(); ++i) {
        const uint32_t first = words[i], second = words[i + 1];
        if (first > 32767 || second > 32767) continue;
        for (uint32_t low : candidate_low_bits(first, second)) {
            uint64_t state = (static_cast<uint64_t>(first) << 16) | low;
            for (size_t k = 0; k < i + 1; ++k) state = ((state - 1) * kInverse) & kMask;
            seeds.insert(state);
        }
    }
    struct Cand {
        uint64_t seed;
        Bytes predicted;
        Json differences;
    };
    std::vector<Cand> candidates;
    for (uint64_t seed : seeds) {
        Bytes predicted = generate_type05(seed, payload.size());
        Json diffs = Json::array();
        size_t nd = 0;
        for (size_t i = 0; i < payload.size(); ++i) {
            if (payload[i] != predicted[i]) {
                Json d = Json::object();
                d["offset"] = i;
                d["observed"] = payload[i];
                d["predicted"] = predicted[i];
                diffs.push_back(d);
                ++nd;
            }
        }
        if (nd >= 1 && nd <= 4) candidates.push_back({seed, predicted, diffs});
    }
    if (candidates.size() == 1) {
        const auto& c = candidates[0];
        long so = 0, sp = 0;
        for (uint8_t b : payload) so += b;
        for (uint8_t b : c.predicted) sp += b;
        result.record["kind"] = "prng_byte_mismatch";
        result.record["candidate_first_state_mod_2_31"] = c.seed;
        result.record["differences"] = c.differences;
        result.record["observed_sum"] = so;
        result.record["predicted_sum"] = sp;
        result.record["sum_preserved"] = so == sp;
    }
    return result;
}

} // namespace videodat
