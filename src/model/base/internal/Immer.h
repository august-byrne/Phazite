// SPDX-License-Identifier: AGPL-3.0-or-later
// The immer library's headers that Phazite uses (external/immer; docs/third-party.md), included here alone: every file of
// Phazite's that needs immer includes this, never <immer/...> itself. immer is the model's alone (tools/check-architecture.sh:
// immer-outside-model), and the tests'.
//
// MSVC's optimizer warns C4702 ("unreachable code") at immer/detail/rbts/operations.hpp, in concat_inners_mut(), as it
// generates the release build's code for a transient flex_vector's concatenation (concat_trees_mut(), which ClipsValue.cpp's
// helpers instantiate). The warning is immer's code, not Phazite's, but the SYSTEM mark on immer's folder (CMakeLists.txt,
// /external:W0) doesn't reach it: that mark quiets what the compiler's front end finds as it reads a header, and C4702 comes
// later, from the code generator, which looks at the warning state the pragmas left at the header's lines. So it is turned off
// around immer's headers here, for MSVC alone and for those lines alone; GCC and Clang raise nothing in them.
#pragma once

#if defined(_MSC_VER)
 #pragma warning(push)
 #pragma warning(disable : 4702)
#endif

#include <immer/algorithm.hpp>
#include <immer/flex_vector.hpp>
#include <immer/flex_vector_transient.hpp>
#include <immer/map.hpp>
#include <immer/map_transient.hpp>
#include <immer/vector.hpp>

#if defined(_MSC_VER)
 #pragma warning(pop)
#endif
