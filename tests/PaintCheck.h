// SPDX-License-Identifier: AGPL-3.0-or-later
// Paint code run in the tests. A TestWindow paints nothing by itself (its peer only notes what was asked to repaint), so a
// component's paint() runs only where a test paints it: these paint one, its children with it, into an image by JUCE's
// software renderer (the one Linux's windows paint with, and the same on every machine), at an interface scale.
// checkPaints() is the net: painting finishes, raises no JUCE assertion (TestMain.cpp fails the test for one), and draws
// something inside the component's bounds. It keeps no pictures: what a view should look like is its own tests' business.
#pragma once

#include "TestHelpers.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::test
{
// The component and its children as the screen would show them, by the software renderer, scale times as many pixels each
// way as it has (an interface scale: View > Interface Size), on a transparent image.
juce::Image paintComponent (juce::Component&, float scale = 1.0f);

// Whether a picture has a pixel that isn't fully transparent.
bool drawsSomething (const juce::Image&);

// Paints a component that shows (a view in the window, a dialog's panel), at a scale, and checks that it draws something;
// what names it in the failure.
void checkPaints (juce::Component&, const juce::String& what, float scale = 1.0f);
} // namespace phazite::test
