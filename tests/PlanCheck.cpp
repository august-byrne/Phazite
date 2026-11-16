// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PlanCheck.h"

#include <string>

namespace phazite::test
{
namespace
{
    struct Differences
    {
        juce::StringArray lines;

        void add (const juce::String& where, const juce::String& what)
        {
            if (lines.size() < 40)
                lines.add (where + ": " + what);
        }
    };

    bool same (const engine::Curve* a, const engine::Curve* b)
    {
        if (a == nullptr || b == nullptr)
            return a == b;

        const auto& x = a->getPoints();
        const auto& y = b->getPoints();
        return std::equal (x.begin(), x.end(), y.begin(), y.end(),
                           [] (const engine::CurvePoint& p, const engine::CurvePoint& q)
                           {
                               return juce::exactlyEqual (p.beat, q.beat) && juce::exactlyEqual (p.value, q.value)
                                      && juce::exactlyEqual (p.curve, q.curve);
                           });
    }

    bool same (const engine::NoteSequence* a, const engine::NoteSequence* b)
    {
        if (a == nullptr || b == nullptr)
            return a == b;

        return std::equal (a->getEvents().begin(), a->getEvents().end(), b->getEvents().begin(), b->getEvents().end(),
                           [] (const engine::NoteSequence::Event& x, const engine::NoteSequence::Event& y)
                           {
                               return juce::exactlyEqual (x.beat, y.beat) && x.type == y.type && x.number == y.number
                                      && juce::exactlyEqual (x.value, y.value);
                           });
    }

    int lengthOf (const std::shared_ptr<engine::Delay>& delay)
    {
        return delay != nullptr ? delay->getLength() : 0;
    }

    void compareAudio (Differences& found, const juce::String& where, const engine::AudioRegion& a, const engine::AudioRegion& b)
    {
        const auto fileOf = [] (const engine::AudioRegion& region)
        {
            // By what it was opened as: its path, and when it was written and how long it was then.
            return region.file != nullptr ? region.file->getOrigin() : juce::String ("none");
        };

        if (fileOf (a) != fileOf (b))
            found.add (where, "its file: " + fileOf (a) + ", not " + fileOf (b));

        if (a.start != b.start || a.length != b.length || ! juce::exactlyEqual (a.fileStart, b.fileStart))
            found.add (where, "its place: " + juce::String (a.start) + "+" + juce::String (a.length) + " from " + juce::String (a.fileStart)
                                  + ", not " + juce::String (b.start) + "+" + juce::String (b.length) + " from "
                                  + juce::String (b.fileStart));

        if (! juce::exactlyEqual (a.gain, b.gain) || a.fadeIn != b.fadeIn || a.fadeOut != b.fadeOut
            || a.fadeInCurve.shape != b.fadeInCurve.shape || ! juce::exactlyEqual (a.fadeInCurve.bend, b.fadeInCurve.bend)
            || a.fadeOutCurve.shape != b.fadeOutCurve.shape || ! juce::exactlyEqual (a.fadeOutCurve.bend, b.fadeOutCurve.bend))
            found.add (where, "its gain or fades");
    }

    void compareDevice (Differences& found, const juce::String& where, const engine::Plan::Device& a, const engine::Plan::Device& b)
    {
        const auto& infoA = a.node->getDeviceInfo();
        const auto& infoB = b.node->getDeviceInfo();

        if (infoA.typeID != infoB.typeID)
        {
            found.add (where, "another device: " + juce::String (std::string (infoA.typeID)) + ", not "
                                  + juce::String (std::string (infoB.typeID)));
            return;
        }

        if (a.node->isOn() != b.node->isOn())
            found.add (where, a.node->isOn() ? "on, not off" : "off, not on");

        for (int i = 0; i < (int) infoA.parameters.size(); ++i)
            if (! juce::exactlyEqual (a.node->getValue (i), b.node->getValue (i)))
                found.add (where, juce::String (std::string (infoA.parameters[(size_t) i].id)) + " is "
                                      + juce::String (a.node->getValue (i)) + ", not " + juce::String (b.node->getValue (i)));

        if (a.latency != b.latency || a.node->getRunner().getLatencySamples() != b.node->getRunner().getLatencySamples())
            found.add (where, "its latency");

        if (a.automation.size() != b.automation.size())
        {
            found.add (where,
                       juce::String ((int) a.automation.size()) + " automated parameters, not " + juce::String ((int) b.automation.size()));
        }
        else
        {
            for (size_t i = 0; i < a.automation.size(); ++i)
                if (a.automation[i].parameter != b.automation[i].parameter
                    || ! same (a.automation[i].curve.get(), b.automation[i].curve.get()))
                    found.add (where, "the automation of parameter " + juce::String (a.automation[i].parameter));
        }

        if (a.sidechain != b.sidechain || lengthOf (a.sidechainDelay) != lengthOf (b.sidechainDelay))
            found.add (where, "its sidechain");

        if ((a.modulation == nullptr) != (b.modulation == nullptr)
            || (a.modulation != nullptr && a.modulation->routes.size() != b.modulation->routes.size()))
            found.add (where, "its modulation");
    }

