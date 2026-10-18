// SPDX-License-Identifier: AGPL-3.0-or-later
#include "GoldenFiles.h"
#include "model/mixer/internal/Mixer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace phazite::test
{
namespace
{
    constexpr const char* moreDifferences = "(more differences not listed)";

    //==============================================================================
    // N5: values that are never rounded, though they may look like numbers.
    bool isNeverRounded (const juce::String& attribute)
    {
        return attribute == "id" || attribute == "colour" || attribute == "name" || attribute == "source" || attribute == "linkID";
    }

    // A decimal number as N5 rounds it: only digits, signs, points and exponents, with a point or an exponent, all of
    // which std::from_chars reads as a double.
    std::optional<double> readDecimalNumber (const juce::String& text)
    {
        if (text.isEmpty() || ! text.containsOnly ("0123456789+-.eE") || ! text.containsAnyOf (".eE"))
            return std::nullopt;

        const auto utf8 = text.toStdString();
        const auto* const end = utf8.data() + utf8.size();
        double value = 0.0;
        const auto result = std::from_chars (utf8.data(), end, value);

        if (result.ec != std::errc() || result.ptr != end)
            return std::nullopt;

        return value;
    }

    // N5: 7 significant digits, and -0 as 0.
    juce::String writeDecimalNumber (double value)
    {
        if (juce::exactlyEqual (value, 0.0))
            value = 0.0;

        char buffer[32] = {};
        const auto result = std::to_chars (buffer, buffer + sizeof (buffer), value, std::chars_format::general, 7);
        jassert (result.ec == std::errc()); // 32 characters hold any double with 7 digits
        return juce::String (buffer, (size_t) (result.ptr - buffer));
    }

    // N6: tracks of every kind sort as TRACK, and clips of every kind as CLIP, so they keep their order among themselves.
    juce::String getSortKey (const juce::XmlElement& element)
    {
        if (element.isTextElement())
            return "#text";

        const juce::Identifier tag (element.getTagName());

        if (te::TrackList::isTrack (tag))
            return "TRACK";

        if (te::Clip::isClipState (tag))
            return "CLIP";

        return element.getTagName();
    }

    // N1, N2 and N4 to N8 (normaliseProject() does N3 on the result).
    struct Normaliser
    {
        juce::StringArray folderPaths; // the project folder's path, with each separator
        const juce::StringArray& droppableEmptyTags;

        std::unique_ptr<juce::XmlElement> normalise (const juce::XmlElement& saved, bool isRoot) const
        {
            if (saved.isTextElement())
                return std::unique_ptr<juce::XmlElement> (juce::XmlElement::createTextElement (saved.getText()));

            std::vector<std::pair<juce::String, juce::String>> attributes;

            for (const auto& attribute : saved.getAttributeIterator())
            {
                const auto name = attribute.name.toString();

                if (isRoot && name == "lastSignificantChange")
                    continue;

                if (isRoot && (name == "appVersion" || name == "creationTime" || name == "projectID"))
                    attributes.emplace_back (name, "(varies)");
                else
                    attributes.emplace_back (name, normaliseValue (name, attribute.value));
            }

            std::sort (attributes.begin(), attributes.end(),
                       [] (const auto& left, const auto& right) { return left.first.compare (right.first) < 0; });

            auto element = std::make_unique<juce::XmlElement> (saved.getTagName());

            for (const auto& [name, value] : attributes)
                element->setAttribute (name, value);

            std::vector<std::unique_ptr<juce::XmlElement>> children;

            if (! saved.hasTagName ("INPUTDEVICES"))
                for (const auto* child : saved.getChildIterator())
                    if (auto normalisedChild = normalise (*child, false))
                        children.push_back (std::move (normalisedChild));

            std::stable_sort (children.begin(), children.end(),
                              [] (const auto& left, const auto& right) { return getSortKey (*left).compare (getSortKey (*right)) < 0; });

            for (auto& child : children)
                element->addChildElement (child.release());

            if (! isRoot && element->getNumAttributes() == 0 && element->getFirstChildElement() == nullptr
                && droppableEmptyTags.contains (element->getTagName()))
                return {};

            return element;
        }

        // N4 and N5.
        juce::String normaliseValue (const juce::String& attribute, juce::String value) const
        {
            for (const auto& path : folderPaths)
                value = value.replace (path, "$PROJECT");

            if (attribute == "source")
                value = value.replaceCharacter ('\\', '/');

            if (! isNeverRounded (attribute))
                if (const auto number = readDecimalNumber (value))
                    return writeDecimalNumber (*number);

            return value;
        }
    };

    // N3: the ids that link clips to their aliases, numbered in document order.
    void renumberLinks (juce::XmlElement& element, juce::StringArray& linkIDs)
    {
        if (const auto linkID = element.getStringAttribute ("linkID"); linkID.isNotEmpty())
        {
            linkIDs.addIfNotAlreadyThere (linkID);
            element.setAttribute ("linkID", "link-" + juce::String (linkIDs.indexOf (linkID) + 1));
        }

        for (auto* child : element.getChildIterator())
            renumberLinks (*child, linkIDs);
    }

    //==============================================================================
    juce::String getTag (const juce::XmlElement& element)
    {
        return element.isTextElement() ? juce::String ("#text") : element.getTagName();
    }

    // A child as findDifferences() matches it: by TAG#id, or else TAG[n] among its siblings of that tag without an id.
    struct KeyedChild
    {
        juce::String key, tag;
        int index = -1; // n, for a child without an id
        const juce::XmlElement* element = nullptr;
    };

    std::vector<KeyedChild> getKeyedChildren (const juce::XmlElement& parent)
    {
        std::vector<KeyedChild> children;
        std::map<std::string, int> numWithoutID, numWithKey;

        for (const auto* child : parent.getChildIterator())
        {
            KeyedChild keyed;
            keyed.tag = getTag (*child);
            keyed.element = child;

            if (const auto id = child->getStringAttribute ("id"); id.isNotEmpty())
            {
                keyed.key = keyed.tag + "#" + id;
            }
            else
            {
                keyed.index = numWithoutID[keyed.tag.toStdString()]++;
                keyed.key = keyed.tag + "[" + juce::String (keyed.index) + "]";
            }

            // Siblings with the same id (which Tracktion doesn't write) are told apart by their order.
            if (const auto earlier = numWithKey[keyed.key.toStdString()]++; earlier > 0)
                keyed.key << "[" << earlier << "]";

            children.push_back (keyed);
        }

        return children;
    }

    const KeyedChild* findChild (const std::vector<KeyedChild>& children, const juce::String& key)
    {
        for (const auto& child : children)
            if (child.key == key)
                return &child;

        return nullptr;
    }

    int countWithoutID (const std::vector<KeyedChild>& children, const juce::String& tag)
    {
        return (int) std::count_if (children.begin(), children.end(),
                                    [&tag] (const KeyedChild& child) { return child.index >= 0 && child.tag == tag; });
    }

    // How a path shows a child: TAG#id, TAG[n], or just TAG if neither project has another of that tag without an id.
    juce::String getPathName (const KeyedChild& child, const std::vector<KeyedChild>& expected, const std::vector<KeyedChild>& actual)
    {
        if (child.index >= 0 && countWithoutID (expected, child.tag) <= 1 && countWithoutID (actual, child.tag) <= 1)
            return child.tag;

        return child.key;
    }

    juce::String quoted (const juce::String& value)
    {
        return "\"" + value + "\"";
    }

    // The same text, or decimal numbers (N5) that differ by at most a millionth of the larger.
    bool areSameValues (const juce::String& attribute, const juce::String& expected, const juce::String& actual)
    {
        if (expected == actual)
            return true;

        if (isNeverRounded (attribute))
            return false;

        const auto a = readDecimalNumber (expected), b = readDecimalNumber (actual);
        return a.has_value() && b.has_value() && std::abs (*a - *b) <= 1.0e-6 * std::max (std::abs (*a), std::abs (*b)) + 1.0e-12;
    }

    // From the first child whose place differs: a few of the children both have, in each one's order.
    juce::String describeOrderChange (const juce::StringArray& expected, const juce::StringArray& actual)
    {
        int first = 0;

        while (first < expected.size() && expected[first] == actual[first])
            ++first;

        const auto excerpt = [first] (const juce::StringArray& keys)
        {
            constexpr int numShown = 4;
            juce::StringArray shown;

            for (int i = first; i < std::min (keys.size(), first + numShown); ++i)
                shown.add (keys[i]);

            return shown.joinIntoString (", ") + (keys.size() > first + numShown ? ", ..." : "");
        };

        return "expected " + excerpt (expected) + "; got " + excerpt (actual);
    }

    void compareElements (const juce::XmlElement& expected, const juce::XmlElement& actual, const juce::String& path,
                          const XmlComparison& comparison, juce::StringArray& differences)
    {
        if (differences.size() > comparison.maxDifferences)
            return;

        if (getTag (expected) != getTag (actual))
        {
            differences.add (path + ": expected <" + getTag (expected) + ">, got <" + getTag (actual) + ">");
            return;
        }

        for (const auto& attribute : expected.getAttributeIterator())
        {
            const auto name = attribute.name.toString();

            if (! actual.hasAttribute (name))
                differences.add (path + "@" + name + ": missing (expected " + quoted (attribute.value) + ")");
            else if (const auto value = actual.getStringAttribute (name); ! areSameValues (name, attribute.value, value))
                differences.add (path + "@" + name + ": expected " + quoted (attribute.value) + ", got " + quoted (value));
        }

        if (! comparison.actualMayAdd)
            for (const auto& attribute : actual.getAttributeIterator())
                if (! expected.hasAttribute (attribute.name.toString()))
                    differences.add (path + "@" + attribute.name.toString() + ": unexpected " + quoted (attribute.value));

        const auto expectedChildren = getKeyedChildren (expected), actualChildren = getKeyedChildren (actual);
        juce::StringArray expectedOrder, actualOrder; // the children both have, in each one's order

        for (const auto& child : expectedChildren)
        {
            if (findChild (actualChildren, child.key) != nullptr)
                expectedOrder.add (child.key);
            else
                differences.add (path + "/" + getPathName (child, expectedChildren, actualChildren) + ": missing");
        }

        for (const auto& child : actualChildren)
        {
            if (findChild (expectedChildren, child.key) != nullptr)
                actualOrder.add (child.key);
            else if (! comparison.actualMayAdd)
                differences.add (path + "/" + getPathName (child, expectedChildren, actualChildren) + ": unexpected");
        }

        if (expectedOrder != actualOrder)
            differences.add (path + ": children in another order: " + describeOrderChange (expectedOrder, actualOrder));

        for (const auto& child : expectedChildren)
            if (const auto* match = findChild (actualChildren, child.key))
                compareElements (*child.element, *match->element, path + "/" + getPathName (child, expectedChildren, actualChildren),
                                 comparison, differences);
    }

    //==============================================================================
    // findValuesNaming(): adds the values of an element, and of every element below it, that hold one of the names as
    // a whole word.
    void addValuesNaming (const juce::XmlElement& element, const juce::String& path, const juce::StringArray& names,
                          juce::StringArray& found)
    {
        const auto holdsName = [&names] (const juce::String& value)
        {
            for (const auto& name : names)
                if (value.containsWholeWordIgnoreCase (name))
                    return true;

            return false;
        };

        if (element.isTextElement())
        {
            if (holdsName (element.getText()))
                found.add (path + ": " + quoted (element.getText()));

            return;
        }

        for (const auto& attribute : element.getAttributeIterator())
            if (holdsName (attribute.value))
                found.add (path + "@" + attribute.name.toString() + ": " + quoted (attribute.value));

        const auto children = getKeyedChildren (element);

        for (const auto& child : children)
            addValuesNaming (*child.element, path + "/" + getPathName (child, children, children), names, found);
    }

    // checkNoUserName(): the names of the person running the tests, each once (JUCE gives the login name as the full
    // name too, except on macOS), trimmed, of 3 or more characters.
    juce::StringArray getUserNames()
    {
        juce::StringArray names;

        for (const auto& name : { juce::SystemStats::getFullUserName().trim(), juce::SystemStats::getLogonName().trim() })
            if (name.length() >= 3)
                names.addIfNotAlreadyThere (name, true);

        return names;
    }

    juce::String describeCount (const juce::StringArray& differences)
    {
        if (differences.size() > 0 && differences[differences.size() - 1] == moreDifferences)
            return "more than " + juce::String (differences.size() - 1) + " differences";

        return differences.size() == 1 ? juce::String ("1 difference") : juce::String (differences.size()) + " differences";
    }

    // checkProjectGolden() and checkGoldenText(): madeBy says what the golden file holds ("what Phazite saved").
    void checkGolden (const juce::String& relativePath, const juce::String& actualText, const juce::String& madeBy,
                      const std::function<juce::StringArray (const juce::String&, const juce::String&)>& compareTexts)
    {
        const auto shownPath = "tests/golden/" + relativePath;
        const auto goldenFile = getGoldenFolder().getChildFile (relativePath);
        const auto actualFile = getActualFolder().getChildFile (relativePath);
        const auto wroteActual =
            actualFile.getParentDirectory().createDirectory().wasOk() && actualFile.replaceWithText (actualText, false, false, "\n");

        const auto exists = goldenFile.existsAsFile();
        const auto differences =
            exists ? compareTexts (goldenFile.loadFileAsString().removeCharacters ("\r"), actualText) : juce::StringArray();

        if ((! exists || ! differences.isEmpty()) && isUpdatingGoldenFiles())
        {
            REQUIRE (goldenFile.getParentDirectory().createDirectory().wasOk());
            REQUIRE (goldenFile.replaceWithText (actualText, false, false, "\n"));
            MESSAGE (("Updated " + shownPath).toStdString());
            return;
        }

        juce::String report;

        if (exists)
        {
            report << "Golden file " << shownPath << " doesn't match " << madeBy << " (" << describeCount (differences) << "):\n";

            for (const auto& line : differences)
                report << "  " << line << "\n";
        }
        else
        {
            report << "Golden file " << shownPath << " is missing; run the golden tests with PHAZITE_UPDATE_GOLDEN=1.\n";
        }

        if (wroteActual)
        {
            report << "Phazite's output: " << actualFile.getFullPathName() << "\n";

            if (exists)
                report << "  (git diff --no-index " << shownPath << " \"" << actualFile.getFullPathName() << "\")\n";
        }
        else
        {
            report << "Phazite's output couldn't be written to " << actualFile.getFullPathName() << "\n";
        }

        report << "If the change is intended: run the golden tests with PHAZITE_UPDATE_GOLDEN=1 and review git diff tests/golden.";

        const auto matches = exists && differences.isEmpty();
        INFO (report.toStdString());
        CHECK (matches);
    }
} // namespace

//==============================================================================
juce::File getGoldenFolder()
{
    const auto executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    auto repository = executable.getParentDirectory();

    while (! repository.getChildFile ("tests").getChildFile ("GoldenFiles.cpp").existsAsFile() && ! repository.isRoot())
        repository = repository.getParentDirectory();

    const auto tests = repository.getChildFile ("tests");
    REQUIRE_MESSAGE (tests.getChildFile ("GoldenFiles.cpp").existsAsFile(),
                     ("No folder above " + executable.getFullPathName()
                      + " holds tests/GoldenFiles.cpp: the golden tests run from a build inside the source tree.")
                         .toStdString());
    return tests.getChildFile ("golden");
}

bool isUpdatingGoldenFiles()
{
    if (juce::SystemStats::getEnvironmentVariable ("PHAZITE_UPDATE_GOLDEN", {}) != "1")
        return false;

    if (juce::SystemStats::getEnvironmentVariable ("CI", {}).isNotEmpty())
        FAIL ("Golden files are never updated in CI: PHAZITE_UPDATE_GOLDEN=1 is for a developer's machine.");

    return true;
}

juce::File getActualFolder()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("PhaziteGoldenActual");
}

