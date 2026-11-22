// SPDX-License-Identifier: AGPL-3.0-or-later
// How editors reach Phazite's own devices (src/model/devices/DeviceHandle.h), the knobs a device gets until it has an
// editor of its own (src/ui/devices/DeviceEditors.h), what every device's editor repaints, how editors lay out their parts
// (src/ui/devices/DeviceLayout.h), and the curves their graphs draw (src/ui/devices/DeviceGraph.h).
#include "EnginePlayback.h"
#include "GoldenFiles.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/saturation/Saturator.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/components/DisplayTicker.h"
#include "ui/devices/CompressorEditor.h"
#include "ui/devices/DeviceEditorBase.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/DeviceGraph.h"
#include "ui/devices/DeviceLayout.h"
#include "ui/devices/ParameterControls.h"
#include "ui/theme/DesignTokens.h"

namespace
{
using phazite::devices::Equalizer;

// A device of a type at the end of a song's first track's devices (in place of its instrument, for an instrument).
phazite::DeviceID addDevice (phazite::Song& song, const char* type)
{
    const auto id = phazite::test::addOwnDevice (song, phazite::getTrackIDs (song)[0], type);
    REQUIRE (id.isValid());
    return id;
}

// A song outside a project with a device of a type on its track, open in a context of its own, whose session plays it.
struct SongWithDevice
{
    explicit SongWithDevice (const char* type)
    {
        phazite::setSong (context, song.get());
        id = addDevice (*song, type);
        phazite::getSession (context)->update();
    }

    ~SongWithDevice() { phazite::setSong (context, nullptr); }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::DeviceID id;
};

// A menu's items as text, without its header (the parameter's name): a separator as "-", a submenu's items after its name
// and ">".
juce::StringArray describeItems (const juce::PopupMenu& menu)
{
    juce::StringArray items;

    for (juce::PopupMenu::MenuItemIterator item (menu); item.next();)
    {
        const auto& each = item.getItem();

        if (each.isSectionHeader)
            continue;

        auto text = each.isSeparator ? juce::String ("-") : each.text;

        if (each.subMenu != nullptr)
            text << " > " << describeItems (*each.subMenu).joinIntoString (", ");

        items.add (text);
    }

    return items;
}

// The header of a menu: the parameter's name.
juce::String getHeader (const juce::PopupMenu& menu)
{
    for (juce::PopupMenu::MenuItemIterator item (menu); item.next();)
        if (item.getItem().isSectionHeader)
            return item.getItem().text;

    return {};
}
} // namespace

TEST_CASE ("A device handle reaches one of Phazite's devices, and nothing else")
{
    phazite::test::TestPlugin testPlugin;
    auto song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::setSong (context, song.get());
    const auto eq = addDevice (*song, Equalizer::typeID);
    phazite::getSession (context)->update();
    const auto device = phazite::makeDeviceHandle (*song, eq);

    REQUIRE (device.isValid());
    REQUIRE (device.getInfo() != nullptr);
    CHECK (device.getInfo()->name == "EQ");
    CHECK (device.getParameter ("band3Gain").getInfo()->unit == phazite::devices::Unit::decibels);
    CHECK_FALSE (device.getParameter ("volume").isValid());
    CHECK (device.getProcessor<Equalizer>() != nullptr);
    CHECK (device.getProcessor<phazite::devices::Processor>() != nullptr);

    // A plug-in isn't one of Phazite's devices.
    const auto plugin = phazite::test::TestPlugin::add (*song, phazite::getTrackIDs (*song)[0]);
    const auto notOurs = phazite::makeDeviceHandle (*song, plugin);
    CHECK_FALSE (notOurs.isValid());
    CHECK (notOurs.getInfo() == nullptr);
    CHECK (notOurs.getProcessor<Equalizer>() == nullptr);

    // When the project closes, the handle has nothing.
    phazite::setSong (context, nullptr);
    song = nullptr;
    CHECK_FALSE (device.isValid());
    CHECK (device.getProcessor<Equalizer>() == nullptr);
    CHECK_FALSE (device.getParameter ("band3Gain").isValid());
}

