#include "TakeView.h"
#include "LookAndFeel.h"

#include <cmath>

namespace ui
{

namespace
{
    constexpr int kRulerHeight = 18;
    constexpr int kGutterWidth = 78;
    constexpr int kOtherStripHeight = 12;
}

TakeView::TakeView (BeatboxProcessor& p)
    : processor (p)
{
}

void TakeView::setPlaceholder (const juce::String& text, bool shouldHideTake)
{
    if (text == placeholder && shouldHideTake == hideTake)
        return;

    placeholder = text;
    hideTake = shouldHideTake;
    repaint();
}

double TakeView::noteLengthBeats (double bpm) const
{
    float ms = 60.0f;

    if (auto* value = processor.apvts.getRawParameterValue (ParamIDs::noteLength))
        ms = value->load();

    return (double) ms / 1000.0 * juce::jmax (1.0, bpm) / 60.0;
}

void TakeView::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (Theme::background.brighter (0.03f));
    g.fillRoundedRectangle (bounds, 6.0f);

    const auto& take = processor.getLastTake();

    if (take.hits.empty() || hideTake)
    {
        g.setColour (Theme::outline.withAlpha (0.7f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
        drawParagraph (g, placeholder, bounds.reduced (24.0f, 10.0f), 15.0f, Theme::textDim, juce::Justification::centred);
        return;
    }

    const auto slots = processor.getSlots();
    auto area = getLocalBounds().reduced (1);
    auto ruler = area.removeFromTop (kRulerHeight);
    auto gutter = area.removeFromLeft (kGutterWidth);
    ruler.removeFromLeft (kGutterWidth);

    auto laneIndexOf = [&slots] (int slotId)
    {
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i].id == slotId)
                return (int) i;

        return -1;
    };

    // Time range: from the capture bar (or earlier hits) to the end of the bar holding the last note.
    const auto qnPerBar = juce::jmax (0.25, bbr::quarterNotesPerBar (take.timeSigNum, take.timeSigDen));
    const auto noteBeats = noteLengthBeats (take.bpm);
    double minPpq = take.hits.front().ppq, maxPpq = minPpq;
    bool anyOther = false;

    for (const auto& hit : take.hits)
    {
        minPpq = juce::jmin (minPpq, hit.ppq);
        maxPpq = juce::jmax (maxPpq, hit.ppq);
        anyOther = anyOther || laneIndexOf (hit.slotId) < 0;
    }

    double start = take.barStartPpq;
    int barsBefore = 0;

    if (minPpq < start)
    {
        barsBefore = (int) std::ceil ((start - minPpq) / qnPerBar);
        start -= barsBefore * qnPerBar;
    }

    const auto numBars = juce::jlimit (1, 10000, (int) std::ceil ((maxPpq + noteBeats - start) / qnPerBar + 1.0e-6));
    const auto end = start + numBars * qnPerBar;
    const auto firstBarNumber = processor.getTakeDropBar (false) - barsBefore;

    auto otherStrip = anyOther ? area.removeFromBottom (kOtherStripHeight) : juce::Rectangle<int>();
    const auto numLanes = juce::jmax (1, (int) slots.size());
    const auto laneHeight = (float) area.getHeight() / (float) numLanes;
    const auto pxPerQn = (double) area.getWidth() / (end - start);

    auto xOf = [&] (double ppq) { return (float) area.getX() + (float) ((ppq - start) * pxPerQn); };
    auto laneRect = [&] (int lane)
    {
        return juce::Rectangle<float> ((float) area.getX(), (float) area.getY() + laneHeight * (float) lane,
                                       (float) area.getWidth(), laneHeight);
    };

    // Lanes and names.
    for (int lane = 0; lane < numLanes; ++lane)
    {
        auto r = laneRect (lane);

        if (lane % 2 == 1)
        {
            g.setColour (Theme::panel.withAlpha (0.6f));
            g.fillRect (r);
        }

        if (lane < (int) slots.size())
        {
            const auto& slot = slots[(size_t) lane];
            auto label = juce::Rectangle<float> ((float) gutter.getX(), r.getY(), (float) gutter.getWidth(), laneHeight).reduced (6.0f, 0.0f);
            g.setColour (slotColour (slot.id));
            g.fillEllipse (label.removeFromLeft (8.0f).withSizeKeepingCentre (8.0f, 8.0f));
            label.removeFromLeft (6.0f);
            g.setColour (Theme::text);
            g.setFont (font (juce::jlimit (10.0f, 13.0f, laneHeight * 0.6f)));
            g.drawText (slot.name, label, juce::Justification::centredLeft, true);
        }
    }

