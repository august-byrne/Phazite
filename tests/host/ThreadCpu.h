// SPDX-License-Identifier: AGPL-3.0-or-later
// The CPU time the calling thread has had, in seconds, for the benchmarks of sandboxed plug-ins (SandboxBenchmarks.cpp),
// from the operating system, in a file of its own so that Windows' headers meet no other.
#pragma once

namespace phazite::test
{
double getThreadCpuSeconds();
double getProcessCpuSeconds(); // every thread's
} // namespace phazite::test
