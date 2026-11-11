/*
 * File:        vd_decoder.cpp
 * Module:      orc-stage-plugin-videodat (VideoDat decoder)
 * Purpose:     Top level of the VideoDat decoder
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "vd_decoder.h"

#include "vd_1990.h"
#include "vd_assemble.h"
#include "vd_crypto.h"

#include <filesystem>
#include <fstream>

namespace videodat {

namespace {

// "%-14s" padded by characters, as Python does, not by bytes.
std::string pad_right(const std::string& s, size_t width)
{
    size_t chars = 0;
    for (unsigned char c : s) chars += (c & 0xC0) != 0x80 ? 1 : 0;
    return chars >= width ? s : s + std::string(width - chars, ' ');
}

std::string file_line(const char* what, const FileResult& r)
{
    const std::string st = r.missing == 0
                               ? "vollstaendig"
                               : strf("unvollstaendig (%lld von %lld Bloecken fehlen)", static_cast<long long>(r.missing),
                                      static_cast<long long>(r.nblk));
    const std::string rep = r.count > 1 ? strf("  (%lldx gesendet)", static_cast<long long>(r.count)) : "";
    return strf("  %s %02X  %s %8lld Bytes  %s%s%s", what, r.A, pad_right(r.name, 14).c_str(),
                static_cast<long long>(r.size), st.c_str(), rep.c_str(), r.path.empty() ? "  [nicht geschrieben]" : "");
}

DecodeSummary finish_1990(std::vector<std::pair<std::string, Stream>>& channels, const DecodeOptions& opt,
                          const LogFn& log)
{
    DecodeSummary sum;
    sum.variant = "1990";
    const auto res = run_1990(channels, opt.outdir, opt.partial, opt.keine_dienste, log);
    log("");
    log("Dateien:");
    for (const auto& r : res) log(file_line("Kanal", r));
    for (const auto& r : res) sum.complete += r.missing == 0 ? 1 : 0;
    sum.files = res.size();
    log(strf("%zu Dateien, davon %zu vollstaendig", sum.files, sum.complete));
    return sum;
}

DecodeSummary finish_1993(const Stream& stream, const DecodeOptions& opt, const LogFn& log)
{
    DecodeSummary sum;
    sum.variant = "1993";
    auto pk = find_packets(stream);
    size_t n_ok = 0, n_unstuffed = 0;
    for (const auto& x : pk) {
        n_ok += x.ok ? 1 : 0;
        n_unstuffed += x.how == "unstuffed" ? 1 : 0;
    }
    auto fixed = repair(pk);
    log(strf("Pakete: %zu gefunden, %zu Pruefsumme ok, repariert: %d per Soft-Decision, %d aus Kopien", pk.size(), n_ok,
             fixed["soft"], fixed["kopien"]));
    auto perm = korrigiere_permutation(pk);
    if (perm["kopien"] || perm["inhalt"] || perm["verworfen"]) {
        log(strf("  Permutationsfehler: %d aus Kopien, %d am Inhalt korrigiert, %d Fehlreparaturen verworfen",
                 perm["kopien"], perm["inhalt"], perm["verworfen"]));
    }
    if (n_unstuffed) log(strf("  %zu Pakete nach Entfernen von Sync-Escape-Bytes gueltig", n_unstuffed));
    std::vector<FileManifestItem> manifest;
    const auto res = assemble(pk, opt.outdir, opt.partial, opt.dump_other, log, opt.decrypt_files ? &manifest : nullptr);
    if (opt.decrypt_files) {
        const auto c = write_file_report(pk, manifest, join_path(opt.outdir, "decrypted_files"));
        log(strf("Datei-Entschluesselung: {'decrypted': %d, 'unavailable_key': %d, 'rejected': %d}", c.decrypted,
                 c.unavailable_key, c.rejected));
    }
    log("");
    log("Dateien:");
    for (const auto& r : res) {
        log(file_line("Strom", r));
        if (!r.path.empty()) {
            std::ifstream f(std::filesystem::u8path(r.path), std::ios::binary);
            Bytes head(32);
            f.read(reinterpret_cast<char*>(head.data()), 32);
            head.resize(static_cast<size_t>(f.gcount()));
            if (auto k = chvd_kopf(head)) {
                log(strf("            Kaufsoftware (CHVDFILE): enthaelt %s, %lld Bytes%s, verschluesselt", k->name.c_str(),
                         static_cast<long long>(k->laenge), k->zeit ? k->zeit->format(" vom %d.%m.%Y %H:%M").c_str() : ""));
            }
        }
    }
    for (const auto& r : res) sum.complete += r.missing == 0 ? 1 : 0;
    sum.files = res.size();
    log(strf("%zu Dateien, davon %zu vollstaendig", sum.files, sum.complete));
    if (!opt.keine_dienste) analyse_dienste(pk, join_path(opt.outdir, "dienste"), log);
    if (opt.decrypt_services) {
        const std::string sd = join_path(opt.outdir, "dienste");
        make_dirs(sd);
        const std::string report = join_path(sd, "decrypted_services.jsonl");
        const auto c = write_service_report(pk, report);
        log(strf("Entschluesselung: %d innere Pruefsummen ok, %d falsch, %d ungueltige Pakete -> %s", c.passed, c.failed,
                 c.malformed, report.c_str()));
    }
    return sum;
}

} // namespace

DecodeSummary decode_fields(const FieldSource& src, const DecodeOptions& opt, const LogFn& log,
                            const std::function<void(size_t, size_t)>& progress, const std::atomic<bool>* cancel)
{
    SignalOptions so;
    so.lines = opt.lines;
    so.use_sync = !opt.no_sync;
    so.scan_seconds = opt.scan_seconds;
    so.use_eq = !opt.no_eq;
    so.variant = opt.variant;
    Capture cap;
    if (!read_tbc(src, so, log, cap, progress, cancel)) {
        DecodeSummary s;
        s.cancelled = true;
        return s;
    }
    if (cap.fmt.variant == "1990") {
        auto channels = split_channels(cap, log);
        if (!opt.save_bytes.empty()) {
            const auto [stem, ext] = splitext(opt.save_bytes);
            for (const auto& [label, st] : channels) {
                const std::string fn = stem + "_" + label + ext;
                write_file(fn, st.data);
                log(strf("Byte-Strom %s -> %s", label.c_str(), fn.c_str()));
            }
        }
        return finish_1990(channels, opt, log);
    }
    const Stream stream = cap.stream();
    if (!opt.save_bytes.empty()) write_file(opt.save_bytes, stream.data);
    return finish_1993(stream, opt, log);
}

DecodeSummary decode_bytes(const Bytes& data, const DecodeOptions& opt, const LogFn& log)
{
    if (opt.variant == "1990") {
        std::vector<std::pair<std::string, Stream>> channels;
        Stream st;
        st.data = data;
        channels.emplace_back("bytes", st);
        return finish_1990(channels, opt, log);
    }
    Stream st;
    st.data = data;
    return finish_1993(st, opt, log);
}

} // namespace videodat