TEST_CASE ("A device without an editor of its own gets a knob for each parameter, showing its unit")
{
    auto song = phazite::createSong();
    const auto eq = addDevice (*song, Equalizer::typeID);
    song->getUndoManager().clearUndoHistory();

    phazite::ui::DeviceKnobs knobs ((phazite::makeDeviceHandle (*song, eq)));
    REQUIRE (knobs.getNumKnobs() == 75);

    auto& frequency = knobs.getKnob (Equalizer::getParameterIndex (0, Equalizer::frequency));
    CHECK (frequency.getTitle() == juce::String ("Band 1 Frequency"));
    CHECK (frequency.getTextFromValue (frequency.getValue()) == juce::String ("30.0 Hz"));
    CHECK (frequency.getValueFromText ("1.2k") == doctest::Approx (1200.0));

    // Turning a knob changes its parameter, as an undo step the model names.
    auto& gain = knobs.getKnob (Equalizer::getParameterIndex (2, Equalizer::gain));
    gain.setValue (6.0, juce::sendNotificationSync);
    CHECK (phazite::getValueSetByHand (*song, phazite::ParameterRef { eq, "band3Gain" }) == doctest::Approx (6.0f));
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo Change Band 3 Gain"));

    CHECK (phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (*song, eq)) != nullptr);
    CHECK (phazite::ui::createDeviceEditor (phazite::DeviceHandle()) == nullptr);
}

TEST_CASE ("Every device's editor rests while nothing it shows moves, and shows a value as it changes")
{
    // Their pictures once repainted at each tick of the displays' ticker, 30 times a second, with nothing playing and nothing
    // turned (docs/archive/performance-audit.md, section 8).
    juce::SharedResourcePointer<phazite::ui::DisplayTicker> ticker;
    const auto tick = [&ticker] (int times)
    {
        for (int i = 0; i < times; ++i)
            ticker->tickNow();
    };

    for (const auto* info : phazite::getOwnDevices())
    {
        CAPTURE (std::string (info->name));
        SongWithDevice song (std::string (info->typeID).c_str());
        auto editor = phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (*song.song, song.id));
        REQUIRE (editor != nullptr);

        {
            phazite::test::TestWindow window (*editor, std::max (1200, editor->getWidth()), std::max (700, editor->getHeight()));

            // Long enough for what falls back to rest: a compressor's dot falls from its lowest level shown to silence.
            tick (200);
            window.takeRepaintedArea();
            tick (30);
            CHECK (window.takeRepaintedArea().isEmpty());

            // A value turned elsewhere (automation, say), other than the mix, which the header shows, and one the editor has a
            // control for now (the drum sampler's show its selected pad's): its control at least shows it.
            const auto* base = dynamic_cast<const phazite::ui::DeviceEditorBase*> (editor.get());
            const auto shown = std::find_if (info->parameters.begin(), info->parameters.end(),
                                             [base] (const auto& parameter)
                                             {
                                                 return parameter.id != phazite::devices::mixParameterID
                                                        && (base == nullptr || base->findControl (parameter.id) != nullptr);
                                             });
            REQUIRE (shown != info->parameters.end());
            const phazite::ParameterRef parameter { song.id, juce::String (shown->id.data(), shown->id.size()) };
            REQUIRE (phazite::describeParameter (*song.song, parameter).has_value());
            phazite::setValueSetByHand (*song.song, parameter,
                                        juce::approximatelyEqual (shown->defaultValue, shown->maximum) ? shown->minimum : shown->maximum);
            tick (1);
            CHECK_FALSE (window.takeRepaintedArea().isEmpty());
        }

        editor = nullptr; // before its song
    }
}

TEST_CASE ("A graph's curve is one line from end to end, through the level at each step")
{
    // Every point began a path of its own (Path::isEmpty() holds until a path has a line), so that no graph drew its line,
    // and a wash under one was a wedge from its last point (2026-11-01, the multiband's curve).
    const juce::Rectangle<float> area (10.0f, 20.0f, 100.0f, 50.0f);
    const auto curve = phazite::ui::graph::makeCurve (area, [] (float x) { return x < 60.0f ? 70.0f : 20.0f; }, 1.0f);
    juce::Path::Iterator element (curve);
    int starts = 0, lines = 0;

    while (element.next())
    {
        if (element.elementType == juce::Path::Iterator::startNewSubPath)
            ++starts;
        else if (element.elementType == juce::Path::Iterator::lineTo)
            ++lines;
    }

    CHECK (starts == 1);
    CHECK (lines == 100);
    CHECK (curve.getBounds() == area);

    // Under it, the area below the line: low to the left, high to the right.
    auto wash = curve;
    wash.lineTo (area.getBottomRight());
    wash.lineTo (area.getBottomLeft());
    wash.closeSubPath();
    CHECK (! wash.contains (30.0f, 60.0f));
    CHECK (wash.contains (90.0f, 30.0f));
}

