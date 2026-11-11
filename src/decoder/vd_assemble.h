/*
 * File:        vd_assemble.h
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     File assembly of the 1993 variant and the service report
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include "vd_crypto.h"
#include "vd_packets.h"

namespace videodat {

struct FileResult {
    int A{0};
    std::string name;
    int64_t size{0};
    int64_t missing{0};
    int64_t nblk{0};
    std::string path;  // "" = not written
    int64_t count{0};
};

std::vector<FileResult> assemble(std::vector<Pkt>& pk, const std::string& outdir, bool keep_partial,
                                 bool dump_other, const LogFn& log,
                                 std::vector<FileManifestItem>* file_manifest);

void analyse_dienste(const std::vector<Pkt>& pk, const std::string& outdir, const LogFn& log);

// Header of a CHVDFILE (paid software), for the console summary.
struct ChvdHeader {
    std::string name;
    int64_t laenge{0};
    std::optional<DateTime> zeit;
};
std::optional<ChvdHeader> chvd_kopf(const Bytes& b);

// Python os.path.splitext on a file name.
std::pair<std::string, std::string> splitext(const std::string& name);
// Sets a file's modification time to a naive local DateTime.
void set_mtime(const std::string& path, const DateTime& t);
// Grey 8-bit PNG writer (uncompressed deflate blocks).
bool write_png_gray(const std::string& path, const std::vector<uint8_t>& pixels, size_t width, size_t height);

} // namespace videodat
