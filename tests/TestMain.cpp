// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's test runner: doctest (MIT, bundled with Tracktion Engine) inside a JUCE environment.
// Run all tests with `ctest --preset <preset>`, or run the executable directly for doctest's options,
// e.g. `phazite_tests --test-case="*project*"` or `phazite_tests --help`.

#define DOCTEST_CONFIG_IMPLEMENT
#include "TestHelpers.h"

int main (int argc, char** argv)
{
    // Tracktion's Engine and Edits expect JUCE's message manager, as in the app. The tests run on this
    // thread, which becomes the message thread.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    doctest::Context context (argc, argv);
    return context.run();
}