    void compareStrip (Differences& found, const engine::Plan::Strip& a, const engine::Plan::Strip& b)
    {
        const auto where = "strip " + juce::String (a.id);

        if (! same (a.notes.get(), b.notes.get()))
            found.add (where, "its notes");

        if (a.audio.size() != b.audio.size())
            found.add (where, juce::String ((int) a.audio.size()) + " audio regions, not " + juce::String ((int) b.audio.size()));
        else
            for (size_t i = 0; i < a.audio.size(); ++i)
                compareAudio (found, where + ", audio region " + juce::String ((int) i), a.audio[i], b.audio[i]);

        if (a.devices.size() != b.devices.size())
            found.add (where, juce::String ((int) a.devices.size()) + " devices, not " + juce::String ((int) b.devices.size()));
        else
            for (size_t i = 0; i < a.devices.size(); ++i)
                compareDevice (found, where + ", device " + juce::String ((int) i), a.devices[i], b.devices[i]);

        if (a.inputs.size() != b.inputs.size())
        {
            found.add (where, juce::String ((int) a.inputs.size()) + " inputs, not " + juce::String ((int) b.inputs.size()));
        }
        else
        {
            for (size_t i = 0; i < a.inputs.size(); ++i)
            {
                const auto& x = a.inputs[i];
                const auto& y = b.inputs[i];

                if (x.strip != y.strip || x.beforeFader != y.beforeFader || x.beforeMute != y.beforeMute
                    || lengthOf (x.delay) != lengthOf (y.delay) || (x.level == nullptr) != (y.level == nullptr)
                    || (x.level != nullptr
                        && (! juce::exactlyEqual (x.level->getGain(), y.level->getGain()) || x.level->isCut() != y.level->isCut()))
                    || (x.part == nullptr) != (y.part == nullptr))
                    found.add (where, "input " + juce::String ((int) i));
            }
        }

        if (a.hasInstrument != b.hasInstrument || a.latency != b.latency || a.inputChannels != b.inputChannels)
            found.add (where, "its instrument, latency or inputs heard");

        if (! same (a.volume.get(), b.volume.get()) || ! same (a.pan.get(), b.pan.get()))
            found.add (where, "its fader's automation");

        if (a.containers.size() != b.containers.size() || a.modulators.size() != b.modulators.size())
            found.add (where, "its containers or modulators");
    }
} // namespace

juce::StringArray describePlanDifferences (const engine::Plan& kept, const engine::Plan& fresh)
{
    Differences found;

    if (kept.strips.size() != fresh.strips.size())
    {
        found.add ("the plan", juce::String ((int) kept.strips.size()) + " strips, not " + juce::String ((int) fresh.strips.size()));
        return found.lines;
    }

    for (size_t i = 0; i < kept.strips.size(); ++i)
    {
        if (kept.strips[i].id != fresh.strips[i].id)
            found.add ("strip " + juce::String ((int) i),
                       "the strip of " + juce::String (kept.strips[i].id) + ", not " + juce::String (fresh.strips[i].id));
        else
            compareStrip (found, kept.strips[i], fresh.strips[i]);
    }

    if (kept.metronome != fresh.metronome || kept.loop.has_value() != fresh.loop.has_value()
        || (kept.loop.has_value()
            && (! juce::exactlyEqual (kept.loop->start, fresh.loop->start) || ! juce::exactlyEqual (kept.loop->end, fresh.loop->end))))
        found.add ("the plan", "its loop or metronome");

    // The tempo map, by where each of the first 256 beats falls.
    for (auto beat = 0.0; beat < 256.0; beat += 1.0)
    {
        if (! juce::exactlyEqual (kept.tempo.toSamples (beat), fresh.tempo.toSamples (beat)))
        {
            found.add ("the plan", "its tempo map at beat " + juce::String (beat));
            break;
        }
    }

    return found.lines;
}
} // namespace phazite::test
