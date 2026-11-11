/*
 * File:        vd_util.cpp
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Helpers shared by the decoder
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "vd_util.h"

#include <cmath>
#include <filesystem>
#include <fstream>

namespace videodat {

std::string strf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string s(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) {
        std::vsnprintf(s.data(), s.size() + 1, fmt, ap2);
    }
    va_end(ap2);
    return s;
}

// ---------------------------------------------------------------------------
// numpy reductions
// ---------------------------------------------------------------------------

namespace {
constexpr size_t kPwBlock = 128;

double pairwise(const double* a, size_t n)
{
    if (n < 8) {
        double res = 0.0;
        for (size_t i = 0; i < n; ++i) {
            res += a[i];
        }
        return res;
    }
    if (n <= kPwBlock) {
        double r[8];
        for (int j = 0; j < 8; ++j) {
            r[j] = a[j];
        }
        size_t i = 8;
        for (; i < n - (n % 8); i += 8) {
            for (int j = 0; j < 8; ++j) {
                r[j] += a[i + j];
            }
        }
        double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; ++i) {
            res += a[i];
        }
        return res;
    }
    size_t n2 = n / 2;
    n2 -= n2 % 8;
    return pairwise(a, n2) + pairwise(a + n2, n - n2);
}

// n counts doubles (2 per complex value).
void cpairwise(const double* a, size_t n, double& rr, double& ri)
{
    if (n < 8) {
        rr = -0.0;
        ri = -0.0;
        for (size_t i = 0; i < n; i += 2) {
            rr += a[i];
            ri += a[i + 1];
        }
        return;
    }
    if (n <= kPwBlock) {
        double r[8];
        for (int j = 0; j < 8; ++j) {
            r[j] = a[j];
        }
        size_t i = 8;
        for (; i < n - (n % 8); i += 8) {
            for (int j = 0; j < 8; ++j) {
                r[j] += a[i + j];
            }
        }
        rr = (r[0] + r[2]) + (r[4] + r[6]);
        ri = (r[1] + r[3]) + (r[5] + r[7]);
        for (; i < n; i += 2) {
            rr += a[i];
            ri += a[i + 1];
        }
        return;
    }
    size_t n2 = n / 2;
    n2 -= n2 % 8;
    double r1, i1, r2, i2;
    cpairwise(a, n2, r1, i1);
    cpairwise(a + n2, n - n2, r2, i2);
    rr = r1 + r2;
    ri = i1 + i2;
}
} // namespace

double np_sum(const double* a, size_t n)
{
    // add.reduce starts from the identity 0.0.
    return 0.0 + pairwise(a, n);
}

double np_mean(const double* a, size_t n)
{
    return np_sum(a, n) / static_cast<double>(n);
}

void np_complex_sum(const double* interleaved, size_t n, double& re, double& im)
{
    double rr, ri;
    cpairwise(interleaved, 2 * n, rr, ri);
    re = 0.0 + rr;
    im = 0.0 + ri;
}

double np_median(std::vector<double> v)
{
    const size_t n = v.size();
    if (n == 0) {
        return std::nan("");
    }
    std::sort(v.begin(), v.end());
    if (n % 2 == 1) {
        return v[n / 2];
    }
    const double pair[2] = {v[n / 2 - 1], v[n / 2]};
    return np_mean(pair, 2);
}

double np_percentile(std::vector<double> v, double q)
{
    const size_t n = v.size();
    if (n == 0) {
        return std::nan("");
    }
    std::sort(v.begin(), v.end());
    const double qq = q / 100.0;
    // _compute_virtual_index(n, q, 1, 1)
    const double virt = (static_cast<double>(n) * qq + (1.0 + qq * (1.0 - 1.0 - 1.0))) - 1.0;
    double prev = std::floor(virt);
    if (virt >= static_cast<double>(n - 1)) {
        return v[n - 1];
    }
    if (virt < 0) {
        return v[0];
    }
    const double gamma = virt - prev;
    const size_t i = static_cast<size_t>(prev);
    const double a = v[i];
    const double b = v[std::min(i + 1, n - 1)];
    const double diff = b - a;
    if (gamma >= 0.5) {
        return b - diff * (1.0 - gamma);
    }
    return a + diff * gamma;
}

double np_std(const std::vector<double>& v)
{
    const size_t n = v.size();
    const double mean = np_sum(v) / static_cast<double>(n);
    std::vector<double> x(n);
    for (size_t i = 0; i < n; ++i) {
        const double d = v[i] - mean;
        x[i] = d * d;
    }
    return std::sqrt(np_sum(x) / static_cast<double>(n));
}

// ---------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------

namespace {
bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int days_in_month(int y, int m)
{
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && is_leap(y)) ? 29 : d[m - 1];
}
int64_t days_before_year(int y)
{
    const int64_t z = y - 1;
    return z * 365 + z / 4 - z / 100 + z / 400;
}
int64_t days_before_month(int y, int m)
{
    int64_t d = 0;
    for (int i = 1; i < m; ++i) {
        d += days_in_month(y, i);
    }
    return d;
}
} // namespace

std::optional<DateTime> DateTime::make(int y, int mo, int d, int h, int mi, int s)
{
    if (y < 1 || y > 9999 || mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo) || h < 0 ||
        h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
        return std::nullopt;
    }
    DateTime t;
    t.year = y;
    t.month = mo;
    t.day = d;
    t.hour = h;
    t.minute = mi;
    t.second = s;
    return t;
}

int64_t DateTime::days() const
{
    return days_before_year(year) + days_before_month(year, month) + day - 1;
}

int64_t DateTime::total_us() const
{
    return ((days() * 24 + hour) * 60 + minute) * 60 * 1000000LL + second * 1000000LL + microsecond;
}

double DateTime::seconds() const
{
    return static_cast<double>(total_us()) / 1e6;
}

DateTime DateTime::from_total_us(int64_t us)
{
    DateTime t;
    int64_t days = us / 86400000000LL;
    int64_t rest = us % 86400000000LL;
    if (rest < 0) {
        rest += 86400000000LL;
        --days;
    }
    // ordinal -> y/m/d
    int y = static_cast<int>(days / 366) + 1;
    while (days_before_year(y + 1) <= days) {
        ++y;
    }
    while (days_before_year(y) > days) {
        --y;
    }
    int64_t doy = days - days_before_year(y);
    int m = 1;
    while (doy >= days_in_month(y, m)) {
        doy -= days_in_month(y, m);
        ++m;
    }
    t.year = y;
    t.month = m;
    t.day = static_cast<int>(doy) + 1;
    t.microsecond = rest % 1000000;
    int64_t secs = rest / 1000000;
    t.hour = static_cast<int>(secs / 3600);
    t.minute = static_cast<int>((secs / 60) % 60);
    t.second = static_cast<int>(secs % 60);
    return t;
}

DateTime DateTime::plus_days(int64_t d) const
{
    return from_total_us(total_us() + d * 86400000000LL);
}

DateTime DateTime::plus_seconds(double s) const
{
    return from_total_us(total_us() + static_cast<int64_t>(std::nearbyint(s * 1e6)));
}

std::string DateTime::format(const char* fmt) const
{
    std::string out;
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%' || p[1] == '\0') {
            out.push_back(*p);
            continue;
        }
        ++p;
        switch (*p) {
        case 'd': out += strf("%02d", day); break;
        case 'm': out += strf("%02d", month); break;
        case 'Y': out += strf("%04d", year); break;
        case 'H': out += strf("%02d", hour); break;
        case 'M': out += strf("%02d", minute); break;
        case 'S': out += strf("%02d", second); break;
        default: out.push_back('%'); out.push_back(*p); break;
        }
    }
    return out;
}

std::optional<DateTime> dos_time(uint32_t v)
{
    const uint32_t date = v >> 16;
    const uint32_t tm = v & 0xFFFF;
    return DateTime::make(1980 + static_cast<int>(date >> 9), (date >> 5) & 15, date & 31,
                          tm >> 11, (tm >> 5) & 63, (tm & 31) * 2);
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(2 * n);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

namespace {
// cp437 0x80..0xFF as Unicode code points.
const uint16_t kCp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF,
    0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
    0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
    0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

void put_utf8(std::string& s, uint32_t c)
{
    if (c < 0x80) {
        s.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (c >> 6)));
        s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xE0 | (c >> 12)));
        s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}
} // namespace

std::string cp437_to_utf8(const uint8_t* p, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        put_utf8(s, p[i] < 0x80 ? p[i] : kCp437[p[i] - 0x80]);
    }
    return s;
}

std::string py_strip(const std::string& s)
{
    // ASCII whitespace as Python sees it (str.isspace), plus U+00A0 (C2 A0).
    auto ws_at_front = [&](size_t i) -> size_t {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F)) return 1;
        if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) return 2;
        return 0;
    };
    size_t a = 0;
    while (a < s.size()) {
        const size_t k = ws_at_front(a);
        if (k == 0) break;
        a += k;
    }
    size_t b = s.size();
    while (b > a) {
        const unsigned char c = static_cast<unsigned char>(s[b - 1]);
        if (c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F)) {
            --b;
        } else if (c == 0xA0 && b >= a + 2 && static_cast<unsigned char>(s[b - 2]) == 0xC2) {
            b -= 2;
        } else {
            break;
        }
    }
    return s.substr(a, b - a);
}

std::string int_list(const std::vector<int>& v)
{
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        s += (i ? ", " : "") + std::to_string(v[i]);
    }
    return s + "]";
}

bool write_file(const std::string& path, const Bytes& data, bool append)
{
    std::ofstream f(std::filesystem::u8path(path),
                    std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

bool write_text(const std::string& path, const std::string& text)
{
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return static_cast<bool>(f);
}

void make_dirs(const std::string& path)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(path), ec);
}

std::string join_path(const std::string& a, const std::string& b)
{
    return (std::filesystem::u8path(a) / std::filesystem::u8path(b)).u8string();
}

} // namespace videodat
