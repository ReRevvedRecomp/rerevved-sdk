/**
 * @file        memory_section_name.h
 * @brief       Names for the system-wide guest memory section (runtime-internal)
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#pragma once

#include <string>

namespace rex::memory::detail {

/// Next section name for Memory::Initialize: host tick count, process id and a
/// per-process counter, so no two names collide across or within processes.
std::string NextGuestSectionName();

using GuestSectionNameSource = std::string (*)();

/// Test seam: replaces the name source (nullptr restores the default) and
/// returns the previous override.
GuestSectionNameSource SetGuestSectionNameSourceForTesting(GuestSectionNameSource source);

}  // namespace rex::memory::detail
