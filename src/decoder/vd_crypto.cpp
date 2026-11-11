/*
 * File:        vd_crypto.cpp
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     VideoDat cipher, encrypted services, paid headers, file keys
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "vd_crypto.h"

#include "vd_packets.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>

namespace videodat {

namespace {

const int kP[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
                    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25};
const int kPC2[48] = {14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4,
                      26, 8, 16, 7, 27, 20, 13, 2, 41, 52, 31, 37, 47, 55, 30, 40,
                      51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
const int kE[48] = {32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11,
                    12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                    22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
const int kPC1[56] = {1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21,
                      22, 23, 25, 26, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37, 38, 39, 41,
                      42, 43, 44, 45, 46, 47, 49, 50, 51, 52, 53, 54, 55, 57, 58, 59, 60,
                      61, 62, 63};
const int kShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
const uint8_t kSbox[8][64] = {
    {14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11,
     9, 5, 3, 8, 4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5,
     11, 3, 14, 10, 0, 6, 13},
    {15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10,
     6, 9, 11, 5, 0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2,
     11, 6, 7, 12, 0, 5, 14, 9},
    {10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14,
     12, 11, 15, 1, 13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7,
     4, 15, 14, 3, 11, 5, 2, 12},
    {7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12,
     1, 10, 14, 9, 10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8,
     9, 4, 5, 11, 12, 7, 2, 14},
    {2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10,
     3, 9, 8, 6, 4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13,
     6, 15, 0, 9, 10, 4, 5, 3},
    {12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14,
     0, 11, 3, 8, 9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10,
     11, 14, 1, 7, 6, 0, 8, 13},
    {4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12,
     2, 15, 8, 6, 1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7,
     9, 5, 0, 15, 14, 2, 3, 12},
    {13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11,
     0, 14, 9, 2, 7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13,
     15, 12, 9, 0, 3, 5, 6, 11}};

uint64_t permute(uint64_t value, int width, const int* table, int n)
{
    uint64_t r = 0;
    for (int i = 0; i < n; ++i) r = (r << 1) | ((value >> (width - table[i])) & 1);
    return r;
}

const Bytes kServiceKeyTemplate = {0x0a, 0x44, 0x69, 0x73, 0x6b, 0x20, 0x45, 0x72};

uint64_t be64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

Bytes sub(const Bytes& b, size_t from, size_t to)
{
    from = std::min(from, b.size());
    to = std::min(std::max(from, to), b.size());
    return Bytes(b.begin() + static_cast<long>(from), b.begin() + static_cast<long>(to));
}

Bytes cat(std::initializer_list<Bytes> parts)
{
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

int64_t le(const Bytes& b, size_t from, size_t n, bool sign = false)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(b[from + i]) << (8 * i);
    if (sign && n < 8 && (v >> (8 * n - 1)) & 1) return static_cast<int64_t>(v) - (int64_t{1} << (8 * n));
    return static_cast<int64_t>(v);
}

} // namespace

Bytes crypt_blocks(const Bytes& data, const Bytes& key, int rounds, bool decrypt)
{
    uint64_t packed = permute(be64(key.data()), 64, kPC1, 56);
    uint64_t left = packed >> 28, right = packed & 0xFFFFFFF;
    std::vector<uint64_t> keys;
    for (int i = 0; i < rounds; ++i) {
        const int s = kShifts[i];
        left = ((left << s) | (left >> (28 - s))) & 0xFFFFFFF;
        right = ((right << s) | (right >> (28 - s))) & 0xFFFFFFF;
        keys.push_back(permute((left << 28) | right, 56, kPC2, 48));
    }
    if (decrypt) std::reverse(keys.begin(), keys.end());
    Bytes out;
    for (size_t off = 0; off + 8 <= data.size(); off += 8) {
        const uint64_t block = be64(data.data() + off);
        uint64_t l = block >> 32, r = block & 0xFFFFFFFF;
        for (uint64_t k : keys) {
            const uint64_t ex = permute(r, 32, kE, 48) ^ k;
            uint64_t s = 0;
            for (int i = 0; i < 8; ++i) {
                const unsigned six = (ex >> (42 - 6 * i)) & 63;
                const unsigned row = ((six >> 4) & 2) | (six & 1);
                const unsigned col = (six >> 1) & 15;
                s = (s << 4) | kSbox[i][16 * row + col];
            }
            const uint64_t nr = l ^ permute(s, 32, kP, 32);
            l = r;
            r = nr;
        }
        const uint64_t res = (r << 32) | l;
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>(res >> (8 * i)));
    }
    return out;
}

ServiceRecord decrypt_service_payload(int stream, const Bytes& data)
{
    ServiceRecord out;
    if (stream != 3 || data.empty() || !(data[0] == 7 || data[0] == 8 || data[0] == 10)) {
        out.error = "unsupported stream/type (expected stream 03, type 07/08/0A)";
        return out;
    }
    const int kind = data[0];
    Bytes key = kServiceKeyTemplate;
    Bytes clear;
    std::string description;
    if (kind == 7 || kind == 8) {
        if (data.size() < 16 || (data.size() & (data.size() - 1))) {
            out.error = "type 07/08 payload must be a power of two, at least 16 bytes";
            return out;
        }
        for (int i = 0; i < 4; ++i) key[2 + i] = data[1 + i];
        clear = cat({sub(data, 0, 6), crypt_blocks(sub(data, 6, data.size() - 2), key, 8),
                     sub(data, data.size() - 2, data.size())});
        description = "account_credit";
    } else {
        if (data.size() != 16) {
            out.error = "type 0A payload must contain exactly 16 bytes";
            return out;
        }
        key[2] = data[2];
        key[3] = data[3];
        clear = cat({sub(data, 0, 4), crypt_blocks(sub(data, 4, 12), key, 8), sub(data, 12, 16)});
        description = "account_limit_update";
    }
    const int64_t expected = le(clear, clear.size() - 2, 2);
    int64_t actual = 0;
    for (size_t i = 0; i + 2 < clear.size(); ++i) actual += clear[i];
    actual &= 0xFFFF;
    out.valid = true;
    out.checksum_ok = expected == actual;
    Json& r = out.record;
    r = Json::object();
    r["stream"] = stream;
    r["type"] = kind;
    r["kind"] = description;
    r["length"] = data.size();
    r["checksum_expected"] = expected;
    r["checksum_actual"] = actual;
    r["checksum_ok"] = out.checksum_ok;
    if (!out.checksum_ok) {
        r["status"] = "inner_checksum_failed";
        return out;
    }
    r["status"] = "inner_checksum_passed";
    r["plaintext_hex"] = hex(clear);
    if (kind == 7 || kind == 8) {
        const int64_t first = le(clear, 1, 3) * 256;
        const int64_t count = le(clear, 4, 2, true) * 256;
        r["first_account"] = first;
        r["account_count"] = count;
        r["last_account"] = first + count - 1;
        r["credit_word_raw"] = le(clear, 6, 4);
        r["account_class"] = clear[10];
        r["eligibility_data_hex"] = hex(sub(clear, 11, clear.size() - 2));
    } else {
        r["sequence"] = le(clear, 2, 2);
        r["limit_raw"] = le(clear, 4, 4, true);
        r["secondary_limit_raw"] = le(clear, 8, 2);
    }
    return out;
}

ServiceCounts write_service_report(const std::vector<Pkt>& packets, const std::string& output)
{
    std::vector<std::pair<Bytes, std::vector<size_t>>> unique;
    std::map<Bytes, size_t> index;
    for (const Pkt& p : packets) {
        if (p.ok && p.A == 3 && !p.pay.empty() && (p.pay[0] == 7 || p.pay[0] == 8 || p.pay[0] == 10)) {
            auto it = index.find(p.pay);
            if (it == index.end()) {
                index.emplace(p.pay, unique.size());
                unique.push_back({p.pay, {p.pos}});
            } else {
                unique[it->second].second.push_back(p.pos);
            }
        }
    }
    ServiceCounts counts;
    std::string text;
    for (const auto& [payload, positions] : unique) {
        ServiceRecord rec = decrypt_service_payload(3, payload);
        Json record;
        if (rec.valid) {
            record = rec.record;
            (rec.checksum_ok ? counts.passed : counts.failed) += 1;
        } else {
            record = Json::object();
            record["stream"] = 3;
            record["type"] = payload[0];
            record["status"] = "malformed";
            record["error"] = rec.error;
            counts.malformed += 1;
        }
        Json pos = Json::array();
        for (size_t p : positions) pos.push_back(p);
        record["positions"] = pos;
        record["copies"] = positions.size();
        record["ciphertext_payload_hex"] = hex(payload);
        text += record.dump(false) + "\n";
    }
    write_text(output, text);
    return counts;
}

TransmissionRecord unwrap_transmission(const Bytes& p)
{
    TransmissionRecord out;
    const bool key01 = p.size() == 16 && p[0] == 1 && p[5] == 0x01 && p[6] == 'K';
    const bool paid = p.size() >= 16 && p[0] == 9;
    if (!key01 && !paid) return out;
    out.valid = true;
    Bytes wrapping = kServiceKeyTemplate;
    for (int i = 0; i < 4; ++i) wrapping[2 + i] = p[1 + i];
    const Bytes clear = cat({sub(p, 0, 7), crypt_blocks(sub(p, 7, 15), wrapping, 8), sub(p, 15, p.size())});
    Json& r = out.record;
    r = Json::object();
    r["type"] = p[0];
    r["payload_hex"] = hex(p);
    r["wrapping_key_hex"] = hex(wrapping);
    if (p[0] == 1) {
        r["kind"] = "file_key";
        r["key_hex"] = hex(sub(clear, 7, 15));
        r["inner_checksum_checked"] = false;
    } else {
        unsigned s = 0;
        for (size_t i = 7; i < 14; ++i) s += clear[i];
        const bool valid = (s & 255) == clear[14];
        r["kind"] = "paid_header";
        r["checksum_ok"] = valid;
        out.checksum_ok = valid;
        if (valid) {
            r["plaintext_hex"] = hex(clear);
            out.price_raw = le(clear, 7, 2);
            r["price_raw"] = out.price_raw;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// CHVDFILE decryption (videodat_files.py)
// ---------------------------------------------------------------------------

namespace {

struct ValueError {
    std::string msg;
};

struct FileHeader {
    int rounds{0};
    int key_check_byte{0};
    int64_t size{0};
    std::string name;
};

FileHeader parse_file_header(const Bytes& data)
{
    static const char kMagic[] = "CHVDFILE";
    if (data.size() < 32 || !std::equal(data.begin(), data.begin() + 8, kMagic)) {
        throw ValueError{"missing original CHVDFILE header"};
    }
    if (!(data[8] >= 1 && data[8] <= 16)) throw ValueError{"invalid cipher round count"};
    const int64_t size = le(data, 15, 4);
    if (static_cast<int64_t>(data.size()) != 32 + ((size + 7) / 8) * 8) {
        throw ValueError{"file length does not match padded size in header"};
    }
    size_t z = 19;
    while (z < 32 && data[z] != 0) ++z;
    if (z == 32) throw ValueError{"unterminated original filename"};
    FileHeader h;
    h.rounds = data[8];
    h.key_check_byte = data[9];
    h.size = size;
    h.name = cp437_to_utf8(data.data() + 19, z - 19);
    return h;
}

using Ranges = std::vector<std::pair<int64_t, int64_t>>;

Ranges merge_ranges(Ranges r)
{
    std::sort(r.begin(), r.end());
    Ranges merged;
    for (const auto& [lo, hi] : r) {
        if (hi <= lo) continue;
        if (!merged.empty() && lo <= merged.back().second) {
            merged.back().second = std::max(hi, merged.back().second);
        } else {
            merged.emplace_back(lo, hi);
        }
    }
    return merged;
}

int64_t floordiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

Ranges affected_plaintext_ranges(const Ranges& ranges, int64_t file_size, int64_t plain_size)
{
    Ranges out;
    for (const auto& [lo, hi] : ranges) {
        if (!(0 <= lo && lo < hi && hi <= file_size)) throw ValueError{"invalid ciphertext uncertainty range"};
        if (lo < 32) throw ValueError{"CHVDFILE header contains missing or conflicting bytes"};
        out.emplace_back(floordiv(lo - 32, 8) * 8, std::min(plain_size, floordiv(hi - 32 + 7, 8) * 8));
    }
    return merge_ranges(out);
}

Json ranges_json(const Ranges& r)
{
    Json a = Json::array();
    for (const auto& [lo, hi] : r) {
        Json p = Json::array();
        p.push_back(lo);
        p.push_back(hi);
        a.push_back(p);
    }
    return a;
}

std::string safe_filename(const std::string& utf8_name)
{
    // re.sub(r'[^A-Za-z0-9_.!()-]', '_', name): one '_' per character.
    std::string n;
    for (size_t i = 0; i < utf8_name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(utf8_name[i]);
        if (c >= 0x80) {
            // skip continuation bytes of this character
            while (i + 1 < utf8_name.size() && (static_cast<unsigned char>(utf8_name[i + 1]) & 0xC0) == 0x80) ++i;
            n.push_back('_');
        } else if (std::isalnum(c) || c == '_' || c == '.' || c == '!' || c == '(' || c == ')' || c == '-') {
            n.push_back(static_cast<char>(c));
        } else {
            n.push_back('_');
        }
    }
    size_t a = 0, b = n.size();
    while (a < b && (n[a] == '.' || n[a] == ' ')) ++a;
    while (b > a && (n[b - 1] == '.' || n[b - 1] == ' ')) --b;
    n = n.substr(a, b - a);
    std::string stem = n.substr(0, n.find('.'));
    for (auto& ch : stem) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    bool reserved = stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL";
    if (n.size() >= 4) {
        std::string pre = n.substr(0, 3);
        for (auto& ch : pre) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if ((pre == "COM" || pre == "LPT") && n[3] >= '1' && n[3] <= '9' && (n.size() == 4 || n[4] == '.')) {
            reserved = true;
        }
    }
    if (n.empty() || reserved) n = "recovered_" + n;
    return n;
}

Bytes read_all(const std::string& path)
{
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

FileCounts write_file_report(const std::vector<Pkt>& packets, const std::vector<FileManifestItem>& manifest,
                             const std::string& output_dir)
{
    make_dirs(output_dir);
    struct KeyRec {
        int stream;
        size_t position;
        Bytes key;
    };
    std::vector<KeyRec> keys;
    std::vector<std::pair<int, size_t>> paid;  // stream, position
    Json metadata = Json::array();
    for (const Pkt& p : packets) {
        if (!p.ok || !(p.B & 128) || !p.A) continue;
        TransmissionRecord t = unwrap_transmission(p.pay);
        if (!t.valid) continue;
        Json rec = t.record;
        rec["stream"] = p.A;
        rec["position"] = p.pos;
        rec["outer_checksum_checked"] = true;
        metadata.push_back(rec);
        if (p.pay[0] == 1) {
            const std::string& kh = rec.find("key_hex")->as_string();
            Bytes key;
            for (size_t i = 0; i + 1 < kh.size(); i += 2) key.push_back(static_cast<uint8_t>(std::stoi(kh.substr(i, 2), nullptr, 16)));
            keys.push_back({p.A, p.pos, key});
        } else {
            paid.emplace_back(p.A, p.pos);
        }
    }
    FileCounts counts;
    Json reports = Json::array();
    for (const FileManifestItem& item : manifest) {
        std::string upper = item.name;
        for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (upper.size() < 4 || upper.compare(upper.size() - 4, 4, ".VD$") != 0) continue;
        Json result = Json::object();
        result["stream"] = item.stream;
        result["name"] = item.name;
        result["size"] = item.size;
        result["path"] = item.path.empty() ? Json() : Json(item.path);
        Json hp = Json::array();
        for (size_t p : item.header_positions) hp.push_back(p);
        result["header_positions"] = hp;
        result["missing_ranges"] = ranges_json(item.missing_ranges);
        result["conflicting_ranges"] = ranges_json(item.conflicting_ranges);
        if (item.path.empty()) {
            result["status"] = "not_written";
            result["reason"] = "Use --partial to retain incomplete files";
            reports.push_back(result);
            continue;
        }
        const Bytes data = read_all(item.path);
        result["ciphertext_sha256"] = sha256_hex(data);
        try {
            const FileHeader header = parse_file_header(data);
            const bool has_start = !item.header_positions.empty();
            const size_t start = has_start ? *std::min_element(item.header_positions.begin(), item.header_positions.end()) : 0;
            double next_start = std::numeric_limits<double>::infinity();
            if (has_start) {
                for (const auto& [s, pos] : paid) {
                    if (s == item.stream && pos > start) next_start = std::min(next_start, static_cast<double>(pos));
                }
            }
            std::vector<std::pair<Bytes, std::vector<size_t>>> candidates;
            for (const KeyRec& k : keys) {
                unsigned sum = data[8] + data[9];
                for (uint8_t b : k.key) sum += b;
                if (k.stream == item.stream &&
                    (!has_start || (start <= k.position && static_cast<double>(k.position) < next_start)) &&
                    (sum & 255) == 0) {
                    bool found = false;
                    for (auto& c : candidates) {
                        if (c.first == k.key) {
                            c.second.push_back(k.position);
                            found = true;
                        }
                    }
                    if (!found) candidates.push_back({k.key, {k.position}});
                }
            }
            if (candidates.size() != 1) {
                counts.unavailable_key += 1;
                result["status"] = candidates.empty() ? "key_unavailable" : "ambiguous_key";
                result["matching_keys"] = candidates.size();
            } else {
                const Bytes& key = candidates[0].first;
                // decrypt_file()
                unsigned ks = data[8] + data[9];
                for (uint8_t b : key) ks += b;
                if (key.size() != 8 || (ks & 255)) throw ValueError{"file key fails the original eight-bit checksum"};
                const int64_t dlen = static_cast<int64_t>(data.size());
                const Ranges holes = affected_plaintext_ranges(
                    Ranges(item.missing_ranges.begin(), item.missing_ranges.end()), dlen, header.size);
                const Ranges conflicts = affected_plaintext_ranges(
                    Ranges(item.conflicting_ranges.begin(), item.conflicting_ranges.end()), dlen, header.size);
                const Bytes body = sub(data, 32, data.size());
                const int64_t blen = static_cast<int64_t>(body.size());
                Bytes plain(body.size(), 0);
                int64_t cursor = 0;
                Ranges hl = holes;
                hl.emplace_back(blen, blen);
                for (const auto& [lo, hi] : hl) {
                    if (lo > cursor) {
                        const Bytes part = crypt_blocks(sub(body, static_cast<size_t>(cursor), static_cast<size_t>(lo)), key, header.rounds);
                        // Python slice assignment replaces plain[cursor:lo] by the
                        // decrypted blocks (lo - cursor is a multiple of 8 here).
                        std::copy(part.begin(), part.end(), plain.begin() + cursor);
                    }
                    cursor = std::min(blen, floordiv(hi + 7, 8) * 8);
                }
                plain.resize(static_cast<size_t>(std::min<int64_t>(header.size, static_cast<int64_t>(plain.size()))));
                Json info = Json::object();
                info["rounds"] = header.rounds;
                info["key_check_byte"] = header.key_check_byte;
                info["size"] = header.size;
                info["name"] = header.name;
                info["missing_plaintext_ranges"] = ranges_json(holes);
                info["conflicting_plaintext_ranges"] = ranges_json(conflicts);
                const bool complete = holes.empty() && conflicts.empty();
                info["complete_capture"] = complete;
                info["key_checksum_ok"] = true;
                info["content_integrity_verified"] = false;
                info["sha256"] = sha256_hex(plain);

                const std::string folder = join_path(output_dir, strf("stream_%02X", item.stream));
                make_dirs(folder);
                const std::string suffix = complete ? ".decrypted" : ".partial";
                const std::string name = safe_filename(header.name);
                std::string path = join_path(folder, name + suffix);
                int count = 1;
                while (std::filesystem::exists(std::filesystem::u8path(path))) {
                    ++count;
                    path = join_path(folder, name + "." + std::to_string(count) + suffix);
                }
                write_file(path, plain);
                result["status"] = "decrypted";
                result["key_hex"] = hex(key);
                Json kp = Json::array();
                for (size_t p : candidates[0].second) kp.push_back(p);
                result["key_packet_positions"] = kp;
                result["output"] = path;
                result["plaintext"] = info;
                counts.decrypted += 1;
            }
        } catch (const ValueError& e) {
            result["status"] = "rejected";
            result["reason"] = e.msg;
            counts.rejected += 1;
        }
        reports.push_back(result);
    }
    Json report = Json::object();
    Json c = Json::object();
    c["decrypted"] = counts.decrypted;
    c["unavailable_key"] = counts.unavailable_key;
    c["rejected"] = counts.rejected;
    report["counts"] = c;
    report["transmission_packets"] = metadata;
    report["files"] = reports;
    report["note"] =
        "Key checks are eight-bit sums, not authentication. Verify archive/file integrity independently. "
        "Missing cipher blocks are zero-filled and explicitly mapped; original bytes are unchanged.";
    write_text(join_path(output_dir, "file_decryption.json"), report.dump(true, 2) + "\n");
    return counts;
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

std::string sha256_hex(const Bytes& data)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    Bytes m = data;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = static_cast<uint32_t>(m[off + 4 * i]) << 24 | static_cast<uint32_t>(m[off + 4 * i + 1]) << 16 |
                   static_cast<uint32_t>(m[off + 4 * i + 2]) << 8 | m[off + 4 * i + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    Bytes out;
    for (uint32_t v : h) {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    return hex(out);
}

} // namespace videodat
