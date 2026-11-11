/*
 * File:        videodat_sink_stage.cpp
 * Module:      orc-stage-plugin-videodat
 * Purpose:     VideoDat Decoder sink stage: decodes Pro7 VideoDat from the VBI
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "videodat_sink_stage.h"

#include "plugin.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <variant>

namespace orc::plugins::videodat {

namespace fs = std::filesystem;

namespace {

// Parameter names. Path parameters end in "_path": the host resolves only
// those against the project directory.
constexpr const char* kOutputPath = "output_path";
constexpr const char* kVariant = "variant";
constexpr const char* kLines = "lines";
constexpr const char* kScanSeconds = "scan_seconds";
constexpr const char* kNoSync = "no_sync";
constexpr const char* kNoEq = "no_eq";
constexpr const char* kPartial = "partial";
constexpr const char* kSkipServices = "skip_services";
constexpr const char* kDumpOther = "dump_other";
constexpr const char* kDecryptServices = "decrypt_services";
constexpr const char* kDecryptFiles = "decrypt_files";
constexpr const char* kSaveBytes = "save_bytes";
constexpr const char* kSaveBytesPath = "save_bytes_path";

// Combo entries may carry a label after this separator (host convention).
constexpr char kLabelSeparator = '\x1f';

constexpr int32_t kMaxScanSeconds = 3600;

// One CVBS_U10_4FSC sample in the 16-bit TBC domain (10-bit value << 6) of
// ld-decode/vhs-decode, which the decoder's thresholds are written for.
inline uint16_t cvbs_to_tbc_sample(int16_t v)
{
    if (v < 0) {
        v = 0;
    } else if (v > 1023) {
        v = 1023;
    }
    return static_cast<uint16_t>(static_cast<uint16_t>(v) << 6);
}

// PAL frame lines: first field 1-313, second field 314-625.
constexpr int32_t kLastFirstFieldLine = 313;
constexpr int32_t kFirstSecondFieldLine = 314;
constexpr int32_t kLastSecondFieldLine = 625;

constexpr const char* kLogName = "videodat_decoder.log";
constexpr const char* kByteStreamExtension = ".bin";

template <typename T>
T get_or(const std::map<std::string, ParameterValue>& p, const std::string& name, T fallback)
{
    const auto it = p.find(name);
    if (it == p.end() || !std::holds_alternative<T>(it->second)) {
        return fallback;
    }
    return std::get<T>(it->second);
}

std::string trim(const std::string& s)
{
    size_t a = 0;
    size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

// Validates and normalises a --lines list ("11, 12,13" -> "11,12,13").
std::string normalise_lines(const std::string& raw)
{
    std::string out;
    std::string item;
    auto flush = [&]() {
        const std::string t = trim(item);
        item.clear();
        if (t.empty()) {
            return;
        }
        for (char c : t) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                throw std::runtime_error("Lines: \"" + t + "\" is not a line number");
            }
        }
        const long n = std::stol(t);
        const bool ok = (n >= 1 && n <= kLastFirstFieldLine) ||
                        (n >= kFirstSecondFieldLine && n <= kLastSecondFieldLine);
        if (!ok) {
            throw std::runtime_error("Lines: " + t + " is not a PAL frame line (1-625)");
        }
        out += (out.empty() ? "" : ",") + t;
    };
    for (char c : raw) {
        if (c == ',' || c == ' ' || c == ';') {
            flush();
        } else {
            item.push_back(c);
        }
    }
    flush();
    return out;
}

ParameterDescriptor checkbox(const char* name, const char* display, const std::string& description)
{
    ParameterDescriptor d;
    d.name = name;
    d.display_name = display;
    d.description = description;
    d.type = ParameterType::BOOL;
    d.constraints.default_value = false;
    return d;
}

// Greys an option out (rather than hiding it) while the 1990 variant is
// selected: these options only exist for the 1993 transmission format.
void only_1993(ParameterDescriptor& d)
{
    d.constraints.depends_on = ParameterDependency{kVariant, {"auto", "1993"}, false};
}

} // namespace

// ---------------------------------------------------------------------------
// DecoderOptions
// ---------------------------------------------------------------------------

std::string DecoderOptions::effective_save_bytes_path() const
{
    if (!save_bytes) {
        return {};
    }
    // The plugin is not told the name of the input file, so the default name
    // is taken from the output directory, which usually is named after the
    // tape.
    fs::path out(output_dir);
    std::string stem = out.filename().string();
    if (stem.empty() || stem == "." || stem == "..") {
        stem = out.parent_path().filename().string();
    }
    if (stem.empty()) {
        stem = "videodat";
    }
    const std::string default_name = stem + kByteStreamExtension;

    if (save_bytes_path.empty()) {
        return (out / default_name).string();
    }
    // A directory (existing, or written with a trailing separator) gets the
    // default file name inside it, instead of the decoder failing to open it.
    std::error_code ec;
    const char last = save_bytes_path.back();
    if (last == '/' || last == '\\' || fs::is_directory(save_bytes_path, ec)) {
        return (fs::path(save_bytes_path) / default_name).string();
    }
    fs::path file(save_bytes_path);
    if (!file.has_extension()) {
        file += kByteStreamExtension;
    }
    return file.string();
}

::videodat::DecodeOptions DecoderOptions::decode_options() const
{
    const bool v1993 = variant != "1990";
    ::videodat::DecodeOptions o;
    o.outdir = output_dir;
    o.variant = variant;
    size_t p = 0;
    while (p < lines.size()) {
        const size_t q = lines.find(',', p);
        o.lines.push_back(std::stoi(lines.substr(p, q == std::string::npos ? std::string::npos : q - p)));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    o.scan_seconds = scan_seconds;
    o.no_sync = no_sync;
    o.no_eq = no_eq && v1993;
    o.partial = partial;
    o.keine_dienste = skip_services;
    o.dump_other = dump_other && v1993;
    o.decrypt_services = decrypt_services && v1993;
    o.decrypt_files = decrypt_files && v1993;
    o.save_bytes = effective_save_bytes_path();
    return o;
}

// ---------------------------------------------------------------------------
// RepresentationFields
// ---------------------------------------------------------------------------

RepresentationFields::RepresentationFields(const VideoFrameRepresentation& source) : source_(source)
{
    const auto params = source.get_video_parameters();
    const FrameIDRange range = source.frame_range();
    first_ = range.first;
    fields_ = static_cast<size_t>(range.count()) * 2;
    if (params && params->is_valid()) {
        width_ = static_cast<size_t>(params->frame_width_nominal);
        frame_height_ = static_cast<size_t>(params->frame_height);
        field1_lines_ = field1_lines(params->system);
        // Field lines as the TBC files of ld-decode/vhs-decode count them.
        field_lines_ = field1_lines_;
    }
    yc_ = source.has_separate_channels();
}

void RepresentationFields::read_line(size_t fi, size_t li, double* out) const
{
    const FrameID frame = first_ + fi / 2;
    const size_t frame_line = (fi % 2 == 0 ? 0 : field1_lines_) + li;
    if (frame_line >= frame_height_ || !source_.has_frame(frame)) {
        std::fill(out, out + width_, 0.0);
        return;
    }
    if (yc_) {
        // Y/C sources: VideoDat is a luma signal.
        const auto* line = source_.get_line_luma(frame, frame_line);
        for (size_t i = 0; i < width_; ++i) {
            out[i] = line ? static_cast<double>(cvbs_to_tbc_sample(line[i])) : 0.0;
        }
        return;
    }
    // get_line_samples() lets the source read single lines instead of whole
    // frames, which is most of the cost of reading the VBI.
    const auto line = source_.get_line_samples(frame, frame_line);
    for (size_t i = 0; i < width_; ++i) {
        out[i] = i < line.size() ? static_cast<double>(cvbs_to_tbc_sample(line[i])) : 0.0;
    }
}

// ---------------------------------------------------------------------------
// Stage
// ---------------------------------------------------------------------------

VideoDatSinkStage::VideoDatSinkStage()
{
    set_configuration_status(ConfigurationStatus::Red);
}

std::string VideoDatSinkStage::version() const
{
    return "1.0.0";
}

NodeTypeInfo VideoDatSinkStage::get_node_type_info() const
{
    return NodeTypeInfo{
        kStageNodeType, kStageName, kStageDisplayName,
        "Decodes Pro7 VideoDat (1990-1994) from the VBI and writes the received files and "
        "services. Trigger to run the decoder.",
        kStageMinInputs, kStageMaxInputs, kStageMinOutputs, kStageMaxOutputs,
        kStageCompatibleFormats};
}

std::vector<ArtifactPtr> VideoDatSinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>&,
    ObservationContext&)
{
    // All work happens in trigger(); the input is cached for the preview.
    cached_input_.reset();
    if (!inputs.empty()) {
        cached_input_ = std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
    }
    return {};
}

std::vector<ParameterDescriptor> VideoDatSinkStage::get_parameter_descriptors(
    VideoSystem, SourceType) const
{
    std::vector<ParameterDescriptor> d;

    {
        ParameterDescriptor p;
        p.name = kOutputPath;
        p.display_name = "Output Directory (-o)";
        p.description =
            "Directory for the received files, the service report and the decoder log. It is "
            "created if it does not exist. The browse dialog is a file dialog: go to the parent "
            "directory and type the directory name.";
        p.type = ParameterType::FILE_PATH;
        p.output_path = true;
        p.constraints.required = true;
        p.constraints.default_value = std::string("");
        d.push_back(p);
    }
    {
        ParameterDescriptor p;
        p.name = kVariant;
        p.display_name = "Variant (--variant)";
        p.description =
            "Transmission variant. 1993: 2 Mbit/s, 10 bytes per line. 1990: 800 kbit/s, 4 bytes "
            "per line, several channels. Automatic decides from the start of the recording.";
        p.type = ParameterType::STRING;
        p.constraints.allowed_strings = {std::string("auto") + kLabelSeparator + "Automatic",
                                         std::string("1993"), std::string("1990")};
        p.constraints.default_value = std::string("auto");
        d.push_back(p);
    }
    {
        ParameterDescriptor p;
        p.name = kLines;
        p.display_name = "Lines (--lines)";
        p.description =
            "PAL frame lines carrying VideoDat, comma separated, e.g. 11,12,13,324,325,326. "
            "Empty: detect them automatically (among the first 40 lines of each field).";
        p.type = ParameterType::STRING;
        p.constraints.default_value = std::string("");
        d.push_back(p);
    }
    {
        ParameterDescriptor p;
        p.name = kScanSeconds;
        p.display_name = "Scan Seconds (--scan-seconds)";
        p.description =
            "Search for the data lines over at most this many seconds from the start of the "
            "recording. The search stops earlier once the lines are reliably detected.";
        p.type = ParameterType::INT32;
        p.constraints.min_value = int32_t{1};
        p.constraints.max_value = kMaxScanSeconds;
        p.constraints.default_value = int32_t{30};
        d.push_back(p);
    }

    d.push_back(checkbox(kNoSync, "No H-Sync Reference (--no-sync)",
                         "Do not use the H-sync as timing reference. By default the distance "
                         "from H-sync to the data start edge is learned from the recording, so "
                         "lines with a destroyed start edge can still be read."));
    {
        auto p = checkbox(kNoEq, "No Equalizer (--no-eq)",
                          "Do not train an equalizer against ringing, ghosting and blur; use "
                          "the adaptive threshold only. 1993 variant only.");
        only_1993(p);
        d.push_back(p);
    }
    d.push_back(checkbox(kPartial, "Write Incomplete Files (--partial)",
                         "Write incomplete files anyway. Missing 255-byte blocks are filled with "
                         "zero bytes and the file name ends in .unvollstaendig."));
    d.push_back(checkbox(kSkipServices, "Skip Other Services (--keine-dienste)",
                         "Do not evaluate the other services (clock, schedule, bit lists, "
                         "encrypted data; 1990: text between the packets)."));
    {
        auto p = checkbox(kDumpOther, "Dump Other Packets (--dump-other)",
                          "Write raw packets of other services to andere_dienste/. 1993 variant "
                          "only.");
        only_1993(p);
        d.push_back(p);
    }
    {
        auto p = checkbox(kDecryptServices, "Decrypt Services (--decrypt-services)",
                          "Decrypt stream 03 types 07/08/0A (account credit, limit) into "
                          "dienste/decrypted_services.jsonl. 1993 variant only.");
        only_1993(p);
        d.push_back(p);
    }
    {
        auto p = checkbox(kDecryptFiles, "Decrypt Files (--decrypt-files)",
                          "Decrypt paid software (CHVDFILE, .VD$) into decrypted_files/. Combine "
                          "with incomplete files for partly received software. 1993 variant "
                          "only.");
        only_1993(p);
        d.push_back(p);
    }
    d.push_back(checkbox(kSaveBytes, "Save Byte Stream (--save-bytes)",
                         "Save the decoded byte stream so it can be re-analysed later with "
                         "videodat_decoder.py --bytes, without signal processing."));
    {
        ParameterDescriptor p;
        p.name = kSaveBytesPath;
        p.display_name = "Byte Stream File";
        p.description =
            "File for the byte stream. Empty: <output directory>/<name of the output "
            "directory>.bin. A directory gets that file name inside it; a name without "
            "extension gets .bin. 1990 recordings get one file per channel, named "
            "<file>_zeilen_<lines>.bin.";
        p.type = ParameterType::FILE_PATH;
        p.output_path = true;
        p.file_extension_hint = ".bin";
        p.constraints.default_value = std::string("");
        p.constraints.depends_on = ParameterDependency{kSaveBytes, {"true"}, false};
        d.push_back(p);
    }
    return d;
}

std::map<std::string, ParameterValue> VideoDatSinkStage::get_parameters() const
{
    return parameters_;
}

bool VideoDatSinkStage::set_parameters(const std::map<std::string, ParameterValue>& params)
{
    parameters_ = params;
    const bool has_output = !get_or<std::string>(params, kOutputPath, "").empty();
    set_configuration_status(has_output ? ConfigurationStatus::Green : ConfigurationStatus::Red);
    return true;
}

DecoderOptions VideoDatSinkStage::parse_options(
    const std::map<std::string, ParameterValue>& p)
{
    DecoderOptions o;
    o.output_dir = trim(get_or<std::string>(p, kOutputPath, ""));
    if (o.output_dir.empty()) {
        throw std::runtime_error("no output directory set");
    }
    o.variant = get_or<std::string>(p, kVariant, "auto");
    if (o.variant != "auto" && o.variant != "1993" && o.variant != "1990") {
        throw std::runtime_error("unknown variant \"" + o.variant + "\"");
    }
    o.lines = normalise_lines(get_or<std::string>(p, kLines, ""));
    o.scan_seconds = get_or<int32_t>(p, kScanSeconds, 30);
    if (o.scan_seconds < 1 || o.scan_seconds > kMaxScanSeconds) {
        throw std::runtime_error("scan seconds must be between 1 and " +
                                 std::to_string(kMaxScanSeconds));
    }
    o.no_sync = get_or<bool>(p, kNoSync, false);
    o.no_eq = get_or<bool>(p, kNoEq, false);
    o.partial = get_or<bool>(p, kPartial, false);
    o.skip_services = get_or<bool>(p, kSkipServices, false);
    o.dump_other = get_or<bool>(p, kDumpOther, false);
    o.decrypt_services = get_or<bool>(p, kDecryptServices, false);
    o.decrypt_files = get_or<bool>(p, kDecryptFiles, false);
    o.save_bytes = get_or<bool>(p, kSaveBytes, false);
    o.save_bytes_path = trim(get_or<std::string>(p, kSaveBytesPath, ""));
    return o;
}

void VideoDatSinkStage::set_status(const std::string& status)
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    trigger_status_ = status;
}

std::string VideoDatSinkStage::get_trigger_status() const
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    return trigger_status_;
}

void VideoDatSinkStage::report(size_t current, size_t total, const std::string& message)
{
    if (progress_callback_) {
        progress_callback_(current, total, message);
    }
}

bool VideoDatSinkStage::trigger(const std::vector<ArtifactPtr>& inputs,
                                const std::map<std::string, ParameterValue>& parameters,
                                IObservationContext&)
{
    is_processing_.store(true);
    cancel_requested_.store(false);
    set_status("Starting VideoDat decoder...");

    auto fail = [&](const std::string& status) {
        set_status(status);
        is_processing_.store(false);
        return false;
    };

    try {
        if (inputs.empty()) {
            return fail("Error: No input connected");
        }
        const auto source = std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
        if (!source) {
            return fail("Error: Input is not a video frame representation");
        }
        const auto params = source->get_video_parameters();
        if (!params || !params->is_valid()) {
            return fail("Error: Input carries no video parameters");
        }
        if (params->system != VideoSystem::PAL) {
            return fail("Error: VideoDat is a PAL service; the input is not PAL");
        }

        const DecoderOptions options = parse_options(parameters);
        std::error_code ec;
        fs::create_directories(fs::u8path(options.output_dir), ec);
        if (ec || !fs::is_directory(fs::u8path(options.output_dir))) {
            return fail("Error: cannot create output directory " + options.output_dir);
        }

        const RepresentationFields fields(*source);
        if (fields.field_count() == 0) {
            return fail("Error: the input has no frames");
        }

        // The decoder's console output: to the log file, and line by line to
        // the progress message.
        const fs::path log_path = fs::u8path(options.output_dir) / kLogName;
        std::ofstream log(log_path, std::ios::trunc);
        std::mutex log_mutex;
        std::string summary;
        size_t done = 0;
        const size_t total = fields.field_count();
        const ::videodat::LogFn log_line = [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(log_mutex);
            log << line << "\n";
            log.flush();
            if (line.find(" Dateien, davon ") != std::string::npos) {
                summary = line;
            }
            if (!line.empty()) {
                report(done, total, line);
            }
        };
        report(0, total, "Searching the VideoDat lines...");
        const auto result = ::videodat::decode_fields(
            fields, options.decode_options(), log_line,
            [&](size_t current, size_t all) {
                done = current;
                if (current % 4096 == 0 || current == all) {
                    report(current, all, "Decoding field " + std::to_string(current) + " of " + std::to_string(all));
                }
            },
            &cancel_requested_);
        log.close();
        if (result.cancelled) {
            return fail("Cancelled");
        }

        report(total, total, "Done");
        set_status("Decoded " + std::to_string(total) + " fields (variant " + result.variant + ")" +
                   (summary.empty() ? "" : "; " + summary) + "; output in " + options.output_dir + ", log " +
                   kLogName);
        is_processing_.store(false);
        return true;
    } catch (const std::exception& e) {
        return fail(std::string("Error: ") + e.what());
    }
}

StagePreviewCapability VideoDatSinkStage::get_preview_capability() const
{
    // Composed by hand (as in the skeleton) so the stage also builds against
    // the header-only in-tree SDK, which links no host libraries.
    StagePreviewCapability capability{};
    if (!cached_input_ || cached_input_->frame_count() == 0) {
        return capability;
    }
    const auto params = cached_input_->get_video_parameters();
    if (!params.has_value() || !params->is_valid()) {
        return capability;
    }
    const bool is_yc = cached_input_->has_separate_channels();
    VideoDataType data_type;
    if (params->system == VideoSystem::NTSC || params->system == VideoSystem::PAL_M) {
        data_type = is_yc ? VideoDataType::YC_NTSC : VideoDataType::CompositeNTSC;
    } else {
        data_type = is_yc ? VideoDataType::YC_PAL : VideoDataType::CompositePAL;
    }
    const auto active_width =
        params->active_video_end > params->active_video_start
            ? static_cast<uint32_t>(params->active_video_end - params->active_video_start)
            : static_cast<uint32_t>(params->frame_width_nominal);
    const auto active_height =
        params->last_active_frame_line > params->first_active_frame_line
            ? static_cast<uint32_t>(params->last_active_frame_line - params->first_active_frame_line)
            : static_cast<uint32_t>(params->frame_height);
    double dar_correction = 1.0;
    if (active_width > 0 && active_height > 0) {
        dar_correction = (4.0 / 3.0) /
                         (static_cast<double>(active_width) / static_cast<double>(active_height));
    }
    capability.supported_data_types = {data_type};
    capability.navigation_extent.item_count = cached_input_->frame_count();
    capability.navigation_extent.granularity = 1;
    capability.navigation_extent.item_label = "frame";
    capability.geometry.active_width = active_width;
    capability.geometry.active_height = active_height;
    capability.geometry.display_aspect_ratio = 4.0 / 3.0;
    capability.geometry.dar_correction_factor = dar_correction;
    return capability;
}

} // namespace orc::plugins::videodat