TEST_CASE ("A device's layout narrows its graphs first, then shows its columns in pages named by them, every part under its title")
{
    using phazite::ui::DeviceLayout;
    juce::Component editor, graph, knobs, more, other;
    DeviceLayout layout (editor, { { { "Graph", &graph, 300, 200, 120, true, 200 } },
                                   { { "Knobs", &knobs, 160, 96, 96 }, { "More", &more, 160, 72, 72 } },
                                   { { "Other", &other, 200, 120, 120 } } });
    constexpr int gap = phazite::ui::Space::m; // between columns
    CHECK (layout.getWidth() == 300 + 160 + 200 + 2 * gap);
    CHECK (layout.getNarrowestWidth() == 200 + 160 + 200 + 2 * gap);

    // A little narrower than its columns side by side: the graph narrows, the others keep their widths; no pages.
    layout.layOut ({ 0, 0, 600, 240 });
    CHECK_FALSE (layout.usesPages());
    CHECK (graph.getWidth() == 600 - 160 - 200 - 2 * gap);
    CHECK (knobs.getWidth() == 160);
    CHECK (other.isVisible());

    // Narrower, in a short area: pages, as many columns on each as fit, named by their first parts.
    layout.layOut ({ 0, 0, 400, 240 });
    REQUIRE (layout.usesPages());
    CHECK (layout.getNumPages() == 2);
    CHECK (layout.getPageName (0) == juce::String::fromUTF8 ("Graph \xc2\xb7 Knobs"));
    CHECK (layout.getPageName (1) == "Other");
    CHECK (graph.isVisible());
    CHECK_FALSE (other.isVisible());
    CHECK (layout.getMinimumHeightForWidth (400) > layout.getMinimumHeight()); // its pages' bar
    CHECK (layout.getMinimumHeightForWidth (600) == layout.getMinimumHeight());

    // The bar of pages over the parts, each part under its title still (the owner, 2026-11-06: every block names itself).
    CHECK (graph.getY() == DeviceLayout::pageBarHeight + phazite::ui::Space::xs + DeviceLayout::titleHeight);
    CHECK (knobs.getY() == graph.getY());
    CHECK (more.getY() >= knobs.getBottom() + DeviceLayout::titleHeight);

    // Its other page shows in its place.
    layout.showPage (1);
    CHECK (other.isVisible());
    CHECK_FALSE (graph.isVisible());
    CHECK_FALSE (knobs.isVisible());

    // In a taller area as narrow, its columns wrap into rows instead, every part showing.
    layout.layOut ({ 0, 0, 400, 600 });
    CHECK_FALSE (layout.usesPages());
    CHECK (graph.isVisible());
    CHECK (other.isVisible());
    CHECK (other.getY() > graph.getBottom());
}

TEST_CASE ("A pinned column leads every page of a device's layout, without naming any; a column's name names its page")
{
    using phazite::ui::DeviceLayout;
    juce::Component editor, picker, first, second;
    DeviceLayout layout (editor, { { { "Band", &picker, 72, 200, 200 } },
                                   { { "First", &first, 300, 200, 150, true } },
                                   { { "Second part", &second, 300, 200, 150, true } } });
    layout.setPinned (0);
    layout.setColumnName (2, "Second");
    CHECK (layout.getMinimumWidth() == 72 + phazite::ui::Space::m + 300);

    layout.layOut ({ 0, 0, 400, 240 });
    REQUIRE (layout.getNumPages() == 2);
    CHECK (layout.getPageName (0) == "First");
    CHECK (layout.getPageName (1) == "Second");
    CHECK (picker.isVisible());
    CHECK (first.isVisible());
    CHECK_FALSE (second.isVisible());

    layout.showPage (1);
    CHECK (picker.isVisible());
    CHECK (second.isVisible());
    CHECK_FALSE (first.isVisible());
    CHECK (second.getX() > picker.getRight());
}

