/*
 * File:        vd_util.h
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Helpers shared by the decoder: formatting, numpy-compatible
 *              statistics, civil dates, ordered counters
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 *
 * The decoder is a port of videodat_decoder.py (channel-videodat-decoder).
 * Where the Python code relies on numpy, the helpers here reproduce numpy's
 * order of operations (pairwise summation, percentile interpolation), so the
 * C++ decoder reads the same bits as the Python one.
 */

#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace videodat {

using Bytes = std::vector<uint8_t>;

// printf-style formatting into a std::string.
std::string strf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Output of the decoder's console (the Python program's log()).
using LogFn = std::function<void(const std::string&)>;

// ---------------------------------------------------------------------------
// numpy-compatible reductions
// ---------------------------------------------------------------------------

// np.add.reduce over a contiguous float64 array (pairwise summation).
double np_sum(const double* a, size_t n);
inline double np_sum(const std::vector<double>& v) { return np_sum(v.data(), v.size()); }
// np.mean (pairwise sum / n).
double np_mean(const double* a, size_t n);
// Sum of a complex array given as interleaved (re, im) doubles, n complex
// values (numpy's complex pairwise summation).
void np_complex_sum(const double* interleaved, size_t n, double& re, double& im);
// np.median (copies and sorts).
double np_median(std::vector<double> v);
// np.percentile(v, q) with the default linear method.
double np_percentile(std::vector<double> v, double q);
// np.std (population).
double np_std(const std::vector<double>& v);

// ---------------------------------------------------------------------------
// Dates (Python datetime subset: proleptic Gregorian, naive local time)
// ---------------------------------------------------------------------------

struct DateTime {
    int year{0}, month{0}, day{0}, hour{0}, minute{0}, second{0};
    int64_t microsecond{0};

    // datetime(...) raising ValueError on an invalid date -> nullopt.
    static std::optional<DateTime> make(int y, int mo, int d, int h = 0, int mi = 0, int s = 0);
    // Days since 0001-01-01 (Python's toordinal() - 1).
    int64_t days() const;
    // Seconds since 0001-01-01 00:00 including microseconds as fraction.
    double seconds() const;
    // Microseconds since 0001-01-01 00:00, exact.
    int64_t total_us() const;
    static DateTime from_total_us(int64_t us);
    DateTime plus_days(int64_t d) const;
    // t + timedelta(seconds=s); Python rounds the timedelta to microseconds
    // (round half to even).
    DateTime plus_seconds(double s) const;
    // strftime for %d %m %Y %H %M %S only.
    std::string format(const char* fmt) const;
    bool operator<(const DateTime& o) const { return total_us() < o.total_us(); }
    bool operator==(const DateTime& o) const { return total_us() == o.total_us(); }
    bool operator!=(const DateTime& o) const { return !(*this == o); }
    bool same_date(const DateTime& o) const
    {
        return year == o.year && month == o.month && day == o.day;
    }
};

// DOS date/time word pair -> datetime (videodat_decoder.dos_time()).
std::optional<DateTime> dos_time(uint32_t v);

// ---------------------------------------------------------------------------
// Counters with Python's Counter semantics where order matters
// ---------------------------------------------------------------------------

// Insertion-ordered counter (collections.Counter). most_common() is a stable
// sort by count, so ties keep insertion order, as in Python.
template <typename K>
class Counter {
public:
    void add(const K& k, int64_t n = 1)
    {
        auto it = index_.find(k);
        if (it == index_.end()) {
            index_.emplace(k, items_.size());
            items_.emplace_back(k, n);
        } else {
            items_[it->second].second += n;
        }
    }
    int64_t get(const K& k) const
    {
        auto it = index_.find(k);
        return it == index_.end() ? 0 : items_[it->second].second;
    }
    bool contains(const K& k) const { return index_.count(k) != 0; }
    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    const std::vector<std::pair<K, int64_t>>& items() const { return items_; }
    std::vector<std::pair<K, int64_t>> most_common() const
    {
        auto v = items_;
        std::stable_sort(v.begin(), v.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
        return v;
    }
    void update(const Counter& o)
    {
        for (const auto& [k, n] : o.items_) {
            add(k, n);
        }
    }

private:
    std::map<K, size_t> index_;
    std::vector<std::pair<K, int64_t>> items_;
};

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

std::string hex(const uint8_t* p, size_t n);
inline std::string hex(const Bytes& b) { return hex(b.data(), b.size()); }

// bytes.decode("cp437", "replace") -> UTF-8.
std::string cp437_to_utf8(const uint8_t* p, size_t n);
// Python str.strip() for an UTF-8 string (whitespace only from ASCII and the
// characters cp437 maps to Unicode whitespace: NBSP is not whitespace for
// str.strip? It is: \xa0 is whitespace in Python).
std::string py_strip(const std::string& s);
// Python's repr of a list of ints: "[1, 2, 3]".
std::string int_list(const std::vector<int>& v);

bool write_file(const std::string& path, const Bytes& data, bool append = false);
bool write_text(const std::string& path, const std::string& text);
void make_dirs(const std::string& path);
std::string join_path(const std::string& a, const std::string& b);

} // namespace videodat
