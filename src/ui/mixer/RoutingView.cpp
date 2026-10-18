// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RoutingView.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    const juce::String hint ("Every channel, with the wires of the selected one. Drag from a channel's Out onto a bus or the master to "
                             "route it there, onto a return to send to it, or onto a sidechain jack (SC) to make that device "
                             "listen to it. Right-click a channel to take a wire away.");

    juce::String getKindName (ChannelKind kind)
    {
        switch (kind)
        {
            case ChannelKind::bus:
                return "Bus";
            case ChannelKind::plainFolder:
                return "Folder";
            case ChannelKind::returnTrack:
                return "Return";
            case ChannelKind::track:
            default:
                return "Track";
        }
    }

    // A jack's place along a box's bottom: the first, second, and so on.
    juce::Point<int> getJack (const juce::Rectangle<int>& box, int index)
    {
        return { box.getX() + RoutingTint::jackRadius + Space::s + index * RoutingTint::jackSpacing,
                 box.getBottom() - RoutingTint::jackRadius - Space::xs };
    }

    bool isNear (juce::Point<int> a, juce::Point<int> b)
    {
        return a.getDistanceFrom (b) <= RoutingTint::grabDistance;
    }
} // namespace

RoutingView::RoutingView (ProjectContext& c)
    : context (c)
{
    setTitle ("Routing");
    setDescription (hint);
    context.getSongSelection().addChangeListener (this);
    context.addListener (this);
    rebuild();
}

RoutingView::~RoutingView()
{
    context.removeListener (this);
    context.getSongSelection().removeChangeListener (this);
}

void RoutingView::songChanged (const SongChanges& changes)
{
    if (changes.tracks || changes.devices)
        rebuild();
}

//==============================================================================
void RoutingView::rebuild()
{
    boxes.clear();
    connections.clear();
    hovered.reset();
    dragFrom.reset();

    if (! context.hasProject())
    {
        repaint();
        return;
    }

    const auto channels = getMixerChannels (context);
    auto ids = channels.tracks;
    ids.insert (ids.end(), channels.returns.begin(), channels.returns.end());

    for (const auto id : ids)
    {
        const auto channel = getChannel (context, id);

        if (! channel.has_value())
            continue;

        Box box { id, channel->name, getKindName (channel->kind), channel->kind, channel->colour, false, {}, {} };
        box.takesInput = channel->kind == ChannelKind::bus || channel->kind == ChannelKind::returnTrack;

        for (const auto& effect : getEffects (context, id))
            if (hasSidechain (context, effect.id))
                box.sidechains.push_back ({ effect.id, effect.name });

        boxes.push_back (box);
    }

    boxes.push_back ({ {}, "Master", "What you hear", ChannelKind::bus, getPalette (*this).accent, true, {}, {} });
    connections = getConnections (context);
    layOutBoxes();

    if (getWidth() < getPreferredWidth()) // the mixer's viewport scrolls to every box
        setSize (getPreferredWidth(), getHeight());

    repaint();
}

int RoutingView::getPreferredWidth() const
{
    return (int) boxes.size() * (RoutingTint::boxWidth + RoutingTint::gap) + RoutingTint::gap;
}

void RoutingView::resized()
{
    layOutBoxes();
}

void RoutingView::layOutBoxes()
{
    auto x = RoutingTint::gap;

    for (auto& box : boxes)
    {
        box.area = { x, RoutingTint::top, RoutingTint::boxWidth, RoutingTint::boxHeight };
        x += RoutingTint::boxWidth + RoutingTint::gap;
    }
}

const RoutingView::Box* RoutingView::findBox (TrackID id) const
{
    for (const auto& box : boxes)
        if (box.id == id)
            return &box;

    return nullptr;
}

const RoutingView::Box* RoutingView::getBoxAt (juce::Point<int> position) const
{
    for (const auto& box : boxes)
        if (box.area.contains (position))
            return &box;

    return nullptr;
}