TEST_CASE ("A layout's pages' bar shows whole the tabs that fit, and the other pages in its More tab's menu, the one shown ticked")
{
    // The tabs once shrank to the room left, down to nothing: in a narrow dock a page's name was cut, and the last ones vanished.
    const phazite::test::ScopedDisplay display; // JUCE places a menu on its display
    using phazite::ui::DeviceLayout;
    juce::Component editor;
    juce::OwnedArray<juce::Component> contents;
    std::vector<DeviceLayout::Column> columns;
    const juce::StringArray names { "Alpha Part", "Bravo Part", "Charlie Part", "Delta Part", "Echo Part" };

    for (const auto& name : names)
        columns.push_back ({ { name, contents.add (new juce::Component()), 200, 120, 120 } });

    DeviceLayout layout (editor, std::move (columns));
    phazite::test::TestWindow window (editor, 220, 240);
    layout.layOut ({ 0, 0, 220, 240 });
    REQUIRE (layout.getNumPages() == 5);
    const auto inBar = layout.getNumPagesInBar();
    REQUIRE (inBar >= 1);
    REQUIRE (inBar < 5);

    // Each tab in the bar whole, as wide as its name at least; the others hidden; the More tab after them, in the bar.
    const auto font = juce::Font (juce::FontOptions (phazite::ui::TextSize::small));
    auto* more = layout.getMoreTab();
    REQUIRE (more != nullptr);
    CHECK (more->isVisible());
    CHECK (more->getTitle() == "More Pages");
    CHECK_FALSE (more->getToggleState());

    for (int page = 0; page < 5; ++page)
    {
        CAPTURE (page);
        auto* tab = layout.getPageTab (page);
        REQUIRE (tab != nullptr);
        CHECK (tab->isVisible() == (page < inBar));
        CHECK (tab->getTitle() == names[page]);

        if (page < inBar)
        {
            CHECK ((float) tab->getWidth() >= juce::GlyphArrangement::getStringWidth (font, names[page]));
            CHECK (tab->getRight() <= more->getX());
        }
    }

    CHECK (more->getRight() <= 220);

    // Its menu: the hidden pages in order, none ticked while a page of the bar shows.
    const auto describe = [&layout]
    {
        juce::StringArray items;
        const auto menu = layout.getMorePagesMenu();

        for (juce::PopupMenu::MenuItemIterator item (menu); item.next();)
            items.add (item.getItem().text + (item.getItem().isTicked ? " (ticked)" : ""));

        return items;
    };

    juce::StringArray hidden;

    for (int page = inBar; page < 5; ++page)
        hidden.add (names[page]);

    CHECK (describe() == hidden);

    // Choosing the last shows its parts; the More tab reads as its name, lit, and its menu ticks it.
    auto menu = layout.getMorePagesMenu();
    juce::PopupMenu::MenuItemIterator last (menu);
    const juce::PopupMenu::Item* chosen = nullptr;

    while (last.next())
        chosen = &last.getItem();

    REQUIRE (chosen != nullptr);
    REQUIRE (chosen->action != nullptr);
    chosen->action();
    CHECK (layout.getShownPage() == 4);
    CHECK (contents[4]->isVisible());
    CHECK_FALSE (contents[0]->isVisible());
    CHECK (more->getButtonText() == "Echo Part");
    CHECK (more->getToggleState());
    CHECK (more->getTitle().contains ("Echo Part"));
    CHECK (more->getRight() <= 220);
    CHECK (describe().contains ("Echo Part (ticked)"));

    // The keyboard reaches every page: the arrows along the bar to the More tab, whose menu Space shows.
    const auto press = [] (juce::Component& tab, int key) { return tab.keyPressed (juce::KeyPress (key)); };
    auto* first = layout.getPageTab (0);
    first->grabKeyboardFocus();
    REQUIRE (first->hasKeyboardFocus (false));

    for (int page = 1; page < layout.getNumPagesInBar(); ++page)
    {
        CHECK (press (*layout.getPageTab (page - 1), juce::KeyPress::rightKey));
        CHECK (layout.getPageTab (page)->hasKeyboardFocus (false));
    }

    press (*layout.getPageTab (layout.getNumPagesInBar() - 1), juce::KeyPress::rightKey);
    CHECK (more->hasKeyboardFocus (false));
    press (*more, juce::KeyPress::leftKey);
    CHECK (layout.getPageTab (layout.getNumPagesInBar() - 1)->hasKeyboardFocus (false));
    more->grabKeyboardFocus();
    REQUIRE (juce::Component::getCurrentlyModalComponent() == nullptr);
    CHECK (press (*more, juce::KeyPress::spaceKey));
    phazite::test::dispatchPendingMessages(); // a button's click by a key comes as a message
    // The menu is up, unless JUCE has already taken it down again: its window's timer does that where the program isn't the
    // foreground one, which a test on a machine without a screen never is (this check failed so once under CI's linux-asan,
    // 2026-11-22, as the right-click's below did before it).
    CHECK ((juce::Component::getCurrentlyModalComponent() != nullptr || ! juce::Process::isForegroundProcess()));
    juce::PopupMenu::dismissAllActiveMenus();
    phazite::test::dispatchPendingMessages();

    // A page of the bar shown by its tab: the More tab is its icon alone again.
    layout.getPageTab (0)->triggerClick();
    phazite::test::dispatchPendingMessages();
    CHECK (layout.getShownPage() == 0);
    CHECK (more->getButtonText().isEmpty());
    CHECK_FALSE (more->getToggleState());

    // With room for every tab, no More tab.
    juce::Component wideEditor, a, b;
    DeviceLayout wide (wideEditor, { { { "A", &a, 200, 120, 120 } }, { { "B", &b, 200, 120, 120 } } });
    wide.layOut ({ 0, 0, 300, 240 });
    REQUIRE (wide.getNumPages() == 2);
    CHECK (wide.getNumPagesInBar() == 2);
    CHECK_FALSE (wide.getMoreTab()->isVisible());
    CHECK (wide.getPageTab (1)->isVisible());
}

