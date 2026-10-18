// SPDX-License-Identifier: AGPL-3.0-or-later
// The routing view (docs/design.md, section 8; roadmap step 2.9), which the mixer's Routing button shows in place of its
// strips: every channel as a box, in the mixer's order with the master last, and the wires of the selected or hovered
// channel as cables under them, as Reason's back panel shows its devices': where its sound goes (a bus or the master),
// its sends to returns, and the sidechains that listen to it or that it listens to. Each box has jacks along its bottom:
// Out, In for a bus, a return or the master, and one for each device with a sidechain. Dragging from an Out jack onto a
// bus or the master routes the channel there, onto a return sends to it, and onto a sidechain jack makes the device listen
// to the channel; right-clicking a box offers to take its wires away. It reaches the routing through the model
// (model/mixer/MixerModel.h).
#pragma once

#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>
#include <vector>

namespace phazite::ui
{
class RoutingView : public juce::Component,
                    private ProjectContext::Listener,
                    private juce::ChangeListener
{
public:
    explicit RoutingView (ProjectContext&);
    ~RoutingView() override;

    // How wide it needs to be for every box, which the mixer scrolls sideways.
    int getPreferredWidth() const;

    // What a test finds: the wires drawn now (those of the selected and hovered channels), a channel's box (the master's
    // with an empty ID), and its jacks: Out, In, and a device's sidechain jack.
    std::vector<Connection> getShownConnections() const;
    juce::Rectangle<int> getBoxArea (TrackID) const;
    std::optional<juce::Point<int>> getOutJack (TrackID) const;
    std::optional<juce::Point<int>> getInJack (TrackID) const;
    std::optional<juce::Point<int>> getSidechainJack (DeviceID) const;

    // The menu a right-click on a box shows: taking away each of its wires.
    juce::PopupMenu getMenuFor (TrackID);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct SidechainInput
    {
        DeviceID device;
        juce::String name;
    };

    struct Box
    {
        TrackID id; // none for the master
        juce::String name, kindName;
        ChannelKind kind = ChannelKind::track; // the master's box: a bus, which takes in the others
        juce::Colour colour;
        bool takesInput = false; // a bus, a return or the master: what an Out jack's cable can go into
        std::vector<SidechainInput> sidechains;
        juce::Rectangle<int> area;
    };

    ProjectContext& context;
    std::vector<Box> boxes; // the tracks', the returns', then the master's
    std::vector<Connection> connections;
    std::optional<TrackID> hovered;
    std::optional<TrackID> dragFrom;    // while a cable is dragged from an Out jack
    juce::Point<int> dragEnd;           // where the dragged cable ends
    std::optional<TrackID> dropTarget;  // the box it would go into
    std::optional<DeviceID> dropDevice; // or the sidechain jack

    void rebuild(); // the boxes and wires as the project has them now
    void layOutBoxes();
    const Box* findBox (TrackID) const;
    const Box* getBoxAt (juce::Point<int>) const;
    std::optional<DeviceID> getSidechainJackAt (juce::Point<int>) const;
    bool isNearOutJack (const Box&, juce::Point<int>) const;
    juce::Point<int> getFromPoint (const Connection&) const;
    std::optional<juce::Point<int>> getToPoint (const Connection&) const;
    void paintBox (juce::Graphics&, const Box&);
    void paintCable (juce::Graphics&, juce::Point<int> from, juce::Point<int> to, juce::Colour);
    void connect (TrackID from, juce::Point<int> where); // what a cable dropped there does
    juce::String describeDrop() const;

    void projectChanged() override { rebuild(); }
    void songChanged (const SongChanges&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); } // the selection changed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RoutingView)
};
} // namespace phazite::ui
