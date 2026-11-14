// SPDX-License-Identifier: AGPL-3.0-or-later
// How much memory the test process holds, from the operating system, for the long run (LongRun.cpp): on Windows its
// private bytes (what it has committed for itself, whether in RAM or paged out), elsewhere its resident set (from
// /proc/self/statm). In a file of its own so that Windows' headers meet no other.
#pragma once

#include <cstdint>

namespace phazite::test
{
// In bytes; 0 where the system doesn't say.
std::int64_t getProcessMemoryBytes();
} // namespace phazite::test