TEST_CASE ("A part's switch shows in its tab where its column shows its parts under tabs, and switches it there, one undo step")
{
    using phazite::devices::Saturator;
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto saturator = song.addEffect (song.firstTrack, Saturator::typeID);
    auto editor = phazite::ui::createDeviceEditor (phazite::getDeviceHandle (song.context, saturator));
    auto* base = dynamic_cast<phazite::ui::DeviceEditorBase*> (editor.get());
    REQUIRE (base != nullptr);

    // As short as it goes: its Band and Quality under tabs.
    phazite::test::TestWindow window (*editor, base->getWidthForHeight (0), base->getMinimumHeight());
    auto& layout = base->getLayout();
    constexpr int column = 2, band = 0, quality = 1;
    REQUIRE (layout.getTitle (column, band) == "Band");
    REQUIRE (layout.usesTabs (column));
    phazite::test::checkChildrenApart (*editor);

    auto* tab = layout.getPartTab (column, band);
    auto* partSwitch = base->findControl ("band");
    REQUIRE (tab != nullptr);
    REQUIRE (partSwitch != nullptr);
    CHECK (partSwitch->getParentComponent() == tab);
    CHECK (partSwitch->isShowing());
    CHECK (partSwitch->getX() < tab->getWidth() / 2); // at its left
    CHECK (partSwitch->getTitle() == "Band");
    CHECK (phazite::ui::ParameterMenuClicks::getMenuOf (*partSwitch).getNumItems() > 0); // a right-click's menu: Automate, Modulate

    // A click on the switch switches the Band, one undo step, and leaves the part shown as it was.
    layout.showPart (column, quality);
    const phazite::ParameterRef parameter { saturator, "band" };
    const auto before = phazite::getValueSetByHand (s, parameter);
    s.getUndoManager().clearUndoHistory();
    phazite::test::Mouse (*partSwitch).click (partSwitch->getLocalBounds().getCentre());
    CHECK (phazite::getValueSetByHand (s, parameter) != doctest::Approx (before));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Band"));
    CHECK (layout.getShownPart (column) == quality);

    // Off, its tab's name is faint, as its title would be.
    if (phazite::getValueSetByHand (s, parameter) > 0.5f)
        phazite::test::Mouse (*partSwitch).click (partSwitch->getLocalBounds().getCentre());

    CHECK (phazite::getValueSetByHand (s, parameter) < 0.5f);
    CHECK (layout.isPartOff (column, band));
    CHECK_FALSE (layout.isPartOff (column, quality));

    // A click beside it shows the part, and switches nothing.
    const auto value = phazite::getValueSetByHand (s, parameter);
    phazite::test::Mouse (*tab).click ({ tab->getWidth() - 4, tab->getHeight() / 2 });
    CHECK (layout.getShownPart (column) == band);
    CHECK (phazite::getValueSetByHand (s, parameter) == doctest::Approx (value));

    // Tall enough for its parts stacked: the switch back at its title's right end.
    editor->setSize (base->getWidthForHeight (0), base->getHeightForWidth (base->getWidthForHeight (0)));
    CHECK_FALSE (layout.usesTabs (column));
    CHECK (partSwitch->getParentComponent() == editor.get());
    CHECK (partSwitch->isShowing());
    phazite::test::checkChildrenApart (*editor);
    editor = nullptr; // before its song
}

