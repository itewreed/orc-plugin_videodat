/*
 * File:        vd_crypto.h
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     VideoDat cipher (DES round function without IP/FP) and the
 *              encrypted services, paid headers and file keys
 *              (port of videodat_crypto.py and videodat_files.py)
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include "vd_json.h"
#include "vd_util.h"

namespace videodat {

struct Pkt;

Bytes crypt_blocks(const Bytes& data, const Bytes& key, int rounds = 8, bool decrypt = true);

struct ServiceRecord {
    bool valid{false};      // false: ValueError
    std::string error;
    bool checksum_ok{false};
    Json record;
};

// decrypt_service_payload(stream, payload)
ServiceRecord decrypt_service_payload(int stream, const Bytes& payload);

// write_service_report(): returns counts passed, failed, malformed.
struct ServiceCounts {
    int passed{0}, failed{0}, malformed{0};
};
ServiceCounts write_service_report(const std::vector<Pkt>& packets, const std::string& output);

struct TransmissionRecord {
    bool valid{false};
    Json record;
    bool checksum_ok{false};  // type 09
    int64_t price_raw{0};
};
// unwrap_transmission(payload)
TransmissionRecord unwrap_transmission(const Bytes& payload);

// One entry of the file manifest assemble() hands to write_file_report().
struct FileManifestItem {
    int stream{0};
    std::string name;
    uint32_t size{0};
    std::string path;  // "" = not written
    std::vector<size_t> header_positions;
    std::vector<std::pair<int64_t, int64_t>> missing_ranges;
    std::vector<std::pair<int64_t, int64_t>> conflicting_ranges;
};

struct FileCounts {
    int decrypted{0}, unavailable_key{0}, rejected{0};
};
FileCounts write_file_report(const std::vector<Pkt>& packets,
                             const std::vector<FileManifestItem>& manifest,
                             const std::string& output_dir);

// SHA-256 of |data| as lowercase hex.
std::string sha256_hex(const Bytes& data);

} // namespace videodat
