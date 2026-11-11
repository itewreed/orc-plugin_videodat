/*
 * File:        vd_decoder.h
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Top level of the VideoDat decoder (main() of
 *              videodat_decoder.py)
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include "vd_signal.h"

namespace videodat {

struct DecodeOptions {
    std::string outdir{"videodat1993_out"};
    std::string variant{"auto"};
    std::vector<int> lines;
    double scan_seconds{30.0};
    bool no_sync{false};
    bool no_eq{false};
    bool partial{false};
    bool keine_dienste{false};
    bool dump_other{false};
    bool decrypt_services{false};
    bool decrypt_files{false};
    std::string save_bytes;  // "" = off
};

struct DecodeSummary {
    bool cancelled{false};
    size_t files{0};
    size_t complete{0};
    std::string variant;
};

// Decodes fields from |src|. |progress| gets (fields done, total fields) of
// the slicing pass.
DecodeSummary decode_fields(const FieldSource& src, const DecodeOptions& opt, const LogFn& log,
                            const std::function<void(size_t, size_t)>& progress = {},
                            const std::atomic<bool>* cancel = nullptr);

// Decodes an already sliced byte stream (--bytes).
DecodeSummary decode_bytes(const Bytes& data, const DecodeOptions& opt, const LogFn& log);

} // namespace videodat
