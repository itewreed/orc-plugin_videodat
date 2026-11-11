/*
 * File:        vd_prng.h
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     The 15-bit PRNG of stream 01 type 05 and type FF packets
 *              (port of videodat_prng.py)
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include "vd_json.h"
#include "vd_util.h"

namespace videodat {

struct PrngResult {
    bool valid{true};       // false: ValueError in Python
    std::string error;      // its message
    bool matches_prng{false};
    Json record;            // the result dict
};

PrngResult identify_type05(const Bytes& payload);
PrngResult analyse_type05(const Bytes& payload);
bool matches_type_ff(const Bytes& payload);

} // namespace videodat
