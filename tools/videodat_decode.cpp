/*
 * File:        videodat_decode.cpp
 * Module:      orc-stage-plugin-videodat
 * Purpose:     Command-line front end of the C++ VideoDat decoder, with the
 *              options of videodat_decoder.py (TBC files and byte streams;
 *              bt8x8 VBI captures are not supported). Used to compare the C++
 *              port with the Python decoder.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "decoder/vd_decoder.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

#if defined(_WIN32)
#define fseeko _fseeki64
#endif

using namespace videodat;

namespace {

class TbcFile : public FieldSource {
public:
    TbcFile(const std::string& path, size_t width, size_t field_lines)
        : width_(width), field_lines_(field_lines)
    {
        f_ = std::fopen(path.c_str(), "rb");
        if (f_) {
            std::fseek(f_, 0, SEEK_END);
            const long long bytes = std::ftell(f_);
            nf_ = static_cast<size_t>(bytes / 2) / (width_ * field_lines_);
        }
        order_ = read_order(path);
        if (!order_ || order_->size() < nf_) {
            order_.reset();
        }
        buf_.resize(width_);
    }
    ~TbcFile() override
    {
        if (f_) std::fclose(f_);
    }
    bool ok() const { return f_ != nullptr; }
    size_t field_count() const override { return nf_; }
    size_t width() const override { return width_; }
    size_t field_lines() const override { return field_lines_; }
    bool has_field_order() const override { return order_.has_value(); }
    bool is_first_field(size_t fi) const override { return order_ ? (*order_)[fi] : fi % 2 == 0; }
    void read_line(size_t fi, size_t li, double* out) const override
    {
        const long long off = static_cast<long long>((fi * field_lines_ + li) * width_ * 2);
        if (off != pos_) {
            fseeko(f_, off, SEEK_SET);
        }
        const size_t n = std::fread(buf_.data(), 2, width_, f_);
        pos_ = off + static_cast<long long>(n * 2);
        for (size_t i = 0; i < width_; ++i) out[i] = i < n ? static_cast<double>(buf_[i]) : 0.0;
    }

private:
    static std::optional<std::vector<bool>> read_order(const std::string& path)
    {
        std::string base = path;
        const size_t dot = base.rfind('.');
        const std::string cands[] = {path + ".json", (dot == std::string::npos ? base : base.substr(0, dot)) + ".tbc.json"};
        for (const auto& js : cands) {
            std::ifstream f(js, std::ios::binary);
            if (!f) continue;
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            size_t p = text.find("\"fields\"");
            if (p == std::string::npos) return std::vector<bool>();
            p = text.find('[', p);
            if (p == std::string::npos) return std::vector<bool>();
            std::vector<bool> order;
            int depth = 0;
            size_t obj_start = 0;
            bool in_str = false;
            for (size_t i = p + 1; i < text.size(); ++i) {
                const char c = text[i];
                if (in_str) {
                    if (c == '\\') ++i;
                    else if (c == '"') in_str = false;
                    continue;
                }
                if (c == '"') {
                    in_str = true;
                } else if (c == '{' || c == '[') {
                    if (depth == 0 && c == '{') obj_start = i;
                    ++depth;
                } else if (c == '}' || c == ']') {
                    if (depth == 0) break;  // end of fields array
                    --depth;
                    if (depth == 0 && c == '}') {
                        // top-level key isFirstField of this object (nested keys are rare)
                        const std::string obj = text.substr(obj_start, i - obj_start + 1);
                        const size_t k = obj.find("\"isFirstField\"");
                        bool v = order.size() % 2 == 0;
                        if (k != std::string::npos) {
                            const size_t c2 = obj.find(':', k);
                            size_t q = c2 + 1;
                            while (q < obj.size() && std::isspace(static_cast<unsigned char>(obj[q]))) ++q;
                            v = obj.compare(q, 4, "true") == 0;
                        }
                        order.push_back(v);
                    }
                }
            }
            return order;
        }
        return std::nullopt;
    }

    std::FILE* f_{nullptr};
    size_t width_, field_lines_;
    size_t nf_{0};
    std::optional<std::vector<bool>> order_;
    mutable std::vector<uint16_t> buf_;
    mutable long long pos_{-1};
};

void usage()
{
    std::fprintf(stderr,
                 "usage: videodat_decode INPUT [--format auto|vbi|tbc] [--variant auto|1993|1990]\n"
                 "       [--tbc-width N] [--tbc-field-lines N] [--no-sync] [--no-eq]\n"
                 "       [--scan-seconds S] [-o DIR] [--bytes] [--save-bytes FILE] [--lines LIST]\n"
                 "       [--partial] [--keine-dienste] [--dump-other] [--decrypt-services]\n"
                 "       [--decrypt-files]\n");
}

} // namespace

int main(int argc, char** argv)
{
    std::string input, format = "auto";
    DecodeOptions opt;
    size_t tbc_width = 1135, tbc_field_lines = 313;
    bool bytes_in = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--format") format = val();
        else if (a == "--variant") opt.variant = val();
        else if (a == "--tbc-width") tbc_width = std::stoul(val());
        else if (a == "--tbc-field-lines") tbc_field_lines = std::stoul(val());
        else if (a == "--no-sync") opt.no_sync = true;
        else if (a == "--no-eq") opt.no_eq = true;
        else if (a == "--no-line-tracking") {}
        else if (a == "--scan-seconds") opt.scan_seconds = std::stod(val());
        else if (a == "-o" || a == "--outdir") opt.outdir = val();
        else if (a == "--bytes") bytes_in = true;
        else if (a == "--save-bytes") opt.save_bytes = val();
        else if (a == "--line-length" || a == "--lines-per-frame") val();
        else if (a == "--lines") {
            const std::string l = val();
            size_t p = 0;
            while (p < l.size()) {
                const size_t q = l.find(',', p);
                opt.lines.push_back(std::stoi(l.substr(p, q == std::string::npos ? std::string::npos : q - p)));
                if (q == std::string::npos) break;
                p = q + 1;
            }
        } else if (a == "--partial") opt.partial = true;
        else if (a == "--keine-dienste") opt.keine_dienste = true;
        else if (a == "--dump-other") opt.dump_other = true;
        else if (a == "--decrypt-services") opt.decrypt_services = true;
        else if (a == "--decrypt-files") opt.decrypt_files = true;
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        } else {
            input = a;
        }
    }
    if (input.empty()) {
        usage();
        return 2;
    }
    const LogFn log = [](const std::string& s) { std::printf("%s\n", s.c_str()); std::fflush(stdout); };
    if (format == "auto") {
        std::string ext;
        const size_t dot = input.rfind('.');
        const size_t sep = input.find_last_of("/\\");
        if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) ext = input.substr(dot);
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        format = (ext == ".tbc" || ext == ".tbcy" || ext == ".tbcc") ? "tbc" : "vbi";
        if (ext == ".tbcc") log("WARNUNG: .tbcc ist die Chroma-Datei; VideoDat steht in der Luma-Datei (.tbc bzw. .tbcy)");
    }
    if (bytes_in) {
        std::ifstream f(input, std::ios::binary);
        const Bytes data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        decode_bytes(data, opt, log);
        return 0;
    }
    if (format != "tbc") {
        std::fprintf(stderr, "bt8x8 VBI captures are not supported by the C++ decoder\n");
        return 2;
    }
    TbcFile src(input, tbc_width, tbc_field_lines);
    if (!src.ok()) {
        std::fprintf(stderr, "cannot open %s\n", input.c_str());
        return 1;
    }
    decode_fields(src, opt, log);
    return 0;
}
