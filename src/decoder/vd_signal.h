/*
 * File:        vd_signal.h
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Signal layer: data line detection, H-sync reference,
 *              equalizer and bit slicing of TBC/CVBS fields
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include "vd_packets.h"
#include "vd_util.h"

#include <atomic>
#include <set>

namespace videodat {

// Fields of a time-base-corrected PAL recording, 4 fsc, samples in the 16-bit
// TBC domain (10-bit value << 6). Only the first lines of each field are read.
class FieldSource {
public:
    virtual ~FieldSource() = default;
    virtual size_t field_count() const = 0;
    virtual size_t width() const = 0;
    // Lines per field the source can deliver (the decoder reads at most 40).
    virtual size_t field_lines() const = 0;
    // true: field |fi| is a first field (isFirstField of the .tbc.json).
    virtual bool is_first_field(size_t fi) const = 0;
    // Copies line |li| (0-based within the field) of field |fi| to |out|
    // (width() samples).
    virtual void read_line(size_t fi, size_t li, double* out) const = 0;
    // Whether the field order came from metadata (else: alternating guess).
    virtual bool has_field_order() const { return true; }
};

struct Fmt {
    double fs{0};
    std::string variant{"1993"};
    double spb{0};
    int nbytes{10};
    int nbits{100};
    int frame_min{18};
    int lo_rng[2]{0, 0};
    int search[2]{0, 0};
    double min_swing{0};

    static Fmt tbc_pal(const std::string& variant = "1993");
};

struct Capture {
    Fmt fmt;
    struct Item {
        size_t field;
        int slot;
        LineBits bits;
    };
    std::vector<Item> items;
    std::map<int, std::string> slot_names;

    std::vector<int> slots() const;
    // Byte stream of all slots (empty set) or the given ones.
    Stream stream(const std::set<int>& only = {}) const;
};

struct SignalOptions {
    std::vector<int> lines;      // frame lines; empty = automatic
    bool use_sync{true};
    double scan_seconds{30.0};
    bool use_eq{true};
    std::string variant{"auto"};  // auto, 1993, 1990
};

// read_tbc(): reads all fields into a Capture. Progress is reported as
// (fields done, total fields) for the final decoding pass. Returns false if
// cancelled.
bool read_tbc(const FieldSource& src, const SignalOptions& opt, const LogFn& log, Capture& cap,
              const std::function<void(size_t, size_t)>& progress = {},
              const std::atomic<bool>* cancel = nullptr);

} // namespace videodat
