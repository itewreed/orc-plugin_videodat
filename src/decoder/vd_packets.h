/*
 * File:        vd_packets.h
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Byte stream and the packet layer of the 1993 variant
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 *
 * Packet format (1993): 56 AE sync, A (stream), B (bits 0-3: payload 2^n,
 * bit 7: subchannel), C (counter), D E F (permutation), S (key), scrambled
 * payload, 16-bit sum. See videodat_decoder.py for the full description.
 */

#pragma once

#include "vd_util.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace videodat {

// Decoded bytes plus 8 bit confidences per byte (sample - threshold, scaled
// by the swing; > 0 means "bright"). soft may be empty for a plain byte
// stream (--bytes input).
struct Stream {
    Bytes data;
    std::vector<float> soft;
    bool has_soft() const { return !soft.empty() || data.empty(); }
};

// One sliced data line: its bytes and 8 confidences per byte.
struct LineBits {
    Bytes bytes;
    std::vector<float> soft;
};

struct Pkt {
    size_t pos{0};
    uint8_t A{0}, B{0}, C{0};
    Bytes raw;
    Bytes pay;
    bool ok{false};
    std::vector<float> soft;  // empty: none
    bool has_soft{false};
    std::string how;  // "ok", "unstuffed", "soft", "kopien", "permutation", ""
};

constexpr int kMaxPayBits = 8;
constexpr uint32_t kMaxFile = 64u * 1024u * 1024u;

std::array<int, 8> qperm(uint8_t D, uint8_t E, uint8_t F);
Bytes entwuerfeln(const Bytes& raw, const std::array<int, 8>& q, size_t n);
// raw = whole packet -> payload; ok = checksum matches.
Bytes descramble(const Bytes& raw, bool& ok);
bool sum_ok(const Bytes& raw);

std::vector<Pkt> find_packets(const Stream& stream);
std::vector<Pkt> find_packets(const Bytes& data);

// Repairs damaged packets from their copies. Returns counts by method
// ("soft", "kopien").
std::map<std::string, int> repair(std::vector<Pkt>& pk, size_t max_diff = 14);

// Corrects flipped permutation bits. Returns counts ("kopien", "inhalt",
// "verworfen").
std::map<std::string, int> korrigiere_permutation(std::vector<Pkt>& pk);

} // namespace videodat
