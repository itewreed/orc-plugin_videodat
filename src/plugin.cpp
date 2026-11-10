/*
 * File:        plugin.cpp
 * Module:      orc-stage-plugin-videodat
 * Purpose:     Runtime plugin bundle for VideoDatSinkStage
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include "plugin.h"
#include "videodat_sink_stage.h"

namespace {

orc::DAGStagePtr create_stage()
{
    return std::make_shared<orc::plugins::videodat::VideoDatSinkStage>();
}

} // namespace

ORC_STAGE_PLUGIN_EXPORT const orc::StagePluginDescriptor* orc_get_stage_plugin_descriptor()
{
    return &orc::plugins::videodat::kPluginDescriptor;
}

ORC_STAGE_PLUGIN_EXPORT bool orc_register_stage_plugin(
    const orc::OrcPluginServices* services,
    void* context,
    bool (*register_stage)(void* context, const char* stage_name, orc::OrcStageFactoryFn factory),
    const char** error_message)
{
    orc::plugin::set_services(services);

    if (!register_stage) {
        if (error_message) {
            *error_message = "Missing stage registration callback";
        }
        return false;
    }

    const auto node_type_info = create_stage()->get_node_type_info();
    if (node_type_info.display_name != orc::plugins::videodat::kStageDisplayName ||
        node_type_info.type != orc::plugins::videodat::kStageNodeType ||
        node_type_info.min_inputs != orc::plugins::videodat::kStageMinInputs ||
        node_type_info.max_inputs != orc::plugins::videodat::kStageMaxInputs ||
        node_type_info.min_outputs != orc::plugins::videodat::kStageMinOutputs ||
        node_type_info.max_outputs != orc::plugins::videodat::kStageMaxOutputs ||
        node_type_info.compatible_formats != orc::plugins::videodat::kStageCompatibleFormats ||
        node_type_info.category() != orc::plugins::videodat::kStageCategory) {
        if (error_message) {
            *error_message = "Stage metadata mismatch between plugin.h and NodeTypeInfo";
        }
        return false;
    }

    if (!register_stage(context, orc::plugins::videodat::kStageName, &create_stage)) {
        if (error_message) {
            *error_message = "Failed to register stage from plugin metadata";
        }
        return false;
    }

    return true;
}