void settle (int milliseconds)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) milliseconds;

    for (;;)
    {
        dispatchPendingMessages();

        if (juce::Time::getMillisecondCounter() >= end)
            return;

        juce::Thread::sleep (5);
    }
}

std::unique_ptr<juce::XmlElement> readXml (const juce::File& file)
{
    auto xml = juce::parseXML (file);
    REQUIRE_MESSAGE (xml != nullptr, (file.getFullPathName() + " isn't an XML document.").toStdString());
    return xml;
}

//==============================================================================
const juce::StringArray& getDroppableEmptyTags()
{
    static const juce::StringArray tags {
        "INPUTDEVICES", // every Edit has one (Edit::initialiseAudioDevices); its children, the machine's inputs, N2 removes
    };

    return tags;
}

std::unique_ptr<juce::XmlElement> normaliseProject (const juce::XmlElement& saved, const juce::File& projectFolder,
                                                    const juce::StringArray& droppableEmptyTags)
{
    juce::StringArray folderPaths;

    if (const auto folder = projectFolder.getFullPathName(); folder.isNotEmpty())
    {
        folderPaths.add (folder.replaceCharacter ('/', '\\'));
        folderPaths.addIfNotAlreadyThere (folder.replaceCharacter ('\\', '/'));
    }

    const Normaliser normaliser { folderPaths, droppableEmptyTags };
    auto normalised = normaliser.normalise (saved, true);

    juce::StringArray linkIDs;
    renumberLinks (*normalised, linkIDs);
    return normalised;
}