TEST_CASE ("Every editor's parts are titled in Title Case, as the look's section titles are, and drawn as given")
{
    // The frame drew them in capitals (docs/vscode-look.md: section titles in Title Case, as the Modern UI writes them).
    const juce::StringArray smallWords { "a", "an", "and", "as", "at", "by", "for", "from", "in", "of", "on", "or", "the", "to", "with" };
    const auto isTitleCase = [&smallWords] (const juce::String& title)
    {
        const auto words = juce::StringArray::fromTokens (title, " ", "");

        for (int w = 0; w < words.size(); ++w)
        {
            const auto& word = words[w];

            if (w > 0 && smallWords.contains (word.toLowerCase()))
            {
                if (word != word.toLowerCase())
                    return false;
            }
            else if (juce::CharacterFunctions::isLetter (word[0]) && ! juce::CharacterFunctions::isUpperCase (word[0]))
            {
                return false;
            }
        }

        return title.isNotEmpty();
    };

    CHECK (isTitleCase ("Sub and Noise"));
    CHECK (isTitleCase ("LFO 1"));
    CHECK_FALSE (isTitleCase ("Sub And Noise"));
    CHECK_FALSE (isTitleCase ("filter"));

    for (const auto* info : phazite::getOwnDevices())
    {
        CAPTURE (std::string (info->name));
        SongWithDevice song (std::string (info->typeID).c_str());
        auto editor = phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (*song.song, song.id));
        REQUIRE (editor != nullptr);

        if (const auto* base = dynamic_cast<const phazite::ui::DeviceEditorBase*> (editor.get()))
        {
            for (int mode = 0; mode < base->getNumLayouts(); ++mode)
            {
                const auto& layout = base->getModeLayout (mode);

                for (int column = 0; column < layout.getNumColumns(); ++column)
                {
                    for (int part = 0; part < layout.getNumParts (column); ++part)
                    {
                        CAPTURE (layout.getTitle (column, part));
                        CHECK (isTitleCase (layout.getTitle (column, part)));

                        if (auto* tab = layout.getPartTab (column, part))
                            CHECK (tab->getButtonText() == layout.getTitle (column, part)); // a tab's text as given too
                    }
                }
            }
        }

        editor = nullptr; // before its song
    }
}

