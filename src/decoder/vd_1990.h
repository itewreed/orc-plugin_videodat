/*
 * File:        vd_1990.h
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Packet and file layer of the 1990 variant (port of
 *              videodat1990.py)
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include "vd_assemble.h"
#include "vd_signal.h"

namespace videodat {

// Groups the data line slots into channels; returns (label, stream).
std::vector<std::pair<std::string, Stream>> split_channels(const Capture& cap, const LogFn& log);

std::vector<FileResult> run_1990(std::vector<std::pair<std::string, Stream>>& channels, const std::string& outdir,
                                 bool partial, bool skip_services, const LogFn& log);

// Raw DEFLATE decoder (for checking ZIP archives). Returns false on error.
bool inflate_raw(const uint8_t* in, size_t n, Bytes& out);

} // namespace videodat