    if (anyOther)
    {
        g.setColour (Theme::textFaint);
        g.setFont (font (10.0f));
        g.drawText ("ignored", juce::Rectangle<int> (gutter.getX() + 6, otherStrip.getY(), gutter.getWidth() - 8, otherStrip.getHeight()),
                    juce::Justification::centredLeft, true);
    }

    // Grid.
    const auto beatQn = 4.0 / (double) juce::jmax (1, take.timeSigDen);
    const auto beatPx = beatQn * pxPerQn;
    const auto barPx = qnPerBar * pxPerQn;
    const auto labelEvery = juce::jmax (1, (int) std::ceil (36.0 / juce::jmax (1.0, barPx)));
    const auto gridTop = (float) ruler.getY();
    const auto gridBottom = (float) (anyOther ? otherStrip.getBottom() : area.getBottom());

    g.setFont (font (11.0f));

    for (int bar = 0; bar <= numBars; ++bar)
    {
        const auto barPpq = start + bar * qnPerBar;
        const auto x = xOf (barPpq);

        g.setColour (Theme::outline.brighter (0.25f));
        g.fillRect (juce::Rectangle<float> (x, gridTop, 1.0f, gridBottom - gridTop));

        if (bar < numBars && bar % labelEvery == 0)
        {
            g.setColour (Theme::textDim);
            g.drawText (juce::String (firstBarNumber + bar), juce::Rectangle<float> (x + 4.0f, gridTop, 40.0f, (float) kRulerHeight),
                        juce::Justification::centredLeft, false);
        }

        if (bar < numBars && beatPx >= 6.0)
        {
            g.setColour (Theme::outline.withAlpha (0.6f));

            for (int beat = 1; (double) beat * beatQn < qnPerBar - 1.0e-9; ++beat)
            {
                const auto bx = xOf (barPpq + beat * beatQn);
                g.fillRect (juce::Rectangle<float> (bx, (float) area.getY(), 1.0f, gridBottom - (float) area.getY()));
            }
        }
    }

    // Notes.
    const auto noteWidth = juce::jmax (3.0f, (float) (noteBeats * pxPerQn));

    for (const auto& hit : take.hits)
    {
        const auto x = xOf (hit.ppq);
        const auto lane = laneIndexOf (hit.slotId);

        if (lane < 0)
        {
            if (anyOther)
            {
                g.setColour (Theme::unassigned.withAlpha (0.45f));
                g.fillRect (juce::Rectangle<float> (x, (float) otherStrip.getY() + 2.0f, 2.0f, (float) otherStrip.getHeight() - 4.0f));
            }

            continue;
        }

        auto r = laneRect (lane).withX (x).withWidth (noteWidth).reduced (0.0f, juce::jmin (4.0f, laneHeight * 0.18f));
        const auto loudness = juce::jlimit (0.0f, 1.0f, juce::jmap (hit.peakDb, -40.0f, -6.0f, 0.0f, 1.0f));
        g.setColour (slotColour (hit.slotId).withAlpha (0.6f + 0.4f * loudness));
        g.fillRoundedRectangle (r, 2.0f);
    }

