/*
 * File:        vd_signal.cpp
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Signal layer (port of the TBC path of videodat_decoder.py)
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "vd_signal.h"

#include <cmath>
#include <optional>
#include <tuple>

namespace videodat {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr int kTaps = 9;     // EQ_TAPS = arange(-4, 5) * 0.5
constexpr int kMid = kTaps / 2;
constexpr int kCols = kTaps + 1;

struct Phase {
    double e0, thr, lo, hi;
};

std::optional<Phase> line_phase(const std::vector<double>& l, const Fmt& fmt,
                                std::optional<double> prior = std::nullopt)
{
    const double spb = fmt.spb;
    const int nb = fmt.nbits;
    const long n = static_cast<long>(l.size());
    const long s0 = fmt.search[0];
    const double lo = np_median(std::vector<double>(l.begin() + fmt.lo_rng[0], l.begin() + fmt.lo_rng[1]));
    const long hi_end = std::min(n, s0 + static_cast<long>((nb + 5) * spb));
    const double hi = np_percentile(std::vector<double>(l.begin() + s0, l.begin() + std::max(hi_end, s0)), 90.0);
    if (hi - lo < fmt.min_swing) return std::nullopt;
    const double thr = (lo + hi) / 2;
    double e0;
    if (!prior) {
        long p = s0 + 1;
        while (p < fmt.search[1] && !(l[static_cast<size_t>(p - 1)] < thr && thr <= l[static_cast<size_t>(p)])) ++p;
        if (p >= fmt.search[1]) return std::nullopt;
        const double a = l[static_cast<size_t>(p - 1)], b = l[static_cast<size_t>(p)];
        e0 = static_cast<double>(p - 1) + (thr - a) / (b - a);
    } else {
        e0 = *prior;
    }
    // refine the phase over all edges of the line
    const long a = std::max(static_cast<long>(std::trunc(e0)) - 3, 0L);
    const long E = std::min(a + static_cast<long>((nb + 1) * spb), n - 2);
    std::vector<double> z;  // interleaved complex
    size_t count = 0;
    std::vector<long> idx;
    for (long i = a; i < E; ++i) {
        if ((l[static_cast<size_t>(i)] >= thr) != (l[static_cast<size_t>(i + 1)] >= thr)) idx.push_back(i);
    }
    if (idx.size() > 4) {
        const double two_pi = 2.0 * kPi;
        const double scl = 1.0 / spb;
        z.reserve(2 * idx.size());
        for (long i : idx) {
            const double li = l[static_cast<size_t>(i)], li1 = l[static_cast<size_t>(i + 1)];
            const double fr = static_cast<double>(i) + (thr - li) / (li1 - li);
            const double theta = (two_pi * (fr - e0)) * scl;
            z.push_back(std::cos(theta));
            z.push_back(std::sin(theta));
            ++count;
        }
        double re, im;
        np_complex_sum(z.data(), count, re, im);
        const double inv = 1.0 / static_cast<double>(count);
        const double ph = std::atan2(im * inv, re * inv);
        e0 += ph * spb / (2 * kPi);
    }
    if (!std::isfinite(e0) || e0 < 0 || e0 + nb * spb >= static_cast<double>(n - 1)) return std::nullopt;
    return Phase{e0, thr, lo, hi};
}

int framing(const std::vector<double>& l, const Phase& ph, const Fmt& fmt)
{
    int a = 0;
    for (int b = 0; b < fmt.nbytes; ++b) {
        const double k = b * 10;
        const size_t ist = static_cast<size_t>(ph.e0 + (k + 0.5) * fmt.spb);
        const size_t isp = static_cast<size_t>(ph.e0 + (k + 9.5) * fmt.spb);
        if (l[ist] >= ph.thr) ++a;
        if (!(l[isp] >= ph.thr)) ++a;
    }
    return std::max(a, 2 * fmt.nbytes - a);
}

int framing_full(const Fmt& fmt) { return 2 * fmt.nbytes; }

// Rows of (len(EQ_TAPS) + 1) features per bit.
using Features = std::vector<double>;  // nbits * kCols

Features tbc_features(const std::vector<double>& l, const Phase& ph, const Fmt& fmt)
{
    const int nb = fmt.nbits;
    const double maxx = static_cast<double>(l.size() - 2);
    std::vector<double> s(static_cast<size_t>(nb) * kTaps);
    for (int b = 0; b < nb; ++b) {
        for (int t = 0; t < kTaps; ++t) {
            const double tap = (t - 4) * 0.5;
            double x = ph.e0 + ((b + 0.5) + tap) * fmt.spb;
            x = std::min(std::max(x, 0.0), maxx);
            const size_t ia = static_cast<size_t>(x);
            const double f = x - static_cast<double>(ia);
            s[static_cast<size_t>(b) * kTaps + t] = l[ia] * (1 - f) + l[ia + 1] * f;
        }
    }
    double t = ph.thr;
    for (int it = 0; it < 5; ++it) {
        std::vector<double> a, b;
        for (int i = 0; i < nb; ++i) {
            const double c = s[static_cast<size_t>(i) * kTaps + kMid];
            (c < t ? a : b).push_back(c);
        }
        if (a.empty() || b.empty()) break;
        t = (np_mean(a.data(), a.size()) + np_mean(b.data(), b.size())) / 2;
    }
    Features X(static_cast<size_t>(nb) * kCols);
    const double swing = ph.hi - ph.lo;
    for (int i = 0; i < nb; ++i) {
        for (int k = 0; k < kTaps; ++k) {
            X[static_cast<size_t>(i) * kCols + k] = (s[static_cast<size_t>(i) * kTaps + k] - t) / swing;
        }
        X[static_cast<size_t>(i) * kCols + kTaps] = 1.0;
    }
    return X;
}

struct Equalizer {
    std::vector<double> w;  // kCols
    double bias{0};
};

// Per-bit confidences (incl. start/stop bits) -> bytes + 8 confidences each.
LineBits soft100_to_line(const std::vector<double>& y)
{
    LineBits r;
    const size_t nbytes = y.size() / 10;
    r.bytes.resize(nbytes);
    r.soft.resize(nbytes * 8);
    for (size_t k = 0; k < nbytes; ++k) {
        unsigned v = 0;
        for (int j = 0; j < 8; ++j) {
            const float f = static_cast<float>(y[10 * k + 1 + static_cast<size_t>(j)]);
            r.soft[8 * k + static_cast<size_t>(j)] = f;
            if (f >= 0.0f) v |= 1u << j;
        }
        r.bytes[k] = static_cast<uint8_t>(v ^ 0xFF);
    }
    return r;
}

std::vector<double> mid_column(const Features& X, int nb)
{
    std::vector<double> y(static_cast<size_t>(nb));
    for (int i = 0; i < nb; ++i) y[static_cast<size_t>(i)] = X[static_cast<size_t>(i) * kCols + kMid];
    return y;
}

std::vector<double> apply_eq(const Features& X, int nb, const std::vector<double>& w, double bias)
{
    std::vector<double> y(static_cast<size_t>(nb));
    for (int i = 0; i < nb; ++i) {
        double s = 0.0;
        for (int k = 0; k < kCols; ++k) s += X[static_cast<size_t>(i) * kCols + k] * w[static_cast<size_t>(k)];
        y[static_cast<size_t>(i)] = s + bias;
    }
    return y;
}

std::optional<LineBits> slice_tbc(const std::vector<double>& l, std::optional<double> prior,
                                  const Equalizer* eq, const Fmt& fmt)
{
    const auto ph = line_phase(l, fmt, prior);
    if (!ph) return std::nullopt;
    const Features X = tbc_features(l, *ph, fmt);
    return soft100_to_line(eq ? apply_eq(X, fmt.nbits, eq->w, eq->bias) : mid_column(X, fmt.nbits));
}

std::optional<double> hsync_end(const std::vector<double>& l)
{
    const double tip = np_median(std::vector<double>(l.begin() + 10, l.begin() + 60));
    const double blank = np_median(std::vector<double>(l.begin() + 100, l.begin() + 190));
    if (blank - tip < 3000) return std::nullopt;
    const double mid = (tip + blank) / 2;
    for (size_t k = 0; k + 1 < 70; ++k) {
        const size_t p = 60 + k;
        if (l[p] < mid && l[p + 1] >= mid) return static_cast<double>(p) + (mid - l[p]) / (l[p + 1] - l[p]);
    }
    return std::nullopt;
}

Stream make_stream(const std::vector<LineBits>& lines)
{
    Stream st;
    for (const auto& r : lines) {
        st.data.insert(st.data.end(), r.bytes.begin(), r.bytes.end());
        st.soft.insert(st.soft.end(), r.soft.begin(), r.soft.end());
    }
    return st;
}

// Least squares via normal equations, solved in long double.
std::vector<double> lstsq(const std::vector<const Features*>& rows, int nb)
{
    long double ata[kCols][kCols] = {};
    long double aty[kCols] = {};
    for (const Features* X : rows) {
        for (int i = 0; i < nb; ++i) {
            const double* r = X->data() + static_cast<size_t>(i) * kCols;
            const long double y = r[kMid] >= 0 ? 1.0L : -1.0L;
            for (int a = 0; a < kCols; ++a) {
                aty[a] += r[a] * y;
                for (int b = a; b < kCols; ++b) ata[a][b] += static_cast<long double>(r[a]) * r[b];
            }
        }
    }
    for (int a = 0; a < kCols; ++a) {
        for (int b = 0; b < a; ++b) ata[a][b] = ata[b][a];
    }
    // Gaussian elimination with partial pivoting
    long double m[kCols][kCols + 1];
    for (int a = 0; a < kCols; ++a) {
        for (int b = 0; b < kCols; ++b) m[a][b] = ata[a][b];
        m[a][kCols] = aty[a];
    }
    for (int c = 0; c < kCols; ++c) {
        int piv = c;
        for (int r = c + 1; r < kCols; ++r) {
            if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
        }
        if (piv != c) {
            for (int k = 0; k <= kCols; ++k) std::swap(m[c][k], m[piv][k]);
        }
        if (m[c][c] == 0) continue;
        for (int r = 0; r < kCols; ++r) {
            if (r == c) continue;
            const long double f = m[r][c] / m[c][c];
            for (int k = c; k <= kCols; ++k) m[r][k] -= f * m[c][k];
        }
    }
    std::vector<double> w(kCols);
    for (int c = 0; c < kCols; ++c) w[static_cast<size_t>(c)] = m[c][c] == 0 ? 0.0 : static_cast<double>(m[c][kCols] / m[c][c]);
    return w;
}

std::optional<Equalizer> train_equalizer(const FieldSource& F, const std::vector<std::vector<int>>& sets,
                                         std::optional<double> offset, size_t start, size_t nf,
                                         const LogFn& log, const std::atomic<bool>* cancel)
{
    const size_t chunk = 2500, max_fields = 30000, need = 3000;
    const Fmt fmt = Fmt::tbc_pal("1993");
    const int nb = fmt.nbits;
    std::vector<Features> X;
    std::vector<bool> good;
    size_t n_simple = 0, ngood = 0;
    const size_t end = std::min(nf, start + max_fields);
    size_t fi = start;
    std::vector<double> l(F.width());
    while (fi < end) {
        for (size_t fj = fi; fj < std::min(fi + chunk, end); ++fj) {
            if (cancel && cancel->load()) return std::nullopt;
            for (int li : sets[F.is_first_field(fj) ? 0 : 1]) {
                F.read_line(fj, static_cast<size_t>(li), l.data());
                std::optional<double> prior;
                if (offset) {
                    if (auto s = hsync_end(l)) prior = *s + *offset;
                }
                if (auto ph = line_phase(l, fmt, prior)) X.push_back(tbc_features(l, *ph, fmt));
            }
        }
        fi = std::min(fi + chunk, end);
        std::vector<LineBits> lines;
        lines.reserve(X.size());
        for (const auto& Xi : X) lines.push_back(soft100_to_line(mid_column(Xi, nb)));
        const Stream st = make_stream(lines);
        good.assign(X.size(), false);
        n_simple = 0;
        for (const Pkt& x : find_packets(st)) {
            if (!x.ok) continue;
            const long from = static_cast<long>((x.pos + 9) / 10);
            const long to = std::min(static_cast<long>((x.pos + x.raw.size()) / 10), static_cast<long>(good.size()));
            for (long i = from; i < to; ++i) good[static_cast<size_t>(i)] = true;
            ++n_simple;
        }
        ngood = static_cast<size_t>(std::count(good.begin(), good.end(), true));
        if (ngood >= need) break;
    }
    if (ngood < 200) {
        log(strf("Entzerrer: zu wenige fehlerfreie Zeilen (%zu), verwende einfache Schwelle", ngood));
        return std::nullopt;
    }
    std::vector<const Features*> rows;
    for (size_t i = 0; i < X.size(); ++i) {
        if (good[i]) rows.push_back(&X[i]);
    }
    Equalizer eq;
    eq.w = lstsq(rows, nb);
    std::vector<std::vector<double>> yall;
    yall.reserve(X.size());
    for (const auto& Xi : X) yall.push_back(apply_eq(Xi, nb, eq.w, 0.0));
    double biases[15];
    double hits[15];
    for (int k = 0; k < 15; ++k) {
        biases[k] = std::nearbyint((0.0 + k * 0.05) * 100.0) / 100.0;
        std::vector<LineBits> lines;
        lines.reserve(yall.size());
        for (const auto& y : yall) {
            std::vector<double> yb(y.size());
            for (size_t i = 0; i < y.size(); ++i) yb[i] = y[i] + biases[k];
            lines.push_back(soft100_to_line(yb));
        }
        size_t h = 0;
        for (const Pkt& x : find_packets(make_stream(lines))) h += x.ok ? 1 : 0;
        hits[k] = static_cast<double>(h);
    }
    // np.convolve(np.pad(hits, 1, "edge"), ones(3) / 3, "valid")
    double pad[17];
    pad[0] = hits[0];
    for (int k = 0; k < 15; ++k) pad[k + 1] = hits[k];
    pad[16] = hits[14];
    const double third = 1.0 / 3.0;
    int best = 0;
    double bestv = -1;
    for (int k = 0; k < 15; ++k) {
        const double v = pad[k] * third + pad[k + 1] * third + pad[k + 2] * third;
        if (v > bestv) {
            bestv = v;
            best = k;
        }
    }
    eq.bias = biases[best];
    log(strf("Entzerrer aus %zu fehlerfreien Zeilen (Halbbilder %zu-%zu) gelernt, "
             "Schwellenverschiebung %.2f: %d statt %zu fehlerfreie Pakete im Lernabschnitt",
             ngood, start, fi, biases[best], static_cast<int>(hits[best]), n_simple));
    return eq;
}

std::optional<Fmt> choose_variant_probe(const FieldSource& F, size_t nf, size_t max_fields, size_t maxline,
                                        const std::string& variant, const LogFn& log)
{
    if (variant != "auto") return Fmt::tbc_pal(variant);
    const Fmt f93 = Fmt::tbc_pal("1993"), f90 = Fmt::tbc_pal("1990");
    long h93 = 0, h90 = 0;
    std::vector<double> l(F.width());
    for (size_t fi = 0; fi < std::min(nf, max_fields); fi += 10) {
        for (size_t li = 0; li < maxline; ++li) {
            F.read_line(fi, li, l.data());
            if (auto ph = line_phase(l, f93); ph && framing(l, *ph, f93) >= f93.frame_min) ++h93;
            if (auto ph = line_phase(l, f90); ph && framing(l, *ph, f90) >= f90.frame_min) ++h90;
        }
    }
    const bool is93 = h93 >= h90;  // tie: "1993" > "1990"
    log(strf("Variante %s (gerahmte Zeilen in der Stichprobe: 1990: %ld, 1993: %ld)", is93 ? "1993" : "1990", h90, h93));
    return is93 ? f93 : f90;
}

std::vector<int> select_lines(const std::vector<long>& sc)
{
    const long best = *std::max_element(sc.begin(), sc.end());
    std::vector<int> out;
    if (best < 10) return out;
    for (size_t k = 0; k < sc.size(); ++k) {
        if (static_cast<double>(sc[k]) >= static_cast<double>(best) * 0.3) out.push_back(static_cast<int>(k));
    }
    return out;
}

} // namespace

Fmt Fmt::tbc_pal(const std::string& variant)
{
    Fmt f;
    f.fs = 17.734475e6;
    f.variant = variant;
    const bool v93 = variant != "1990";
    f.spb = f.fs / (v93 ? 2.0e6 : 0.8e6);
    f.nbytes = v93 ? 10 : 4;
    f.nbits = 10 * f.nbytes;
    f.frame_min = v93 ? 18 : 8;
    f.lo_rng[0] = 100;
    f.lo_rng[1] = 190;
    f.search[0] = 150;
    f.search[1] = 400;
    f.min_swing = 3000;
    return f;
}

std::vector<int> Capture::slots() const
{
    std::set<int> s;
    for (const auto& it : items) s.insert(it.slot);
    return std::vector<int>(s.begin(), s.end());
}

Stream Capture::stream(const std::set<int>& only) const
{
    Stream st;
    for (const auto& it : items) {
        if (!only.empty() && !only.count(it.slot)) continue;
        st.data.insert(st.data.end(), it.bits.bytes.begin(), it.bits.bytes.end());
        st.soft.insert(st.soft.end(), it.bits.soft.begin(), it.bits.soft.end());
    }
    return st;
}

bool read_tbc(const FieldSource& F, const SignalOptions& opt, const LogFn& log, Capture& cap,
              const std::function<void(size_t, size_t)>& progress, const std::atomic<bool>* cancel)
{
    const size_t nf = F.field_count();
    if (!F.has_field_order()) log("Keine .tbc.json gefunden: 1. Halbbild der Datei = 1. Halbbild");
    const size_t max_fields = static_cast<size_t>(opt.scan_seconds * 50);
    const size_t maxline = std::min<size_t>(40, F.field_lines());
    const Fmt fmt = *choose_variant_probe(F, nf, max_fields, maxline, opt.variant, log);
    std::vector<double> l(F.width());

    // 1) data lines (field line index per parity)
    std::vector<std::vector<int>> sets(2);
    std::optional<std::vector<double>> offs;
    if (opt.lines.empty()) {
        std::vector<long> score[2] = {std::vector<long>(maxline, 0), std::vector<long>(maxline, 0)};
        std::vector<std::tuple<int, int, double>> o;
        std::vector<int> last[2];
        bool have_last = false;
        int stable = 0;
        const size_t end = std::min(nf, max_fields);
        const size_t block = 50, need = 100;
        size_t t = 0;
        for (size_t b0 = 0; b0 < end; b0 += block) {
            if (cancel && cancel->load()) return false;
            for (size_t fi = b0; fi < std::min(b0 + block, end); ++fi) {
                const int par = F.is_first_field(fi) ? 0 : 1;
                for (size_t li = 0; li < maxline; ++li) {
                    F.read_line(fi, li, l.data());
                    const auto ph = line_phase(l, fmt);
                    if (!ph) continue;
                    const int q = framing(l, *ph, fmt);
                    if (q >= fmt.frame_min) ++score[par][li];
                    if (q == framing_full(fmt)) {
                        if (auto s = hsync_end(l)) o.emplace_back(par, static_cast<int>(li), ph->e0 - *s);
                    }
                }
            }
            std::vector<int> cur[2] = {select_lines(score[0]), select_lines(score[1])};
            const long m0 = *std::max_element(score[0].begin(), score[0].end());
            const long m1 = *std::max_element(score[1].begin(), score[1].end());
            if (std::min(m0, m1) >= static_cast<long>(need) && have_last && cur[0] == last[0] && cur[1] == last[1]) {
                ++stable;
            } else {
                stable = 0;
            }
            last[0] = cur[0];
            last[1] = cur[1];
            have_last = true;
            t = std::min(b0 + block, end);
            if (stable >= 5) break;
        }
        std::string hits;
        for (int p = 0; p < 2; ++p) {
            for (int li : last[p]) {
                hits += (hits.empty() ? "" : ", ") + strf("%d:%ld", li + 1 + 313 * p, score[p][static_cast<size_t>(li)]);
            }
        }
        log(strf("Zeilensuche ueber %zu Halbbilder (%.1f s), Treffer je Zeile: %s", t, static_cast<double>(t) / 50.0,
                 hits.c_str()));
        for (int p = 0; p < 2; ++p) {
            if (last[p].empty()) {
                log(strf("WARNUNG: im %d. Halbbild keine VideoDat-Zeile gefunden "
                         "(ggf. --lines angeben oder --scan-seconds erhoehen)",
                         p + 1));
            }
        }
        sets[0] = last[0];
        sets[1] = last[1];
        offs = std::vector<double>();
        for (const auto& [p, li, off] : o) {
            if (std::find(last[p].begin(), last[p].end(), li) != last[p].end()) offs->push_back(off);
        }
        std::vector<int> all;
        for (int k : sets[0]) all.push_back(k + 1);
        for (int k : sets[1]) all.push_back(k + 314);
        log("VideoDat-Zeilen (automatisch, Bildzeilen PAL): " + int_list(all));
    } else {
        std::set<int> f1, f2;
        for (int n : opt.lines) {
            if (n >= 1 && n <= 313) f1.insert(n - 1);
            if (n >= 314 && n <= 625) f2.insert(n - 314);
        }
        sets[0].assign(f1.begin(), f1.end());
        sets[1].assign(f2.begin(), f2.end());
    }
    // Lines beyond what the source holds cannot be read.
    for (auto& s : sets) {
        s.erase(std::remove_if(s.begin(), s.end(), [&](int li) { return li < 0 || static_cast<size_t>(li) >= F.field_lines(); }),
                s.end());
    }

    // 2) distance H-sync -> start edge
    std::optional<double> offset;
    if (opt.use_sync) {
        if (!offs) {
            offs = std::vector<double>();
            for (size_t fi = 0; fi < std::min(nf, max_fields); ++fi) {
                for (int li : sets[F.is_first_field(fi) ? 0 : 1]) {
                    F.read_line(fi, static_cast<size_t>(li), l.data());
                    const auto ph = line_phase(l, fmt);
                    const auto s = hsync_end(l);
                    if (ph && s && framing(l, *ph, fmt) == framing_full(fmt)) offs->push_back(ph->e0 - *s);
                }
                if (offs->size() >= 500) break;
            }
        }
        if (offs->size() >= 10) {
            offset = np_median(*offs);
            log(strf("Startflanke %.2f Samples nach H-Sync-Rueckflanke (Streuung %.2f)", *offset, np_std(*offs)));
        }
    }

    // 3) equalizer (1993 only)
    std::optional<Equalizer> eq;
    if (opt.use_eq && fmt.variant == "1990") {
        log("Entzerrer: fuer Variante 1990 nicht verwendet");
    } else if (opt.use_eq) {
        const size_t start = opt.lines.empty() ? std::min(max_fields, nf / 2) : 0;
        eq = train_equalizer(F, sets, offset, start, nf, log, cancel);
        if (cancel && cancel->load()) return false;
    }

    // 4) decode
    cap = Capture();
    cap.fmt = fmt;
    std::vector<int> l1, l2;
    for (int k : sets[0]) l1.push_back(k + 1);
    for (int k : sets[1]) l2.push_back(k + 314);
    for (size_t i = 0; i < std::max(l1.size(), l2.size()); ++i) {
        std::string name;
        if (i < l1.size()) name = std::to_string(l1[i]);
        if (i < l2.size()) name += (name.empty() ? "" : "-") + std::to_string(l2[i]);
        cap.slot_names[static_cast<int>(i)] = name;
    }
    size_t nsync = 0;
    for (size_t fi = 0; fi < nf; ++fi) {
        if ((fi & 1023) == 0) {
            if (cancel && cancel->load()) return false;
            if (progress) progress(fi, nf);
        }
        const auto& set = sets[F.is_first_field(fi) ? 0 : 1];
        for (size_t slot = 0; slot < set.size(); ++slot) {
            F.read_line(fi, static_cast<size_t>(set[slot]), l.data());
            std::optional<double> prior;
            if (offset) {
                if (auto s = hsync_end(l)) {
                    prior = *s + *offset;
                    ++nsync;
                }
            }
            if (auto r = slice_tbc(l, prior, eq ? &*eq : nullptr, fmt)) {
                cap.items.push_back({fi, static_cast<int>(slot), std::move(*r)});
            }
        }
    }
    if (progress) progress(nf, nf);
    log(strf("%zu Halbbilder, %zu Datenzeilen (%zu mit H-Sync-Bezug) -> %zu Bytes", nf, cap.items.size(), nsync,
             cap.items.size() * static_cast<size_t>(fmt.nbytes)));
    return true;
}

} // namespace videodat
