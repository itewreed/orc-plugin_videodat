/*
 * File:        decoder_process.h
 * Module:      orc-stage-plugin-videodat
 * Purpose:     Runs videodat_decoder.py as a child process and streams its
 *              console output line by line
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace orc::plugins::videodat {

struct ProcessResult {
    bool started{false};
    bool cancelled{false};
    int exit_code{-1};
    std::string error;  // why the process could not be started
};

// Runs |argv| (argv[0] is looked up in PATH), merging stdout and stderr, and
// calls |on_line| for every output line. Polls |cancel| and terminates the
// child when it becomes true.
ProcessResult run_process(const std::vector<std::string>& argv,
                          const std::function<void(const std::string&)>& on_line,
                          const std::atomic<bool>& cancel);

// Directory of the shared library this function lives in (the plugin), with a
// trailing separator, or "" if it cannot be determined.
std::string plugin_directory();

} // namespace orc::plugins::videodat