//==============================================================================
juce::Rectangle<int> RoutingView::getBoxArea (TrackID id) const
{
    const auto* box = findBox (id);
    return box != nullptr ? box->area : juce::Rectangle<int>();
}

std::optional<juce::Point<int>> RoutingView::getOutJack (TrackID id) const
{
    // The master's sound leaves Phazite; a plain folder has none of its own.
    const auto* box = findBox (id);

    if (box == nullptr || ! id.isValid() || box->kind == ChannelKind::plainFolder)
        return std::nullopt;

    return getJack (box->area, 0);
}

std::optional<juce::Point<int>> RoutingView::getInJack (TrackID id) const
{
    const auto* box = findBox (id);

    if (box == nullptr || ! box->takesInput)
        return std::nullopt;

    return getJack (box->area, id.isValid() ? 1 : 0);
}

std::optional<juce::Point<int>> RoutingView::getSidechainJack (DeviceID device) const
{
    for (const auto& box : boxes)
        for (size_t i = 0; i < box.sidechains.size(); ++i)
            if (box.sidechains[i].device == device)
                return getJack (box.area, (box.takesInput ? 2 : 1) + (int) i);

    return std::nullopt;
}

std::optional<DeviceID> RoutingView::getSidechainJackAt (juce::Point<int> position) const
{
    for (const auto& box : boxes)
        for (const auto& input : box.sidechains)
            if (const auto jack = getSidechainJack (input.device); jack.has_value() && isNear (*jack, position))
                return input.device;

    return std::nullopt;
}

bool RoutingView::isNearOutJack (const Box& box, juce::Point<int> position) const
{
    const auto jack = getOutJack (box.id);
    return jack.has_value() && isNear (*jack, position);
}

juce::Point<int> RoutingView::getFromPoint (const Connection& connection) const
{
    return getOutJack (connection.from).value_or (juce::Point<int>());
}

std::optional<juce::Point<int>> RoutingView::getToPoint (const Connection& connection) const
{
    if (connection.kind == Connection::Kind::sidechain)
        return getSidechainJack (connection.device);

    return getInJack (connection.to);
}

std::vector<Connection> RoutingView::getShownConnections() const
{
    // The wires of the selected channels and the one under the pointer, at either end.
    const auto selected = context.getSongSelection().getTracks();
    const auto isShown = [this, &selected] (TrackID id)
    { return (hovered.has_value() && *hovered == id) || std::find (selected.begin(), selected.end(), id) != selected.end(); };

    std::vector<Connection> shown;

    for (const auto& connection : connections)
        if (isShown (connection.from) || isShown (connection.to))
            shown.push_back (connection);

    return shown;
}

//==============================================================================
void RoutingView::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.background);

    if (! context.hasProject())
        return;

    for (const auto& box : boxes)
        paintBox (g, box);

    // The wires, under the boxes: where the sound goes in the text color, sends in the accent color, and sidechains in the
    // modulation color.
    for (const auto& connection : getShownConnections())
    {
        const auto to = getToPoint (connection);

        if (! to.has_value())
            continue;

        const auto colour = connection.kind == Connection::Kind::output ? p.text
                            : connection.kind == Connection::Kind::send ? p.accent
                                                                        : p.modulation;
        paintCable (g, getFromPoint (connection), *to, colour);
    }

    // A cable being dragged from an Out jack.
    if (dragFrom.has_value())
        if (const auto from = getOutJack (*dragFrom))
            paintCable (g, *from, dragEnd, p.accent.withAlpha (0.7f));
}

