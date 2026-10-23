/**
 * @file        core/crash_report.h
 * @brief       Internal crash report hooks shared by logging and the handlers.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <vector>

#include <spdlog/common.h>

namespace rex::detail {

// First call installs terminate and Windows exception/abort handlers; each call
// publishes sinks and log_path with .dmp (empty means no dump). The crash path
// never takes the logging lock.
void InstallCrashReport(const std::filesystem::path& log_path,
                        std::vector<spdlog::sink_ptr> flush_sinks);

// Drops the published dump path and sinks, so shutdown can close the log file.
void ClearCrashReportTargets();

}  // namespace rex::detail
