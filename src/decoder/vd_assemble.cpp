/*
 * File:        vd_assemble.cpp
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     File assembly of the 1993 variant and the service report
 *              (assemble() and analyse_dienste() of videodat_decoder.py)
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "vd_assemble.h"

#include "vd_prng.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <set>

#if defined(_WIN32)
#include <sys/utime.h>
#else
#include <utime.h>
#endif

namespace videodat {

namespace {

uint32_t le32(const Bytes& p, size_t i)
{
    return static_cast<uint32_t>(p[i]) | static_cast<uint32_t>(p[i + 1]) << 8 |
           static_cast<uint32_t>(p[i + 2]) << 16 | static_cast<uint32_t>(p[i + 3]) << 24;
}

Bytes sub(const Bytes& b, size_t from, size_t to)
{
    from = std::min(from, b.size());
    to = std::min(std::max(from, to), b.size());
    return Bytes(b.begin() + static_cast<long>(from), b.begin() + static_cast<long>(to));
}

std::string ascii(const Bytes& b)
{
    return std::string(b.begin(), b.end());
}

int64_t ceil_div(int64_t a, int64_t b)
{
    return -((-a) / b - (((-a) % b != 0) && ((-a < 0) != (b < 0)) ? 1 : 0));
}

std::string safe_name(const Bytes& raw)
{
    Bytes b;
    for (uint8_t c : raw) {
        if (c == 0) break;
        const bool bad = c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                         c == '>' || c == '|' || c < 0x20;
        b.push_back(bad ? '_' : c);
    }
    size_t a = 0, e = b.size();
    while (a < e && (b[a] == ' ' || b[a] == '.')) ++a;
    while (e > a && (b[e - 1] == ' ' || b[e - 1] == '.')) --e;
    if (a == e) return "unbenannt";
    return cp437_to_utf8(b.data() + a, e - a);
}

std::string lower_ascii(std::string s)
{
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool name_char(uint8_t c)
{
    return std::isalnum(c) || c == '.' || c == '_' || c == '$' || c == '!' || c == '#' || c == '&' || c == '-';
}

bool name_re_char(uint8_t c)
{
    static const std::string extra = "_!$#&@%'(){}~^`-";
    return (c < 0x80 && std::isalnum(c)) || extra.find(static_cast<char>(c)) != std::string::npos;
}

// NAME_RE.fullmatch
bool name_re(const Bytes& n)
{
    size_t i = 0;
    while (i < n.size() && name_re_char(n[i])) ++i;
    if (i < 1 || i > 8) return false;
    if (i == n.size()) return true;
    if (n[i] != '.') return false;
    size_t j = i + 1;
    while (j < n.size() && name_re_char(n[j])) ++j;
    return j == n.size() && j - i - 1 <= 3;
}

struct HeaderFields {
    Bytes name;
    uint32_t size;
    uint32_t ts;
};

std::optional<HeaderFields> header_fields(const Bytes& p)
{
    if (p.size() < 15 || !(p[0] == 0x02 || p[0] == 0x0C)) return std::nullopt;
    const uint32_t size = le32(p, 5);
    const size_t nl = p[13];
    Bytes name = sub(p, 14, 14 + nl);
    if (!(nl >= 1 && nl <= 12) || name.size() != nl || size > kMaxFile) return std::nullopt;
    return HeaderFields{name, size, le32(p, 9)};
}

Bytes vote_header(const std::vector<Bytes>& copies)
{
    size_t L = copies[0].size();
    for (const auto& c : copies) L = std::min(L, c.size());
    Bytes out(L);
    for (size_t i = 0; i < L; ++i) {
        Counter<uint8_t> cnt;
        for (const auto& c : copies) cnt.add(c[i]);
        int64_t best = 0;
        for (const auto& [v, n] : cnt.items()) best = std::max(best, n);
        std::vector<uint8_t> cands;
        for (const auto& [v, n] : cnt.items()) {
            if (n == best) cands.push_back(v);
        }
        if (cands.size() > 1 && i >= 14) {
            std::vector<uint8_t> nice;
            for (uint8_t v : cands) {
                if (name_char(v)) nice.push_back(v);
            }
            if (!nice.empty()) cands = nice;
        }
        out[i] = cands[0];
    }
    return out;
}

int jump(int from, int to)
{
    const int d = (to - from) & 0xFF;
    return d > 128 ? d - 256 : d;
}

std::vector<std::optional<int64_t>> unwrap_counter(const std::vector<Pkt*>& xs, int back = 16, int fwd = 128)
{
    std::vector<std::optional<int64_t>> res;
    bool have_last = false;
    int lastC = 0;
    int64_t lastU = 0;
    std::optional<int64_t> maxu;
    for (size_t i = 0; i < xs.size(); ++i) {
        const int C = xs[i]->C;
        int64_t u;
        if (!have_last) {
            u = C;
        } else {
            const int d = jump(lastC, C);
            if (-back <= d && d <= fwd) {
                u = lastU + d;
            } else {
                std::vector<int> follow;
                for (size_t k = i + 1; k < std::min(xs.size(), i + 4); ++k) follow.push_back(jump(C, xs[k]->C));
                int good = 0;
                for (int f : follow) good += (-back <= f && f <= fwd) ? 1 : 0;
                if (!follow.empty() && good >= std::max(1, static_cast<int>(follow.size()) - 1)) {
                    const int64_t m = *maxu;
                    u = m + 1024 + (((C - m) % 256) + 256) % 256;
                } else {
                    res.emplace_back(std::nullopt);
                    continue;
                }
            }
        }
        have_last = true;
        lastC = C;
        lastU = u;
        maxu = maxu ? std::max(*maxu, u) : u;
        res.emplace_back(u);
    }
    return res;
}

std::map<int64_t, Bytes> rescue_headers(const std::vector<Pkt*>& bad, const std::vector<Pkt*>& okx,
                                        const std::vector<int64_t>& oku, const std::map<int64_t, Bytes>& seq,
                                        int64_t blksz_hint, const LogFn& log, int A)
{
    std::vector<size_t> pos;
    for (Pkt* x : okx) pos.push_back(x->pos);
    std::vector<std::pair<int64_t, std::vector<Bytes>>> cand;
    auto seq_type = [&](int64_t u, uint8_t dflt) -> uint8_t {
        auto it = seq.find(u);
        return it == seq.end() ? dflt : it->second[0];
    };
    for (Pkt* x : bad) {
        if (!header_fields(x->pay)) continue;
        const size_t j = static_cast<size_t>(std::upper_bound(pos.begin(), pos.end(), x->pos) - pos.begin());
        if (j >= okx.size() || okx[j]->pos - x->pos > 4000) continue;
        const int64_t lo_u = j > 0 ? oku[j - 1] + 1 : oku[j] - 8;
        std::optional<int64_t> u;
        const int64_t stop = std::max(lo_u, oku[j] - 8) - 1;
        for (int64_t w = oku[j] - 1; w > stop; --w) {
            if ((w & 0xFF) == x->C) {
                u = w;
                break;
            }
        }
        if (!u) {
            u = oku[j] - 1;
            unsigned v = static_cast<unsigned>((x->C ^ *u) & 0xFF);
            int bits = 0;
            for (; v; v &= v - 1) ++bits;
            if (bits > 1) continue;
        }
        const uint8_t t = seq_type(*u, 0xFF);
        if (t == 0x02 || t == 0x0C) continue;
        bool other = false;
        for (int64_t w = *u + 1; w < oku[j]; ++w) {
            if (seq_type(w, 0x04) != 0x04) other = true;
        }
        if (other) continue;
        bool found = false;
        for (auto& c : cand) {
            if (c.first == *u) {
                c.second.push_back(x->pay);
                found = true;
            }
        }
        if (!found) cand.push_back({*u, {x->pay}});
    }
    std::map<int64_t, Bytes> rescued;
    for (const auto& [u, copies] : cand) {
        const Bytes p = copies.size() > 1 ? vote_header(copies) : copies[0];
        const auto hf = header_fields(p);
        if (!hf || !name_re(hf->name)) continue;
        if (!dos_time(hf->ts)) continue;
        std::optional<int64_t> nblk;
        if (blksz_hint) nblk = ceil_div(hf->size, blksz_hint);
        std::optional<int64_t> end;
        for (int64_t v = u + 1; v < u + 1 + (nblk ? *nblk : 0) + 64; ++v) {
            auto it = seq.find(v);
            if (it != seq.end() && (it->second[0] == 0x02 || it->second[0] == 0x03 || it->second[0] == 0x0C)) {
                end = v;
                break;
            }
        }
        if (end && seq.at(*end)[0] == 0x03 && nblk) {
            if (*end - u - 1 != *nblk) continue;
        }
        rescued[u] = p;
        log(strf("  Strom %02X: defekter Dateikopf repariert: %s (%u Bytes, %zu Kopie%s)", A,
                 safe_name(hf->name).c_str(), hf->size, copies.size(), copies.size() > 1 ? "n" : ""));
    }
    return rescued;
}

struct Transmission {
    std::string name;
    uint32_t size{0};
    std::optional<DateTime> mtime;
    std::vector<size_t> header_positions;
    int64_t start{0};
    std::map<int64_t, Counter<Bytes>> blocks;
    int64_t blksz{0};
    int64_t nblk{0};
};

struct FileEntry {
    std::string name;
    uint32_t size{0};
    std::optional<DateTime> mtime;
    std::map<int64_t, Counter<Bytes>> blocks;
    int64_t blksz{0};
    int64_t nblk{0};
    int64_t count{0};
    int A{0};
    std::vector<size_t> header_positions;
};

bool printable(const Bytes& b)
{
    for (uint8_t c : b) {
        if (c < 32 || c >= 127) return false;
    }
    return true;
}

} // namespace

std::pair<std::string, std::string> splitext(const std::string& name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return {name, ""};
    // leading dots do not start an extension
    size_t first = 0;
    while (first < name.size() && name[first] == '.') ++first;
    if (dot < first) return {name, ""};
    return {name.substr(0, dot), name.substr(dot)};
}

void set_mtime(const std::string& path, const DateTime& t)
{
    std::tm tm{};
    tm.tm_year = t.year - 1900;
    tm.tm_mon = t.month - 1;
    tm.tm_mday = t.day;
    tm.tm_hour = t.hour;
    tm.tm_min = t.minute;
    tm.tm_sec = t.second;
    tm.tm_isdst = -1;
    const std::time_t tt = std::mktime(&tm);
    if (tt == static_cast<std::time_t>(-1)) return;
#if defined(_WIN32)
    struct _utimbuf ub{tt, tt};
    _wutime(std::filesystem::u8path(path).wstring().c_str(), &ub);
#else
    struct utimbuf ub{tt, tt};
    utime(path.c_str(), &ub);
#endif
}

std::optional<ChvdHeader> chvd_kopf(const Bytes& b)
{
    static const char kMagic[] = "CHVDFILE";
    if (b.size() < 32 || !std::equal(b.begin(), b.begin() + 8, kMagic)) return std::nullopt;
    const uint32_t t = b[11] | b[12] << 8;
    const uint32_t d = b[13] | b[14] << 8;
    ChvdHeader h;
    h.laenge = le32(b, 15);
    size_t z = 20;
    while (z < 32 && b[z] != 0) ++z;
    // ascii decode with "replace": non-ASCII -> U+FFFD
    std::string name;
    for (size_t i = 20; i < z; ++i) {
        if (b[i] < 0x80) {
            name.push_back(static_cast<char>(b[i]));
        } else {
            name += "\xEF\xBF\xBD";
        }
    }
    h.name = name;
    h.zeit = dos_time(d << 16 | t);
    return h;
}

std::vector<FileResult> assemble(std::vector<Pkt>& pk, const std::string& outdir, bool keep_partial,
                                 bool dump_other, const LogFn& log, std::vector<FileManifestItem>* file_manifest)
{
    make_dirs(outdir);
    std::map<std::pair<int, int>, std::vector<Pkt*>> subs, badsubs;
    for (Pkt& x : pk) {
        if (x.ok) {
            subs[{x.A, x.B >> 7}].push_back(&x);
        } else if (!x.pay.empty() && (x.pay[0] == 0x02 || x.pay[0] == 0x0C)) {
            badsubs[{x.A, x.B >> 7}].push_back(&x);
        }
    }
    std::vector<FileResult> results;
    std::vector<std::pair<std::tuple<std::string, uint32_t, int64_t>, FileEntry>> files;
    auto find_file = [&](const std::tuple<std::string, uint32_t, int64_t>& key) -> FileEntry* {
        for (auto& f : files) {
            if (f.first == key) return &f.second;
        }
        return nullptr;
    };
    size_t n_rescued = 0;
    for (auto& [key, all] : subs) {
        const int A = key.first, sub_ = key.second;
        const auto us_all = unwrap_counter(all);
        std::vector<Pkt*> xs;
        std::vector<int64_t> us;
        for (size_t i = 0; i < all.size(); ++i) {
            if (us_all[i]) {
                xs.push_back(all[i]);
                us.push_back(*us_all[i]);
            }
        }
        std::map<int64_t, Bytes> seq;
        std::map<int64_t, std::vector<size_t>> positions;
        std::map<int64_t, Counter<Bytes>> votes;
        for (size_t i = 0; i < xs.size(); ++i) {
            const int64_t u = us[i];
            positions[u].push_back(xs[i]->pos);
            seq.emplace(u, xs[i]->pay);
            votes[u].add(xs[i]->pay, (xs[i]->how == "ok" || xs[i]->how == "unstuffed") ? 2 : 1);
        }
        int64_t blksz_hint = 0;
        bool have_bl = false;
        for (const auto& [u, p] : seq) {
            if (p[0] == 4) {
                blksz_hint = std::max(blksz_hint, static_cast<int64_t>(p.size()) - 1);
                have_bl = true;
            }
        }
        if (!have_bl) blksz_hint = 255;
        auto bad_it = badsubs.find(key);
        const auto resc = rescue_headers(bad_it == badsubs.end() ? std::vector<Pkt*>{} : bad_it->second, xs, us,
                                         seq, blksz_hint, log, A);
        n_rescued += resc.size();
        for (const auto& [u, p] : resc) seq[u] = p;

        Counter<int> other;
        std::vector<std::map<int64_t, Bytes>> orphan_runs;
        std::vector<std::optional<int64_t>> orphan_ende;
        long orphan = -1;  // index into orphan_runs, -1 = None
        std::optional<Transmission> cur;

        auto finish = [&](std::optional<Transmission>& c) {
            if (!c) return;
            const auto fkey = std::make_tuple(c->name, c->size, c->mtime ? c->mtime->total_us() : INT64_MIN);
            FileEntry* f = find_file(fkey);
            if (!f) {
                FileEntry e;
                e.name = c->name;
                e.size = c->size;
                e.mtime = c->mtime;
                e.blksz = c->blksz;
                e.nblk = c->nblk;
                e.A = A;
                files.emplace_back(fkey, e);
                f = &files.back().second;
            }
            f->header_positions.insert(f->header_positions.end(), c->header_positions.begin(),
                                       c->header_positions.end());
            if (f->blocks.empty() && !c->blocks.empty()) {
                f->blksz = c->blksz;
                f->nblk = c->nblk;
            }
            if (!c->blocks.empty() && f->blksz != c->blksz) return;
            f->count += 1;
            for (const auto& [i, cnt] : c->blocks) f->blocks[i].update(cnt);
        };

        for (const auto& [u, p] : seq) {
            const uint8_t t = p[0];
            if ((t == 0x02 || t == 0x0C) && p.size() >= 14) {
                finish(cur);
                cur.reset();
                orphan = -1;
                const uint32_t size = le32(p, 5);
                const size_t nl = p[13];
                if (size > kMaxFile) {
                    log(strf("  Strom %02X: Dateikopf mit unplausibler Groesse %u ignoriert", A, size));
                    continue;
                }
                Transmission tr;
                tr.name = safe_name(sub(p, 14, 14 + nl));
                tr.size = size;
                auto pit = positions.find(u);
                if (pit != positions.end()) tr.header_positions = pit->second;
                tr.mtime = dos_time(le32(p, 9));
                tr.start = u;
                tr.blksz = blksz_hint;
                tr.nblk = blksz_hint ? ceil_div(size, blksz_hint) : 0;
                cur = tr;
            } else if (t == 0x04) {
                const Bytes blk = sub(p, 1, p.size());
                std::optional<int64_t> i;
                if (cur) {
                    i = u - cur->start - 1;
                    if (!(0 <= *i && *i < cur->nblk)) {
                        finish(cur);
                        cur.reset();
                        i.reset();
                    } else {
                        Counter<Bytes> cnt;
                        for (const auto& [pp, n] : votes[u].items()) {
                            if (pp[0] == 0x04) cnt.add(sub(pp, 1, pp.size()), n);
                        }
                        cur->blocks[*i] = cnt;
                        if (*i == cur->nblk - 1) {
                            finish(cur);
                            cur.reset();
                        }
                    }
                }
                if (!i) {
                    if (orphan >= 0 && !orphan_runs[static_cast<size_t>(orphan)].empty() &&
                        u - orphan_runs[static_cast<size_t>(orphan)].rbegin()->first > 512) {
                        orphan = -1;
                    }
                    if (orphan < 0) {
                        orphan_runs.emplace_back();
                        orphan_ende.emplace_back();
                        orphan = static_cast<long>(orphan_runs.size()) - 1;
                    }
                    orphan_runs[static_cast<size_t>(orphan)][u] = blk;
                }
            } else if (t == 0x03) {
                finish(cur);
                cur.reset();
                if (orphan >= 0) orphan_ende[static_cast<size_t>(orphan)] = u;
                orphan = -1;
            } else {
                other.add(t);
                if (dump_other) {
                    const std::string sd = join_path(outdir, "andere_dienste");
                    make_dirs(sd);
                    write_file(join_path(sd, strf("strom_%02X_typ_%02X.bin", A, t)), p, true);
                }
            }
            if (t == 0x00 && p.size() > 9 && 2 < p[7] && p[7] < 40) {
                const Bytes info = sub(p, 8, 8 + p[7]);
                if (printable(info)) log(strf("  Strom %02X: Infoblock '%s'", A, ascii(info).c_str()));
            }
            if (t == 0x09 && p.size() > 16 && 2 < p[15] && p[15] < 40) {
                const Bytes info = sub(p, 16, 16 + p[15]);
                if (printable(info)) log(strf("  Strom %02X: Infoblock Bezahlsendung '%s'", A, ascii(info).c_str()));
            }
        }
        finish(cur);
        if (keep_partial) {
            for (size_t r = 0; r < orphan_runs.size(); ++r) {
                const auto& run = orphan_runs[r];
                if (run.empty()) continue;
                const std::string sd = join_path(outdir, strf("strom_%02X", A));
                make_dirs(sd);
                const int64_t u0 = run.begin()->first, u1 = run.rbegin()->first;
                size_t bs = 0;
                for (const auto& [u, b] : run) bs = std::max(bs, b.size());
                const std::string fname = strf("ohne_dateikopf_%lld.bin.unvollstaendig", static_cast<long long>(u0));
                Bytes data;
                for (int64_t u = u0; u <= u1; ++u) {
                    auto it = run.find(u);
                    if (it != run.end()) {
                        data.insert(data.end(), it->second.begin(), it->second.end());
                    } else {
                        data.insert(data.end(), bs, 0);
                    }
                }
                write_file(join_path(sd, fname), data);
                log(strf("  Strom %02X/%d: %zu Datenbloecke ohne Dateikopf%s -> %s", A, sub_, run.size(),
                         orphan_ende[r] && *orphan_ende[r] == u1 + 1 ? ", Dateiende empfangen" : "", fname.c_str()));
            }
        }
        if (!other.empty()) {
            auto items = other.items();
            std::sort(items.begin(), items.end());
            std::string s;
            for (const auto& [t, n] : items) s += (s.empty() ? "" : ", ") + strf("%02X:%lld", t, static_cast<long long>(n));
            log(strf("  Strom %02X/%d: andere Pakettypen %s", A, sub_, s.c_str()));
        }
    }

    Counter<std::pair<int, std::string>> used;
    size_t conflicts = 0;
    for (auto& [fkey, f] : files) {
        Bytes data;
        int64_t missing = 0;
        std::vector<std::pair<int64_t, int64_t>> missing_ranges, conflicting_ranges;
        const int64_t size = f.size;
        for (int64_t i = 0; i < f.nblk; ++i) {
            auto it = f.blocks.find(i);
            std::optional<Bytes> blk;
            if (it != f.blocks.end() && !it->second.empty()) {
                blk = it->second.most_common()[0].first;
                if (it->second.size() > 1) {
                    ++conflicts;
                    conflicting_ranges.emplace_back(i * f.blksz, std::min(size, (i + 1) * f.blksz));
                }
            }
            if (!blk) {
                ++missing;
                blk = Bytes(static_cast<size_t>(f.blksz), 0);
                missing_ranges.emplace_back(i * f.blksz, std::min(size, (i + 1) * f.blksz));
            } else if (static_cast<int64_t>(blk->size()) < std::min(f.blksz, size - i * f.blksz)) {
                missing_ranges.emplace_back(i * f.blksz + static_cast<int64_t>(blk->size()),
                                            std::min(size, (i + 1) * f.blksz));
            }
            Bytes b = sub(*blk, 0, static_cast<size_t>(f.blksz));
            b.resize(static_cast<size_t>(f.blksz), 0);
            data.insert(data.end(), b.begin(), b.end());
        }
        if (static_cast<int64_t>(data.size()) > size) data.resize(static_cast<size_t>(size));
        const bool complete = missing == 0 && static_cast<int64_t>(data.size()) == size;
        const auto ukey = std::make_pair(f.A, lower_ascii(f.name));
        used.add(ukey);
        std::string fname = f.name;
        if (used.get(ukey) > 1) {
            const auto [stem, ext] = splitext(f.name);
            fname = stem + "_" + std::to_string(used.get(ukey)) + ext;
        }
        std::string path;
        if (complete || keep_partial) {
            if (!complete) fname += ".unvollstaendig";
            const std::string sd = join_path(outdir, strf("strom_%02X", f.A));
            make_dirs(sd);
            path = join_path(sd, fname);
            write_file(path, data);
            if (f.mtime) set_mtime(path, *f.mtime);
        }
        results.push_back({f.A, f.name, size, missing, f.nblk, path, f.count});
        if (file_manifest) {
            FileManifestItem m;
            m.stream = f.A;
            m.name = f.name;
            m.size = f.size;
            m.path = path;
            m.header_positions = f.header_positions;
            m.missing_ranges = missing_ranges;
            m.conflicting_ranges = conflicting_ranges;
            file_manifest->push_back(m);
        }
    }
    if (n_rescued) log(strf("  %zu defekte Dateikoepfe repariert", n_rescued));
    if (conflicts) log(strf("  %zu Dateibloecke lagen in abweichenden Versionen vor (Mehrheitsentscheid)", conflicts));
    return results;
}

// ---------------------------------------------------------------------------
// Other services
// ---------------------------------------------------------------------------

namespace {

Bytes unrun(const Bytes& b, size_t from)
{
    Bytes out;
    size_t i = from;
    while (i < b.size()) {
        if (b[i] == 0x00 || b[i] == 0xFF) {
            if (i + 1 >= b.size()) break;
            out.insert(out.end(), b[i + 1], b[i]);
            i += 2;
        } else {
            out.push_back(b[i]);
            ++i;
        }
    }
    return out;
}

std::optional<DateTime> dos_dt(const Bytes& p, size_t i)
{
    const uint32_t t = p[i] | p[i + 1] << 8;
    const uint32_t d = p[i + 2] | p[i + 3] << 8;
    return DateTime::make(1980 + static_cast<int>(d >> 9), (d >> 5) & 15, d & 31, t >> 11, (t >> 5) & 63, (t & 31) * 2);
}

std::string hm(const std::optional<DateTime>& t)
{
    return t ? t->format("%H:%M:%S") : "?";
}

class Uhr {
public:
    explicit Uhr(std::vector<std::pair<size_t, DateTime>> ticks) : t_(std::move(ticks))
    {
        std::vector<double> rates;
        for (size_t k = 0; k + 1 < t_.size(); ++k) {
            const double dt = static_cast<double>(t_[k + 1].second.total_us() - t_[k].second.total_us()) / 1e6;
            if (0 < dt && dt <= 30) {
                rates.push_back(static_cast<double>(t_[k + 1].first - t_[k].first) / dt);
            }
        }
        if (!rates.empty()) rate_ = np_median(rates);
    }
    std::optional<DateTime> operator()(size_t pos) const
    {
        if (t_.empty() || !rate_ || *rate_ == 0) return std::nullopt;
        size_t i = 0;
        while (i < t_.size() && t_[i].first < pos) ++i;  // searchsorted left
        const std::pair<size_t, DateTime>* best = nullptr;
        for (long k : {static_cast<long>(i) - 1, static_cast<long>(i)}) {
            if (k < 0 || k >= static_cast<long>(t_.size())) continue;
            const auto& c = t_[static_cast<size_t>(k)];
            if (!best || std::llabs(static_cast<long long>(c.first) - static_cast<long long>(pos)) <
                             std::llabs(static_cast<long long>(best->first) - static_cast<long long>(pos))) {
                best = &c;
            }
        }
        return best->second.plus_seconds((static_cast<double>(pos) - static_cast<double>(best->first)) / *rate_);
    }
    struct Gap {
        size_t p0;
        DateTime t0, t1;
        double fehl;
    };
    std::vector<Gap> schnitte() const
    {
        std::vector<Gap> out;
        if (!rate_) return out;
        for (size_t k = 0; k + 1 < t_.size(); ++k) {
            const double dt = static_cast<double>(t_[k + 1].second.total_us() - t_[k].second.total_us()) / 1e6;
            const double erwartet = static_cast<double>(t_[k + 1].first - t_[k].first) / *rate_;
            if (dt - erwartet > 30 || dt < 0) out.push_back({t_[k].first, t_[k].second, t_[k + 1].second, dt - erwartet});
        }
        return out;
    }
    std::optional<double> rate() const { return rate_; }

private:
    std::vector<std::pair<size_t, DateTime>> t_;
    std::optional<double> rate_;
};

std::array<std::string, 3> kopf_felder(const Bytes& p, size_t i)
{
    std::array<std::string, 3> f;
    for (int k = 0; k < 3; ++k) {
        const size_t n = i < p.size() ? p[i] : 0;
        if (i + 1 + n <= p.size()) {
            // decode cp437, then str.strip(); strip works on the 1:1 bytes
            size_t a = i + 1, e = i + 1 + n;
            auto ws = [](uint8_t c) { return c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F) || c == 0xFF; };
            while (a < e && ws(p[a])) ++a;
            while (e > a && ws(p[e - 1])) --e;
            f[static_cast<size_t>(k)] = cp437_to_utf8(p.data() + a, e - a);
        }
        i += 1 + n;
    }
    return f;
}

std::string kopf_text(const std::string& kommando, const std::string& kommentar)
{
    std::string s;
    if (!kommando.empty()) s += ", Kommando '" + kommando + "'";
    if (!kommentar.empty()) s += ", Kommentar '" + kommentar + "'";
    return s;
}

// list id -> block -> counter of 32-byte segments
using BitLists = std::map<int, std::map<int64_t, Counter<Bytes>>>;

BitLists bitlisten(const std::vector<const Pkt*>& ok, uint8_t typ)
{
    BitLists lists;
    const size_t W = 32;
    for (const Pkt* x : ok) {
        const Bytes& p = x->pay;
        if (x->A == 0x00 && p[0] == typ && p.size() >= 16) {
            const int64_t blk = p[2] | p[3] << 8;
            const int64_t n = p[5] | p[6] << 8;
            const Bytes u = unrun(p, 7);
            for (int64_t k = 0; k < n; ++k) {
                const Bytes seg = sub(u, static_cast<size_t>(k) * W, static_cast<size_t>(k + 1) * W);
                if (seg.size() == W) lists[p[1]][blk + k].add(seg);
            }
        }
    }
    return lists;
}

Bytes liste_bytes(const std::map<int64_t, Counter<Bytes>>& d)
{
    Bytes out;
    const int64_t H = d.rbegin()->first + 1;
    for (int64_t y = 0; y < H; ++y) {
        auto it = d.find(y);
        if (it != d.end()) {
            const Bytes b = it->second.most_common()[0].first;
            out.insert(out.end(), b.begin(), b.end());
        } else {
            out.insert(out.end(), 32, 0);
        }
    }
    return out;
}

std::vector<uint8_t> unpackbits(const Bytes& data)
{
    std::vector<uint8_t> b;
    b.reserve(data.size() * 8);
    for (uint8_t v : data) {
        for (int i = 7; i >= 0; --i) b.push_back((v >> i) & 1);
    }
    return b;
}

int64_t count(const std::vector<uint8_t>& b)
{
    int64_t n = 0;
    for (uint8_t v : b) n += v;
    return n;
}

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void be32(Bytes& b, uint32_t v)
{
    for (int i = 3; i >= 0; --i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void chunk(Bytes& png, const char* type, const Bytes& data)
{
    be32(png, static_cast<uint32_t>(data.size()));
    Bytes td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    png.insert(png.end(), td.begin(), td.end());
    be32(png, crc32(td.data(), td.size()));
}

} // namespace

bool write_png_gray(const std::string& path, const std::vector<uint8_t>& pixels, size_t width, size_t height)
{
    Bytes png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Bytes ihdr;
    be32(ihdr, static_cast<uint32_t>(width));
    be32(ihdr, static_cast<uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    Bytes raw;
    for (size_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), pixels.begin() + static_cast<long>(y * width),
                   pixels.begin() + static_cast<long>((y + 1) * width));
    }
    // zlib stream with stored blocks
    Bytes z = {0x78, 0x01};
    size_t off = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<long>(off), raw.begin() + static_cast<long>(off + n));
        off += n;
    } while (off < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return write_file(path, png);
}

void analyse_dienste(const std::vector<Pkt>& pk, const std::string& outdir, const LogFn& log)
{
    std::vector<const Pkt*> ok;
    for (const Pkt& x : pk) {
        if (x.ok && !x.pay.empty()) ok.push_back(&x);
    }
    make_dirs(outdir);
    std::vector<std::string> rep;
    auto w = [&](const std::string& s) { rep.push_back(s); };

    // --- Uhrzeit ---
    std::vector<std::pair<size_t, DateTime>> ticks;
    for (const Pkt* x : ok) {
        const Bytes& p = x->pay;
        if (x->A == 0x01 && p[0] == 0x06 && p.size() == 8) {
            if (auto t = DateTime::make(1900 + p[1], p[2], p[3], p[4], p[5], p[6])) ticks.emplace_back(x->pos, *t);
        }
    }
    std::sort(ticks.begin(), ticks.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second < b.second;
    });
    std::vector<std::pair<size_t, DateTime>> gut;
    for (size_t k = 0; k < ticks.size(); ++k) {
        const size_t a = k >= 3 ? k - 3 : 0;
        const size_t e = std::min(ticks.size(), k + 4);
        int close = 0, nb = 0;
        for (size_t j = a; j < e; ++j) {
            if (j == k) continue;
            ++nb;
            const double d = static_cast<double>(ticks[k].second.total_us() - ticks[j].second.total_us()) / 1e6;
            if (std::fabs(d) < 900) ++close;
        }
        if (close >= std::min(2, nb)) gut.push_back(ticks[k]);
    }
    const Uhr uhr(gut);
    w("UHRZEIT (Strom 01, Typ 06)");
    if (!gut.empty()) {
        w(strf("  %zu Zeitstempel, %s bis %s%s", gut.size(), gut.front().second.format("%d.%m.%Y %H:%M:%S").c_str(),
               gut.back().second.format("%H:%M:%S").c_str(),
               uhr.rate() && *uhr.rate() != 0 ? strf(", %.0f Bytes/s", *uhr.rate()).c_str() : ""));
        for (const auto& g : uhr.schnitte()) {
            w(strf("  Aufnahmeluecke bei Byte %zu: %s -> %s (ca. %lld s fehlen)", g.p0, hm(g.t0).c_str(), hm(g.t1).c_str(),
                   static_cast<long long>(g.fehl)));
        }
    } else {
        w("  keine Uhrzeit-Pakete gefunden");
    }

    // --- Datum ---
    w("");
    w("DATUM");
    auto date_key = [](const DateTime& t) { return t.year * 10000 + t.month * 100 + t.day; };
    auto date_str = [](int k) { return strf("%02d.%02d.%04d", k % 100, (k / 100) % 100, k / 10000); };
    Counter<int> tage_uhr, tage_kopf, tage_zaehler;
    for (const auto& [pos, t] : gut) tage_uhr.add(date_key(t));
    for (const Pkt* x : ok) {
        if ((x->B & 0x80) && (x->pay[0] == 0x00 || x->pay[0] == 0x01 || x->pay[0] == 0x09) && x->pay.size() >= 8) {
            if (auto t = dos_dt(x->pay, 1)) tage_kopf.add(date_key(*t));
        }
    }
    for (const Pkt* x : ok) {
        if (x->A == 0x00 && x->pay[0] == 0x0B && x->pay.size() == 8) {
            const DateTime base = *DateTime::make(1989, 12, 31);
            tage_zaehler.add(date_key(base.plus_days(x->pay[1] | x->pay[2] << 8)));
        }
    }
    const std::pair<const char*, const Counter<int>*> tage_all[] = {{"Uhr (Strom 01 Typ 06)", &tage_uhr},
                                                                     {"Sendungskoepfe", &tage_kopf},
                                                                     {"Tageszaehler (Strom 00 Typ 0B)", &tage_zaehler}};
    for (const auto& [name, tage] : tage_all) {
        if (tage->empty()) continue;
        std::string s;
        const auto mc = tage->most_common();
        for (size_t i = 0; i < std::min<size_t>(4, mc.size()); ++i) {
            s += (i ? ", " : "") + strf("%s (%lldx)", date_str(mc[i].first).c_str(), static_cast<long long>(mc[i].second));
        }
        w(strf("  %s: %s", name, s.c_str()));
    }
    if (!tage_uhr.empty() && !tage_kopf.empty() &&
        tage_uhr.most_common()[0].first != tage_kopf.most_common()[0].first) {
        w("  ACHTUNG: Uhr und Sendungskoepfe nennen verschiedene Tage. Die "
          "Aufnahme kann aus mehreren Teilen bestehen, die Uhrzeiten im Bericht "
          "sind dann unzuverlaessig.");
    }

    // --- Sendungsablauf ---
    w("");
    w("SENDUNGSABLAUF");
    std::vector<std::pair<size_t, std::string>> ereig;
    Counter<std::pair<int, int>> verwaltung;
    for (const Pkt* x : ok) {
        const Bytes& p = x->pay;
        if (x->A == 0x00 && (p[0] == 0x01 || p[0] == 0x02) && p.size() == 8) {
            verwaltung.add({p[0], p[1]});
        } else if (x->A == 0x00 && (p[0] == 0x81 || p[0] == 0x82) && p.size() == 8) {
            ereig.emplace_back(x->pos, strf("Steuerung: %-7s Strom %02X", p[0] == 0x82 ? "Beginn" : "Ende", p[1]));
        } else if ((x->B & 0x80) && p[0] == 0x00 && p.size() > 9) {
            const auto f = kopf_felder(p, 7);
            ereig.emplace_back(x->pos, strf("Strom %02X Sendungskopf '%s', %d Datei(en)%s, Sendezeit %s", x->A,
                                            f[0].c_str(), p[5] | p[6] << 8, kopf_text(f[1], f[2]).c_str(),
                                            hm(dos_dt(p, 1)).c_str()));
        } else if ((x->B & 0x80) && p[0] == 0x09 && p.size() > 16) {
            const TransmissionRecord kopf = unwrap_transmission(p);
            if (kopf.checksum_ok) {
                const auto f = kopf_felder(p, 15);
                ereig.emplace_back(x->pos, strf("Strom %02X Bezahlsendung '%s', DM %lld,%02lld%s, Sendezeit %s", x->A,
                                                f[0].c_str(), static_cast<long long>(kopf.price_raw / 100),
                                                static_cast<long long>(kopf.price_raw % 100),
                                                kopf_text(f[1], f[2]).c_str(), hm(dos_dt(p, 1)).c_str()));
            }
        } else if ((x->B & 0x80) && p[0] == 0x01 && p.size() == 8) {
            ereig.emplace_back(x->pos, strf("Strom %02X Sendungsende, Zeit %s", x->A, hm(dos_dt(p, 1)).c_str()));
        }
    }
    std::sort(ereig.begin(), ereig.end());
    std::optional<std::string> letzte;
    for (const auto& [pos, txt] : ereig) {
        if (letzte && txt == *letzte) continue;
        letzte = txt;
        w(strf("  %s  %s", hm(uhr(pos)).c_str(), txt.c_str()));
    }
    w("");
    w("  Aktive Datenstroeme (Dateiuebertragung):");
    std::map<std::pair<int, int>, std::vector<size_t>> akt;
    for (const Pkt* x : ok) {
        const uint8_t t = x->pay[0];
        if (x->A != 0x00 && (t == 0x02 || t == 0x03 || t == 0x04 || t == 0x0C)) akt[{x->A, x->B >> 7}].push_back(x->pos);
    }
    for (const auto& [key, ps] : akt) {
        if (ps.size() < 3) continue;
        w(strf("    Strom %02X/%d: %s - %s, %zu Pakete", key.first, key.second,
               hm(uhr(*std::min_element(ps.begin(), ps.end()))).c_str(),
               hm(uhr(*std::max_element(ps.begin(), ps.end()))).c_str(), ps.size()));
    }

    // --- Bitlisten ---
    const BitLists lists = bitlisten(ok, 0x00);
    w("");
    w("BITLISTEN (Strom 00, Typ 00) - vermutlich Freischaltung je Dienst,");
    w("ein Bit je Decoder-/Teilnehmernummer (Bit 7 des ersten Bytes = Nr. 0)");
    std::map<int, std::vector<uint8_t>> bits;
    for (const auto& [lid, d] : lists) {
        const int64_t H = d.rbegin()->first + 1;
        const Bytes data = liste_bytes(d);
        const int64_t fehl = H - static_cast<int64_t>(d.size());
        const auto b = unpackbits(data);
        bits[lid] = b;
        const std::string name = strf("bitliste_%02X.bin", lid);
        write_file(join_path(outdir, name), data);
        std::vector<size_t> nz;
        for (size_t i = 0; i < b.size(); ++i) {
            if (b[i]) nz.push_back(i);
        }
        w(strf("  Liste %02X: %zu Bit (%lld Bloecke, %lld fehlen), %zu gesetzt, Nummern %s -> %s", lid, b.size(),
               static_cast<long long>(H), static_cast<long long>(fehl), nz.size(),
               nz.empty() ? "-" : strf("%zu-%zu", nz.front(), nz.back()).c_str(), name.c_str()));
        const size_t h = (b.size() + 255) / 256;
        std::vector<uint8_t> img(h * 256, 255);
        for (size_t i = 0; i < b.size(); ++i) img[i] = static_cast<uint8_t>(255 - b[i] * 255);
        write_png_gray(join_path(outdir, strf("bitliste_%02X.png", lid)), img, 256, h);
    }
    if (bits.count(0x00)) {
        const auto& b0 = bits[0x00];
        std::vector<size_t> nz;
        for (size_t i = 0; i < b0.size(); ++i) {
            if (!b0[i]) nz.push_back(i);
        }
        if (!nz.empty()) {
            std::string s;
            for (const auto& [k, bk] : bits) {
                if (!k) continue;
                int64_t n = 0;
                for (size_t i = 0; i < std::min(bk.size(), b0.size()); ++i) n += (bk[i] && !b0[i]) ? 1 : 0;
                s += (s.empty() ? "" : ", ") + strf("%02X: %lld", k, static_cast<long long>(n));
            }
            w(strf("    Liste 00: %zu Nummern NICHT gesetzt (%zu-%zu); davon in anderen Listen: %s -> Liste 00 "
                   "wirkt wie eine Sperrliste (0 = gesperrt)",
                   nz.size(), nz.front(), nz.back(), s.c_str()));
        }
    }
    for (const auto& [a, ba_all] : bits) {
        for (const auto& [c, bc_all] : bits) {
            if (a == c) continue;
            const size_t n = std::min(ba_all.size(), bc_all.size());
            int64_t sa = 0, out = 0;
            for (size_t i = 0; i < n; ++i) {
                sa += ba_all[i];
                out += (ba_all[i] && !bc_all[i]) ? 1 : 0;
            }
            if (sa && static_cast<double>(out) <= 0.1 * static_cast<double>(sa)) {
                w(strf("    Liste %02X ist (fast) Teilmenge von Liste %02X (%lld von %lld Bit ausserhalb)", a, c,
                       static_cast<long long>(out), static_cast<long long>(sa)));
            }
        }
    }
    if (!verwaltung.empty()) {
        w("  Listen ohne Daten (Strom 00, Typ 01/02) - vermutlich Platzhalter:");
        auto items = verwaltung.items();
        std::sort(items.begin(), items.end());
        for (const auto& [tw, c] : items) {
            w(strf("    Liste %02X: Typ %02X, %lldx%s", tw.second, tw.first, static_cast<long long>(c),
                   bits.count(tw.second) ? ", aber auch als Daten empfangen" : ""));
        }
    }

    // --- Bitlisten je Sendung (Typ 80) ---
    const BitLists sendungslisten = bitlisten(ok, 0x80);
    if (!sendungslisten.empty()) {
        w("");
        w("BITLISTEN JE SENDUNG (Strom 00, Typ 80) - Format wie Typ 00, Byte 1 = Datenstrom;");
        w("vermutlich die Freischaltung fuer diese Sendung, kurz vor Sendungsbeginn");
        for (const auto& [lid, d] : sendungslisten) {
            const Bytes data = liste_bytes(d);
            const auto b = unpackbits(data);
            const std::string name = strf("bitliste_strom_%02X.bin", lid);
            write_file(join_path(outdir, name), data);
            std::vector<size_t> nz;
            for (size_t i = 0; i < b.size(); ++i) {
                if (b[i]) nz.push_back(i);
            }
            std::vector<std::string> bezug;
            const int64_t sb = count(b);
            for (const auto& [k, bk] : bits) {
                const size_t n = std::min(b.size(), bk.size());
                int64_t out = 0;
                for (size_t i = 0; i < n; ++i) out += (b[i] && !bk[i]) ? 1 : 0;
                if (sb && !out) {
                    bezug.push_back(strf("%s Liste %02X", sb == count(bk) ? "gleich" : "Teilmenge von", k));
                }
            }
            std::string bz;
            for (const auto& s : bezug) bz += (bz.empty() ? "" : ", ") + s;
            const int64_t H = d.rbegin()->first + 1;
            w(strf("  Strom %02X: %zu Bit (%lld Bloecke, %lld fehlen), %zu gesetzt%s%s -> %s", lid, b.size(),
                   static_cast<long long>(H), static_cast<long long>(H - static_cast<int64_t>(d.size())), nz.size(),
                   nz.size() == 1 ? strf(" (Nr. %zu)", nz[0]).c_str() : "", bezug.empty() ? "" : (", " + bz).c_str(),
                   name.c_str()));
        }
    }

    // --- verschluesselte / unbekannte Dienste ---
    w("");
    w("WEITERE DIENSTE (Rohdaten in dienst_*.bin)");
    std::map<std::pair<int, int>, std::vector<const Pkt*>> gruppen;
    for (const Pkt* x : ok) {
        const int t = x->pay[0];
        if ((x->A == 0x01 && t == 0x05) || (x->A == 0x03 && (t == 0x07 || t == 0x08 || t == 0x0A))) {
            gruppen[{x->A, t}].push_back(x);
        }
    }
    std::map<std::pair<int, int>, std::string> texte = {
        {{0x01, 0x05}, "unbekannt; jedes 2. Byte 7 Bit (15-Bit-Werte), keine Wiederholungen"},
        {{0x03, 0x07}, "Kontoguthaben; VideoDat-DES, 8 Runden"},
        {{0x03, 0x08}, "Kontoguthaben; VideoDat-DES, 8 Runden"},
        {{0x03, 0x0A}, "Dispo-Aenderung; VideoDat-DES, 8 Runden"}};
    for (const auto& [key, xs] : gruppen) {
        std::vector<std::pair<Bytes, std::vector<size_t>>> verschieden;
        std::map<Bytes, size_t> vidx;
        for (const Pkt* x : xs) {
            auto it = vidx.find(x->pay);
            if (it == vidx.end()) {
                vidx.emplace(x->pay, verschieden.size());
                verschieden.push_back({x->pay, {x->pos}});
            } else {
                verschieden[it->second].second.push_back(x->pos);
            }
        }
        const std::string name = strf("dienst_%02X_typ%02X.bin", key.first, key.second);
        Bytes out;
        for (const auto& [pay, ps] : verschieden) {
            out.push_back(static_cast<uint8_t>(pay.size() & 0xFF));
            out.push_back(static_cast<uint8_t>((pay.size() >> 8) & 0xFF));
            out.insert(out.end(), pay.begin(), pay.end());
        }
        write_file(join_path(outdir, name), out);
        if (key == std::make_pair(0x01, 0x05)) {
            int matched = 0;
            Counter<std::string> classifications;
            const std::string report_name = "dienst_01_typ05_analysis.jsonl";
            std::string text;
            for (const auto& [pay, positions] : verschieden) {
                PrngResult res = analyse_type05(pay);
                Json record;
                if (res.valid) {
                    record = res.record;
                } else {
                    record = Json::object();
                    record["kind"] = "unknown";
                    record["matches_prng"] = false;
                    record["error"] = res.error;
                }
                const bool m = record.find("matches_prng")->as_bool();
                matched += m ? 1 : 0;
                classifications.add(record.find("kind")->as_string());
                Json ps = Json::array();
                for (size_t p : positions) ps.push_back(p);
                record["positions"] = ps;
                record["copies"] = positions.size();
                text += record.dump() + "\n";
            }
            write_text(join_path(outdir, report_name), text);
            std::string cls = "{";
            for (size_t i = 0; i < classifications.items().size(); ++i) {
                const auto& [k, n] = classifications.items()[i];
                cls += (i ? ", " : "") + strf("'%s': %lld", k.c_str(), static_cast<long long>(n));
            }
            cls += "}";
            texte[key] = strf("%d/%zu exakt passende PRNG-Folgen; %s -> %s", matched, verschieden.size(), cls.c_str(),
                              report_name.c_str());
        }
        w(strf("  Strom %02X Typ %02X: %zu Pakete, %zu verschieden, %s - %s; %s -> %s", key.first, key.second,
               xs.size(), verschieden.size(), hm(uhr(xs.front()->pos)).c_str(), hm(uhr(xs.back()->pos)).c_str(),
               texte[key].c_str(), name.c_str()));
    }
    w("  (Format der .bin: je Paket 2 Byte Laenge LE + Nutzlast)");
    std::set<Bytes> fuell;
    for (const Pkt* x : ok) {
        if (x->pay[0] == 0xFF && x->pay.size() == 8) fuell.insert(x->pay);
    }
    if (!fuell.empty()) {
        int n = 0;
        for (const auto& p : fuell) n += matches_type_ff(p) ? 1 : 0;
        w(strf("  Typ FF (je Sendung, Zaehler 07/08): %zu verschieden, %d passen zum "
               "Zufallsgenerator von Strom 01 Typ 05 (Fuelldaten)",
               fuell.size(), n));
    }
    std::string text;
    for (const auto& line : rep) text += line + "\n";
    write_text(join_path(outdir, "dienste.txt"), text);
    log("");
    for (const auto& line : rep) log(line);
}

} // namespace videodat