void RoutingView::paintBox (juce::Graphics& g, const Box& box)
{
    const auto& p = getPalette (*this);
    const auto selected = box.id.isValid() && context.getSongSelection().isSelected (box.id);
    const auto dropping = dropTarget.has_value() && *dropTarget == box.id && ! dropDevice.has_value();
    auto area = box.area;

    g.setColour (selected ? p.surfaceRaised.interpolatedWith (p.accent, 0.15f) : p.surfaceRaised);
    g.fillRoundedRectangle (area.toFloat(), cornerRadius);

    if (dropping)
    {
        g.setColour (p.accent.withAlpha (RoutingTint::dropWash));
        g.fillRoundedRectangle (area.toFloat(), cornerRadius);
    }

    g.setColour (box.colour);
    g.fillRect (area.removeFromTop (4));

    if (selected || dropping)
    {
        g.setColour (p.accent);
        g.drawRoundedRectangle (box.area.toFloat().reduced (0.5f), cornerRadius, 1.0f);
    }

    // The name and what the channel is.
    auto text = area.reduced (Space::s, Space::xs);
    g.setColour (p.text);
    g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
    g.drawText (box.name, text.removeFromTop (16), juce::Justification::centredLeft, true);
    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (box.kindName, text.removeFromTop (16), juce::Justification::centredLeft, true);

    // The jacks along its bottom, each with its name above it.
    const auto paintJack = [&] (juce::Point<int> jack, const juce::String& label, bool highlighted)
    {
        const auto radius = (float) RoutingTint::jackRadius;
        g.setColour (highlighted ? p.accent : p.textMuted);
        g.fillEllipse (jack.toFloat().x - radius, jack.toFloat().y - radius, radius * 2.0f, radius * 2.0f);
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (label,
                    juce::Rectangle<int> (jack.x - RoutingTint::jackSpacing / 2, jack.y - RoutingTint::jackRadius - 16,
                                          RoutingTint::jackSpacing, 14),
                    juce::Justification::centred, false);
    };

    if (const auto out = getOutJack (box.id))
        paintJack (*out, "Out", false);

    if (const auto in = getInJack (box.id))
        paintJack (*in, "In", dropping);

    for (const auto& input : box.sidechains)
        if (const auto jack = getSidechainJack (input.device))
            paintJack (*jack, "SC", dropDevice.has_value() && *dropDevice == input.device);
}