std::unique_ptr<juce::XmlElement> readNormalisedProject (const juce::File& projectFile)
{
    return normaliseProject (*readXml (projectFile), projectFile.getParentDirectory());
}

//==============================================================================
juce::StringArray findDifferences (const juce::XmlElement& expected, const juce::XmlElement& actual, XmlComparison comparison)
{
    comparison.maxDifferences = std::max (1, comparison.maxDifferences);

    juce::StringArray differences;
    compareElements (expected, actual, getTag (expected), comparison, differences);
    limitDifferences (differences, comparison.maxDifferences);
    return differences;
}

void limitDifferences (juce::StringArray& differences, int maxDifferences)
{
    maxDifferences = std::max (1, maxDifferences);

    if (differences.size() > maxDifferences)
    {
        differences.removeRange (maxDifferences - 1, differences.size());
        differences.add (moreDifferences);
    }
}

juce::StringArray compareLines (const juce::String& expected, const juce::String& actual, int maxDifferences)
{
    const auto expectedLines = juce::StringArray::fromLines (expected);
    const auto actualLines = juce::StringArray::fromLines (actual);
    juce::StringArray differences;

    for (int i = 0; i < std::max (expectedLines.size(), actualLines.size()); ++i)
    {
        const auto where = "line " + juce::String (i + 1) + ": ";

        if (i >= actualLines.size())
            differences.add (where + "missing (expected " + quoted (expectedLines[i]) + ")");
        else if (i >= expectedLines.size())
            differences.add (where + "unexpected " + quoted (actualLines[i]));
        else if (expectedLines[i] != actualLines[i])
            differences.add (where + "expected " + quoted (expectedLines[i]) + ", got " + quoted (actualLines[i]));
    }

    limitDifferences (differences, maxDifferences);
    return differences;
}