TEST_CASE ("A right-click on a switch, a list, a choice's button or tab, or a part's switch shows its parameter's menu, a knob's "
           "but for Type a Value...; a left click still switches or chooses, one undo step")
{
    const phazite::test::ScopedDisplay display; // JUCE places a menu on its display
    juce::SharedResourcePointer<phazite::ui::DisplayTicker> ticker;
    using phazite::devices::Compressor;
    using phazite::devices::Saturator;
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto track = song.firstTrack;
    const auto compressor = song.addEffect (track, Compressor::typeID);
    const auto saturator = song.addEffect (track, Saturator::typeID);

    phazite::ui::CompressorEditor compressorEditor (phazite::getDeviceHandle (song.context, compressor));
    phazite::test::TestWindow compressorWindow (compressorEditor, compressorEditor.getWidth(), compressorEditor.getHeight());
    auto saturatorEditor = phazite::ui::createDeviceEditor (phazite::getDeviceHandle (song.context, saturator));
    REQUIRE (saturatorEditor != nullptr);
    phazite::test::TestWindow saturatorWindow (*saturatorEditor, saturatorEditor->getWidth(), saturatorEditor->getHeight());
    auto* saturatorBase = dynamic_cast<phazite::ui::DeviceEditorBase*> (saturatorEditor.get());
    REQUIRE (saturatorBase != nullptr);

    // A toggle and a list as a grid of a device's controls has them (ParameterControls), and a knob, to compare menus with.
    juce::Component holder;
    juce::ToggleButton toggle;
    phazite::ui::ToggleAttachment toggleAttachment (toggle);
    toggleAttachment.setParameter (phazite::getParameter (song.context, { saturator, "band" }));
    juce::ComboBox box;
    phazite::ui::ChoiceAttachment boxAttachment (box);
    boxAttachment.setParameter (phazite::getParameter (song.context, { compressor, "character" }));
    phazite::ui::Knob knob ("Attack", 0.0);
    phazite::ui::ParameterAttachment knobAttachment (knob);
    knobAttachment.setParameter (phazite::getParameter (song.context, { compressor, "attack" }));
    holder.addAndMakeVisible (toggle);
    holder.addAndMakeVisible (box);
    holder.addAndMakeVisible (knob);
    toggle.setBounds (10, 10, 30, 30);
    box.setBounds (60, 10, 120, 30);
    knob.setBounds (200, 10, 40, 40);
    phazite::test::TestWindow holderWindow (holder, 260, 60);

    auto* characters = dynamic_cast<phazite::ui::ChoiceButtons*> (compressorEditor.findControl ("character"));
    REQUIRE (characters != nullptr);
    auto* partSwitch = saturatorBase->findControl ("band");
    REQUIRE (partSwitch != nullptr);
    auto* autoMakeup = compressorEditor.findControl ("autoMakeup");
    REQUIRE (autoMakeup != nullptr);

    struct Kind
    {
        const char* what;
        juce::Component* control;
        phazite::ParameterRef parameter;
        float clickedTo; // what a left click sets it to
    };

    // The mode's tab last: Duck's page hides Compress's controls.
    const std::vector<Kind> kinds {
        { "a juce::ToggleButton (ToggleAttachment)", &toggle, { saturator, "band" }, 1.0f },
        { "a list (ChoiceAttachment)", &box, { compressor, "character" }, 2.0f },
        { "a switch (ParameterSwitch)", autoMakeup, { compressor, "autoMakeup" }, 1.0f },
        { "a choice's button (ChoiceButtons)", &characters->getButton (1), { compressor, "character" }, 1.0f },
        { "a part's switch", partSwitch, { saturator, "band" }, 0.0f },
        { "a mode's tab (ChoiceTabs)", &compressorEditor.getModeTab (1), { compressor, "mode" }, 1.0f },
    };

    for (const auto& kind : kinds)
    {
        CAPTURE (std::string (kind.what));
        REQUIRE (kind.control->isShowing());
        REQUIRE (juce::Component::getCurrentlyModalComponent() == nullptr); // no menu left open
        auto* top = kind.control->getTopLevelComponent();
        auto* hit = top->getComponentAt (top->getLocalPoint (kind.control, kind.control->getLocalBounds().getCentre()));
        CAPTURE (hit != nullptr ? hit->getTitle() + " / " + hit->getName() : juce::String ("nothing"));
        REQUIRE ((hit == kind.control || kind.control->isParentOf (hit))); // a click at its middle reaches it
        auto handle = phazite::getParameter (song.context, kind.parameter);
        REQUIRE (handle.exists());
        const auto valueOf = [&s, &kind] { return phazite::getValueSetByHand (s, kind.parameter); };
        s.getUndoManager().clearUndoHistory();

        // The knob's menu less its first two items (Type a Value... and the line under it), under the parameter's name.
        auto menu = phazite::ui::ParameterMenuClicks::getMenuOf (*kind.control);
        auto knobItems = describeItems (knobAttachment.getMenu());
        REQUIRE (knobItems.size() > 2);
        CHECK (knobItems[0] == juce::String ("Type a Value..."));
        knobItems.removeRange (0, 2);
        CHECK (describeItems (menu) == knobItems);
        CHECK (getHeader (menu) == handle.getName());
        CHECK (phazite::test::findMenuItem (menu, { "Type a Value..." }) == nullptr);
        REQUIRE (phazite::test::findMenuItem (menu, { "Delete Automation" }) != nullptr);
        CHECK_FALSE (phazite::test::findMenuItem (menu, { "Delete Automation" })->isEnabled);

        // A right-click shows it, and changes nothing.
        const auto before = valueOf();
        phazite::test::Mouse mouse (*kind.control);
        mouse.click (kind.control->getLocalBounds().getCentre(), juce::ModifierKeys::rightButtonModifier);
        // The menu is up, unless JUCE has already taken it down again: its window's timer does that where the program isn't
        // the foreground one, which a test on a machine without a screen never is (the EQ's case failed so once on Linux CI,
        // 2026-11-19: its picture takes longest to paint, and the timer came first).
        CHECK ((juce::Component::getCurrentlyModalComponent() != nullptr || ! juce::Process::isForegroundProcess()));
        juce::PopupMenu::dismissAllActiveMenus();
        phazite::test::dispatchPendingMessages();
        CHECK (valueOf() == doctest::Approx (before));
        CHECK_FALSE (s.getUndoManager().canUndo());
        CHECK_FALSE (box.isPopupActive());

        // The switch shows the value set by hand, which nothing moves here. A control follows a change made elsewhere (an
        // earlier round's click on another control of the same parameter) at the displays' next tick: ticked here, as a
        // release build runs this whole case within one frame (CI's linux-release job read the part's switch before any
        // tick).
        ticker->tickNow();

        if (auto* button = dynamic_cast<juce::Button*> (kind.control); button != nullptr && button->getClickingTogglesState())
            CHECK (button->getToggleState() == (valueOf() > 0.5f));

        // Automate shows its lane; Delete Automation, enabled while it has automation, takes it away.
        REQUIRE (phazite::test::runMenuItem (menu, { "Automate" }));
        const auto lanes = phazite::getAutomationLanes (song.context, track);
        CHECK (std::any_of (lanes.begin(), lanes.end(), [&kind] (const auto& lane) { return lane.parameter == kind.parameter; }));
        phazite::addAutomationPoint (song.context, kind.parameter, { phazite::BeatPosition(), 0.0f });
        CHECK (phazite::isAutomated (handle));
        menu = phazite::ui::ParameterMenuClicks::getMenuOf (*kind.control);
        REQUIRE (phazite::test::runMenuItem (menu, { "Delete Automation" }));
        CHECK_FALSE (phazite::isAutomated (handle));

        // Modulate > New LFO modulates it.
        const auto modulationsBefore = phazite::getModulations (handle).size();
        REQUIRE (phazite::test::runMenuItem (menu, { "Modulate", "New LFO" }));
        CHECK (phazite::getModulations (handle).size() == modulationsBefore + 1);

        // Undone, so that what follows clicks a parameter that nothing moves: a moved one's value is what the engine played
        // last, which plays no blocks here.
        phazite::undo (s);
        CHECK (phazite::getModulations (handle).size() == modulationsBefore);

        // A left click switches it or chooses, as one undo step; a list's opens its own popup, from which a choice is one step.
        s.getUndoManager().clearUndoHistory();
        phazite::test::UndoCheck undo (song.context);

        if (kind.control == &box)
        {
            mouse.click (box.getLocalBounds().getCentre());
            CHECK (box.isPopupActive());
            phazite::test::dispatchPendingMessages(); // JUCE shows it a message later
            juce::PopupMenu::dismissAllActiveMenus();
            phazite::test::dispatchPendingMessages();
            undo.check ("a choice from the list", [&box] { box.setSelectedItemIndex (2, juce::sendNotificationSync); });
        }
        else
        {
            undo.check ("a click", [&mouse, &kind] { mouse.click (kind.control->getLocalBounds().getCentre()); });
        }

        CHECK (undo.getNumSteps() == 1);
        CHECK (valueOf() == doctest::Approx (kind.clickedTo));
        CHECK_FALSE (phazite::Gesture::isInProgress (s));
    }
}

