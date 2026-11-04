// SPDX-License-Identifier: AGPL-3.0-or-later
// The CPU time the calling thread has had, in seconds, for the benchmarks of sandboxed plug-ins (SandboxBenchmarks.cpp),
// from the operating system, and a wait for the message thread's messages as the app's message loop waits (Benchmarks.cpp),
// in a file of their own so that Windows' headers meet no other.
#pragma once

namespace phazite::test
{
double getThreadCpuSeconds();
double getProcessCpuSeconds(); // every thread's

// Waits, asleep, until the message thread has a message (a window's or a timer's), as the app's message loop waits, for
// at most so long; false if none came. Elsewhere than on Windows, it sleeps that long and says there may be one.
bool waitForMessages (int timeoutMilliseconds);
} // namespace phazite::test
