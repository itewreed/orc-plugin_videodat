/*
 * File:        plugin.h
 * Module:      orc-stage-plugin-videodat
 * Purpose:     Plugin entrypoint metadata for VideoDatSinkStage
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include <orc/abi/orc_plugin_sdk.h>

#ifndef ORC_STAGE_PLUGIN_VERSION
#define ORC_STAGE_PLUGIN_VERSION "dev"
#endif

namespace orc::plugins::videodat {

// Stage identifier used during plugin registration and project serialization.
// Deliberately without "sink" in the name: the host GUI opens a save dialog
// for every path parameter of a stage whose name contains "sink", and the
// decoder script is an input file. Output paths set output_path explicitly.
inline constexpr const char* kStageName = "videodat_decoder";

inline constexpr const char* kStageDisplayName = "VideoDat Decoder";

// A sink: it consumes the video and writes the decoded files; the Add Stage
// menu group (Sink) is derived from this by the SDK.
inline constexpr orc::NodeType kStageNodeType = NodeType::SINK;

inline constexpr uint32_t kStageMinInputs = 1;
inline constexpr uint32_t kStageMaxInputs = 1;
inline constexpr uint32_t kStageMinOutputs = 0;
inline constexpr uint32_t kStageMaxOutputs = 0;

// VideoDat was broadcast by Pro7 in PAL only.
inline constexpr orc::VideoFormatCompatibility kStageCompatibleFormats =
    VideoFormatCompatibility::PAL_ONLY;

inline constexpr orc::StageCategory kStageCategory = stage_category_for(kStageNodeType);

static_assert(kStageMaxInputs >= kStageMinInputs, "kStageMaxInputs must be >= kStageMinInputs");
static_assert(kStageMaxOutputs >= kStageMinOutputs, "kStageMaxOutputs must be >= kStageMinOutputs");

inline constexpr orc::StagePluginDescriptor kPluginDescriptor =
    ORC_STAGE_PLUGIN_DESCRIPTOR(
        "io.github.itewreed.stage.videodat_decoder",
        ORC_STAGE_PLUGIN_VERSION,
        "GPL-3.0-or-later",
        false);

} // namespace orc::plugins::videodat
