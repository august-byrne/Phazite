// SPDX-License-Identifier: AGPL-3.0-or-later
// Rubber Band Library 4.0 (GPL-2.0-or-later, external/rubberband), which Stretch.cpp uses, compiled from its single-file
// build: its built-in FFT and resampler, no threads of its own. Its code is third-party, so its warnings are off here
// (and for GCC and Clang in CMakeLists.txt): every warning a build prints stays Phazite's own.
#if defined(_MSC_VER)
 #pragma warning(push, 0)
 #define WIN32_LEAN_AND_MEAN 1
 #define NOMINMAX 1
#endif

#include <single/RubberBandSingle.cpp>

#if defined(_MSC_VER)
 #pragma warning(pop)
#endif