juce::String toGoldenText (const juce::XmlElement& normalised, const juce::String& about)
{
    jassert (! about.contains ("--")); // which XML comments can't hold

    juce::XmlElement::TextFormat format;
    format.addDefaultHeader = false;
    format.lineWrapLength = 0; // every attribute on a line of its own, so that a diff shows which one changed
    format.newLineChars = "\n";

    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- " + about + ". Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md). -->\n"
           + normalised.toString (format);
}

//==============================================================================
void checkSameProject (const juce::String& what, const juce::XmlElement& expected, const juce::XmlElement& actual, XmlComparison comparison)
{
    const auto differences = findDifferences (expected, actual, comparison);

    juce::String report;
    report << "The project " << what << " doesn't match the project it came from (" << describeCount (differences) << "):";

    for (const auto& line : differences)
        report << "\n  " << line;

    INFO (report.toStdString());
    CHECK (differences.isEmpty());
}

void checkProjectGolden (const juce::String& relativePath, const juce::XmlElement& normalised, const juce::String& about)
{
    checkGolden (relativePath, toGoldenText (normalised, about), "what Phazite saved",
                 [&normalised] (const juce::String& expectedText, const juce::String&)
                 {
                     if (const auto expected = juce::parseXML (expectedText))
                         return findDifferences (*expected, normalised);

                     return juce::StringArray { "the golden file isn't an XML document" };
                 });
}