TEST_CASE ("A switch that a lane moves shows the lane's value as the song plays, as a knob shows its own")
{
    using phazite::devices::Saturator;
    phazite::test::TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto saturator = song.addEffect (track, Saturator::typeID);
    const phazite::ParameterRef band { saturator, "band" };
    song.setParameter (saturator, "band", 0.0f);

    // Off for the first two beats, on from there.
    song.addAutomation (band, { { phazite::BeatPosition(), 0.0f },
                                { phazite::BeatPosition::fromBeats (1.99), 0.0f },
                                { phazite::BeatPosition::fromBeats (2.0), 1.0f },
                                { phazite::BeatPosition::fromBeats (16.0), 1.0f } });

    auto editor = phazite::ui::createDeviceEditor (phazite::getDeviceHandle (song.context, saturator));
    REQUIRE (editor != nullptr);
    phazite::test::TestWindow window (*editor, editor->getWidth(), editor->getHeight());
    auto* base = dynamic_cast<phazite::ui::DeviceEditorBase*> (editor.get());
    REQUIRE (base != nullptr);
    auto* partSwitch = dynamic_cast<juce::Button*> (base->findControl ("band"));
    REQUIRE (partSwitch != nullptr);
    REQUIRE (partSwitch->isShowing());
    juce::SharedResourcePointer<phazite::ui::DisplayTicker> ticker;

    phazite::test::EnginePlayback playback;
    song.run ("transport.playStop");
    playback.play (0.5); // to beat 1
    ticker->tickNow();
    CHECK_FALSE (partSwitch->getToggleState());

    playback.play (1.0); // to beat 3
    ticker->tickNow();
    CHECK (partSwitch->getToggleState());
    song.run ("transport.playStop");
}
