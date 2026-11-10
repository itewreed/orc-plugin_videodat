/*
 * File:        plugin_build_info.cpp
 * Module:      orc-plugin-build-info
 * Purpose:     Emit the build facts a release manifest must declare
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 <Your Name>
 */

// Since host ABI 12 every plugin release must ship an
// orc-plugin-manifest.yaml declaring each binary's platform, host ABI,
// toolchain tag and SHA-256 digest (see docs/technical/plugin-publishing.md
// in decode-orc). The ABI number and toolchain tag are properties of the
// compiler that built the plugin, so they cannot be hard-coded in a script
// without eventually drifting from the binary they describe. This tiny
// executable is compiled with the same toolchain and against the same SDK
// headers as the plugin itself, and prints the descriptor's own values;
// scripts/package_local.sh turns them into the manifest.

#include "plugin.h"

#include <cstdio>

int main()
{
    const auto& descriptor = orc::plugins::videodat::kPluginDescriptor;

    std::printf("plugin_id=%s\n", descriptor.plugin_id);
    std::printf("plugin_version=%s\n", descriptor.plugin_version);
    std::printf("stage_name=%s\n", orc::plugins::videodat::kStageName);
    std::printf("abi=%u\n", descriptor.host_abi_version);
    std::printf("api=%u\n", descriptor.plugin_api_version);
    std::printf("toolchain_tag=%s\n", descriptor.toolchain_tag);
    std::printf("license_spdx=%s\n", descriptor.license_spdx);

    return 0;
}