void checkGoldenText (const juce::String& relativePath, const juce::String& actualText,
                      const std::function<juce::StringArray (const juce::String& expected, const juce::String& actual)>& compareTexts)
{
    checkGolden (relativePath, actualText, "Phazite's output", compareTexts);
}

//==============================================================================
juce::StringArray findValuesNaming (const juce::XmlElement& xml, const juce::StringArray& names)
{
    juce::StringArray found;
    addValuesNaming (xml, getTag (xml), names, found);
    return found;
}

void checkNoUserName (const juce::String& what, const juce::XmlElement& save)
{
    const auto found = findValuesNaming (save, getUserNames());

    juce::String report;
    report << what << " names the person running the tests in " << found.size() << (found.size() == 1 ? " value:" : " values:");

    for (const auto& line : found)
        report << "\n  " << line;

    INFO (report.toStdString());
    CHECK (found.isEmpty());
}

//==============================================================================
te::Plugin::Ptr createBuiltInDevice (te::Edit& edit, const char* xmlTypeName)
{
    te::Plugin::Ptr device;

    for (const auto& type : phazite::getDeviceTypes (edit.engine))
        if (device == nullptr && type.description.fileOrIdentifier == xmlTypeName)
            device = phazite::createDevice (edit, type);

    REQUIRE (device != nullptr);
    return device;
}
} // namespace phazite::test
