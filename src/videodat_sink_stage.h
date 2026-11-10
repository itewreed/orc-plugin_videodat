/*
 * File:        videodat_sink_stage.h
 * Module:      orc-stage-plugin-videodat
 * Purpose:     VideoDat Decoder sink stage: decodes Pro7 VideoDat from the VBI
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include <orc/abi/orc_plugin_sdk.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace orc::plugins::videodat {

// Options of one decoder run, parsed from the stage parameters.
struct DecoderOptions {
    std::string output_dir;
    std::string variant{"auto"};  // auto, 1993, 1990
    std::string lines;            // "" = automatic
    int32_t scan_seconds{30};
    bool no_sync{false};
    bool no_eq{false};
    bool partial{false};
    bool skip_services{false};
    bool dump_other{false};
    bool decrypt_services{false};
    bool decrypt_files{false};
    bool save_bytes{false};
    std::string save_bytes_path;  // "" = <output_dir>/<dir name>.bytes
    std::string python{"python3"};
    std::string decoder_script;   // "" = bundled copy next to the plugin

    // Command line for videodat_decoder.py (without interpreter and script),
    // reading |tbc_path|. Options that only the 1993 variant knows are left
    // out when the 1990 variant is selected.
    std::vector<std::string> decoder_arguments(const std::string& tbc_path) const;

    // The byte-stream file --save-bytes writes, or "" when it is off.
    std::string effective_save_bytes_path() const;
};

// VideoDat Decoder (SINK)
//
// Pro7 VideoDat (1990-1994) is a data service in the vertical blanking
// interval. On trigger the stage copies the first 40 lines of every field of
// its input into a reduced TBC file in the output directory, runs
// videodat_decoder.py on it, and deletes the file afterwards. The decoder
// writes the received files, the service report and its console log
// (videodat_decoder.log) into the output directory.
class VideoDatSinkStage : public DAGStage,
                          public ParameterizedStage,
                          public TriggerableStage,
                          public IStagePreviewCapability {
public:
    VideoDatSinkStage();

    std::string version() const override;
    NodeTypeInfo get_node_type_info() const override;
    ORC_STAGE_INSTRUCTIONS_MD

    std::vector<ArtifactPtr> execute(
        const std::vector<ArtifactPtr>& inputs,
        const std::map<std::string, ParameterValue>& parameters,
        ObservationContext& observation_context) override;

    size_t required_input_count() const override { return 1; }
    size_t output_count() const override { return 0; }

    // ParameterizedStage
    std::vector<ParameterDescriptor> get_parameter_descriptors(
        VideoSystem project_format, SourceType source_type) const override;
    using ParameterizedStage::get_parameter_descriptors;
    std::map<std::string, ParameterValue> get_parameters() const override;
    bool set_parameters(const std::map<std::string, ParameterValue>& params) override;

    // TriggerableStage
    bool trigger(const std::vector<ArtifactPtr>& inputs,
                 const std::map<std::string, ParameterValue>& parameters,
                 IObservationContext& observation_context) override;
    std::string get_trigger_status() const override;
    void set_progress_callback(TriggerProgressCallback callback) override
    {
        progress_callback_ = std::move(callback);
    }
    bool is_trigger_in_progress() const override { return is_processing_.load(); }
    void cancel_trigger() override { cancel_requested_.store(true); }

    // IStagePreviewCapability
    StagePreviewCapability get_preview_capability() const override;

    // Parses a parameter map into decoder options. Throws std::runtime_error
    // with a user-facing message on invalid values.
    static DecoderOptions parse_options(const std::map<std::string, ParameterValue>& parameters);

private:
    void set_status(const std::string& status);
    void report(size_t current, size_t total, const std::string& message);

    std::map<std::string, ParameterValue> parameters_;
    mutable std::mutex status_mutex_;
    std::string trigger_status_{"Idle"};
    TriggerProgressCallback progress_callback_;
    std::atomic<bool> is_processing_{false};
    std::atomic<bool> cancel_requested_{false};
    mutable std::shared_ptr<const VideoFrameRepresentation> cached_input_;
};

} // namespace orc::plugins::videodat