    g.setColour (Theme::outline.withAlpha (0.7f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
}

//==================================================================================================
DragClipHandle::DragClipHandle (BeatboxProcessor& p, Source s)
    : processor (p), source (s)
{
    refresh();
}

bool DragClipHandle::hasClip() const
{
    return source == Source::learn ? processor.hasLearnClip() : processor.hasTake();
}

int DragClipHandle::dropBar() const
{
    return source == Source::learn ? processor.getLearnClipDropBar() : processor.getTakeDropBar (fromSongStart);
}

juce::File DragClipHandle::writeForDrag() const
{
    return source == Source::learn ? processor.writeLearnClipForDrag() : processor.writeTakeForDrag (fromSongStart);
}

DragClipHandle::~DragClipHandle()
{
    stopTimer();
}

void DragClipHandle::setFromSongStart (bool shouldStartAtSongStart)
{
    fromSongStart = shouldStartAtSongStart;
    refresh();
}

void DragClipHandle::refresh()
{
    const bool clip = hasClip();
    setEnabled (clip);

    if (clip)
        setTooltip ("Drag this onto a MIDI track in Ableton (or into the arrangement) and drop it at bar "
                    + juce::String (dropBar()) + ". The clip's first bar lines up with that bar.");
    else if (source == Source::learn)
        setTooltip ("Record or load something with Learn and choose a sound for its hits first.");
    else
        setTooltip ("Record a take with Capture first.");

    repaint();
}

void DragClipHandle::enablementChanged()
{
    setMouseCursor (isEnabled() ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void DragClipHandle::mouseEnter (const juce::MouseEvent&)    { repaint(); }
void DragClipHandle::mouseExit (const juce::MouseEvent&)     { repaint(); }

void DragClipHandle::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool enabled = isEnabled();
    const bool hover = enabled && isMouseOver (true);

    g.setColour (enabled ? Theme::accent.withAlpha (hover ? 0.24f : 0.14f) : Theme::panelRaised.withAlpha (0.5f));
    g.fillRoundedRectangle (r, 6.0f);

    juce::Path outline;
    outline.addRoundedRectangle (r, 6.0f);
    juce::Path dashed;
    const float dashes[] = { 5.0f, 4.0f };
    juce::PathStrokeType (1.2f).createDashedStroke (dashed, outline, dashes, 2);
    g.setColour (enabled ? Theme::accent : Theme::outline);
    g.fillPath (dashed);

    // Grip dots.
    auto grip = r.removeFromLeft (30.0f).withSizeKeepingCentre (10.0f, 16.0f);
    g.setColour (enabled ? Theme::accent : Theme::textFaint);

    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col)
            g.fillEllipse (grip.getX() + (float) col * 6.0f, grip.getY() + (float) row * 6.0f, 3.5f, 3.5f);

    g.setColour (enabled ? Theme::text : Theme::textFaint);
    g.setFont (font (14.0f, enabled));
    const auto text = feedback.isNotEmpty() ? feedback
                                            : juce::String (source == Source::learn ? "Drag this as MIDI into Ableton"
                                                                                    : "Drag MIDI clip into Ableton");
    g.drawFittedText (text, r.reduced (4.0f, 2.0f).toNearestInt(), juce::Justification::centredLeft, 2, 0.85f);
}

void DragClipHandle::mouseDown (const juce::MouseEvent&)
{
    // Re-arm for this press. If an OS drag never reported back, a new press starts fresh.
    dragArmed = true;
    dragInProgress = false;
}

void DragClipHandle::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragArmed || dragInProgress || ! isEnabled() || e.getDistanceFromDragStart() <= 4)
        return;

    dragArmed = false;

    // Written completely (and closed) before the drag starts; a new unique file every time.
    const auto file = writeForDrag();

    if (! file.existsAsFile())
    {
        showFeedback (source == Source::learn ? "Couldn't write the clip file" : "Couldn't write the clip file - try Save .mid...");
        return;
    }

    dragInProgress = true;
    feedback = "Drop it on a MIDI track at bar " + juce::String (dropBar());
    repaint();

    juce::StringArray files;
    files.add (file.getFullPathName());

    const bool started = juce::DragAndDropContainer::performExternalDragDropOfFiles (
        files, false, this,
        [safeThis = juce::Component::SafePointer<DragClipHandle> (this)]
        {
            if (safeThis == nullptr)
                return;

            safeThis->dragInProgress = false;
            safeThis->showFeedback ("Drag ended. The clip starts at bar "
                                    + juce::String (safeThis->dropBar()) + ".");
        });

    if (! started)
    {
        dragInProgress = false;
        showFeedback (source == Source::learn ? "Couldn't start the drag" : "Couldn't start the drag - use Save .mid... instead");
    }
}

void DragClipHandle::showFeedback (const juce::String& text, int milliseconds)
{
    feedback = text;
    repaint();
    startTimer (milliseconds);
}

void DragClipHandle::timerCallback()
{
    stopTimer();

    if (dragInProgress)
        return;

    feedback.clear();
    repaint();
}

} // namespace ui