void RoutingView::paintCable (juce::Graphics& g, juce::Point<int> from, juce::Point<int> to, juce::Colour colour)
{
    // A curve that dips under the boxes, deeper the farther it goes.
    const auto dip = (float) juce::jlimit (RoutingTint::shallowestDip, RoutingTint::deepestDip,
                                           RoutingTint::shallowestDip + std::abs (to.x - from.x) / 4);
    juce::Path cable;
    cable.startNewSubPath (from.toFloat());
    cable.cubicTo ((float) from.x, (float) from.y + dip, (float) to.x, (float) to.y + dip, (float) to.x, (float) to.y);
    g.setColour (colour);
    g.strokePath (cable, juce::PathStrokeType (RoutingTint::cableWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//==============================================================================
void RoutingView::mouseMove (const juce::MouseEvent& e)
{
    const auto* box = getBoxAt (e.getPosition());
    const auto now = box != nullptr ? std::optional<TrackID> (box->id) : std::nullopt;

    if (now != hovered)
    {
        hovered = now;
        repaint();
    }

    const auto onJack = box != nullptr && isNearOutJack (*box, e.getPosition());
    setMouseCursor (onJack ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    setDescription (onJack ? "Drag to a bus or the master to route " + box->name
                                 + " there, to a return to send to it, or to a "
                                   "sidechain jack (SC) to make that device listen to it"
                           : hint);
}

void RoutingView::mouseExit (const juce::MouseEvent&)
{
    hovered.reset();
    setDescription (hint);
    repaint();
}

void RoutingView::mouseDown (const juce::MouseEvent& e)
{
    const auto* box = getBoxAt (e.getPosition());

    if (box == nullptr)
        return;

    if (e.mods.isPopupMenu())
    {
        getMenuFor (box->id).showMenuAsync (juce::PopupMenu::Options());
        return;
    }

    if (isNearOutJack (*box, e.getPosition()))
    {
        dragFrom = box->id;
        dragEnd = e.getPosition();
        return;
    }

    // A click selects the channel, whose wires then show.
    if (box->id.isValid())
        context.getSongSelection().selectOnly (box->id);
}

void RoutingView::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragFrom.has_value())
        return;

    dragEnd = e.getPosition();
    dropDevice = getSidechainJackAt (dragEnd);
    const auto* box = getBoxAt (dragEnd);
    dropTarget = box != nullptr && box->id != *dragFrom ? std::optional<TrackID> (box->id) : std::nullopt;
    setDescription (describeDrop());
    repaint();
}

void RoutingView::mouseUp (const juce::MouseEvent& e)
{
    if (! dragFrom.has_value())
        return;

    const auto from = *dragFrom;
    dragFrom.reset();
    dropTarget.reset();
    dropDevice.reset();
    setDescription (hint);
    repaint();
    connect (from, e.getPosition());
}

juce::String RoutingView::describeDrop() const
{
    const auto* from = dragFrom.has_value() ? findBox (*dragFrom) : nullptr;

    if (from == nullptr)
        return hint;

    if (dropDevice.has_value())
        return "Make this device listen to " + from->name;

    if (const auto* target = dropTarget.has_value() ? findBox (*dropTarget) : nullptr)
    {
        if (! target->id.isValid())
            return "Route " + from->name + " to the master";

        if (target->kind == ChannelKind::returnTrack)
            return "Send " + from->name + " to " + target->name;

        if (target->kind == ChannelKind::bus)
            return "Route " + from->name + " into " + target->name;
    }

    return "Drop on a bus, the master, a return or a sidechain jack";
}

void RoutingView::connect (TrackID from, juce::Point<int> where)
{
    // A sidechain jack: the device listens to the channel, if it can without a loop.
    if (const auto device = getSidechainJackAt (where))
    {
        const auto choices = getSidechainChoices (context, *device);

        if (std::any_of (choices.begin(), choices.end(), [from] (const DeviceHandle::SidechainChoice& c) { return c.track == from; }))
            setSidechainSource (context, *device, from);

        return;
    }

    const auto* target = getBoxAt (where);

    if (target == nullptr || target->id == from)
        return;

    // The master or a bus: where the channel's sound goes. A return: a send to it, unless there is one.
    if (! target->id.isValid() || target->kind == ChannelKind::bus)
    {
        const auto choices = getOutputChoices (context, from);

        if (std::any_of (choices.begin(), choices.end(), [target] (const OutputChoice& c) { return c.bus == target->id; }))
            setOutput (context, from, target->id);

        return;
    }

    if (target->kind == ChannelKind::returnTrack)
        for (const auto& sendTarget : getSendTargets (context, from))
            if (sendTarget.returnTrack == target->id && ! sendTarget.send.has_value())
                addSendTo (context, from, target->id);
}

juce::PopupMenu RoutingView::getMenuFor (TrackID id)
{
    juce::PopupMenu menu;
    auto& projectContext = context;
    const auto nameOf = [this] (TrackID track)
    {
        const auto* box = findBox (track);
        return box != nullptr ? box->name : juce::String();
    };

    menu.addSectionHeader ("Take a wire away");

    for (const auto& connection : connections)
    {
        if (connection.from != id && connection.to != id)
            continue;

        const auto from = connection.from;
        const auto device = connection.device;

        switch (connection.kind)
        {
            case Connection::Kind::output:
                if (connection.to.isValid()) // into a bus: back to the master
                    menu.addItem ("Route " + nameOf (from) + " to the Master, Out of " + nameOf (connection.to),
                                  [&projectContext, from] { setOutput (projectContext, from, {}); });
                break;

            case Connection::Kind::send:
                menu.addItem ("Remove the Send from " + nameOf (from) + " to " + nameOf (connection.to),
                              [&projectContext, device] { removeSend (projectContext, device); });
                break;

            case Connection::Kind::sidechain:
                menu.addItem ("Stop " + nameOf (connection.to) + "'s Sidechain Listening to " + nameOf (from),
                              [&projectContext, device] { setSidechainSource (projectContext, device, {}); });
                break;
        }
    }

    if (menu.getNumItems() <= 1)
        menu.addItem ("No wires to take away", false, false, [] {});

    return menu;
}
} // namespace phazite::ui
