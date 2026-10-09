#include "WaveformView.h"
#include "LookAndFeel.h"

#include <cmath>

namespace ui
{

namespace
{
    constexpr int kMarkerLane = 16;
    constexpr int kRuler = 16;
    constexpr float kHitRadiusPx = 6.0f;
    constexpr double kMinViewSeconds = 0.1;

    // Popup menu item ids.
    constexpr int kMenuPlay = 1, kMenuIgnore = 2, kMenuFollow = 3, kMenuSlotBase = 100;

    juce::String secondsLabel (double t, double step)
    {
        if (step >= 1.0)
            return juce::String (juce::roundToInt (t)) + "s";

        auto text = juce::String (t, 2);

        if (text.containsChar ('.'))
            text = text.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

        return text + "s";
    }
}

WaveformView::WaveformView (BeatboxProcessor& p)
    : processor (p)
{
    setOpaque (false);
}

WaveformView::~WaveformView() = default;

bool WaveformView::hasSession() const
{
    return showSession && audioSize > 0 && processor.getLearnSession().hasAudio();
}

void WaveformView::refresh()
{
    auto& session = processor.getLearnSession();
    const auto& audio = session.getAudio();

    if (audio.data() != audioData || audio.size() != audioSize || ! juce::exactlyEqual (session.getSampleRate(), audioRate))
    {
        audioData = audio.data();
        audioSize = audio.size();
        audioRate = session.getSampleRate();

        float peak = 0.0f;

        for (auto v : audio)
            peak = juce::jmax (peak, std::abs (v));

        // Normalise quiet recordings for display, but don't blow up silence.
        audioPeak = juce::jmax (peak, 0.02f);
        viewStart = 0.0;
        viewLength = (double) audioSize;
        selectedHit = hoverHit = -1;
        cacheValid = false;
    }

    if (selectedHit >= session.numHits())
        selectedHit = -1;

    if (hoverHit >= session.numHits())
        hoverHit = -1;

    repaint();
}

void WaveformView::setPlaceholder (const juce::String& text, bool large)
{
    if (text == placeholder && large == placeholderLarge)
        return;

    placeholder = text;
    placeholderLarge = large;
    repaint();
}

void WaveformView::setShowSession (bool shouldShow)
{
    if (showSession == shouldShow)
        return;

    showSession = shouldShow;
    selectedHit = hoverHit = -1;
    repaint();
}

juce::Rectangle<int> WaveformView::getWaveArea() const
{
    return getLocalBounds().reduced (1).withTrimmedTop (kMarkerLane).withTrimmedBottom (kRuler);
}

juce::Rectangle<int> WaveformView::getMarkerLane() const
{
    return getLocalBounds().reduced (1).removeFromTop (kMarkerLane);
}

float WaveformView::sampleToX (double sample) const
{
    const auto area = getWaveArea();

    if (viewLength <= 0.0)
        return (float) area.getX();

    return (float) area.getX() + (float) ((sample - viewStart) / viewLength * (double) area.getWidth());
}

double WaveformView::xToSample (float x) const
{
    const auto area = getWaveArea();

    if (area.getWidth() <= 0)
        return viewStart;

    return viewStart + (double) (x - (float) area.getX()) / (double) area.getWidth() * viewLength;
}

int WaveformView::hitAt (juce::Point<int> pos) const
{
    if (! hasSession())
        return -1;

    auto& session = processor.getLearnSession();
    int best = -1;
    float bestDistance = kHitRadiusPx;

    for (int i = 0; i < session.numHits(); ++i)
    {
        const auto distance = std::abs (sampleToX ((double) session.getHit (i).onsetSample) - (float) pos.x);

        if (distance <= bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }

    return best;
}

void WaveformView::setView (double start, double length)
{
    const auto total = (double) audioSize;

    if (total <= 0.0)
        return;

    const auto minLength = juce::jmin (total, juce::jmax (16.0, kMinViewSeconds * audioRate));
    length = juce::jlimit (minLength, total, length);
    start = juce::jlimit (0.0, total - length, start);

    if (juce::exactlyEqual (start, viewStart) && juce::exactlyEqual (length, viewLength))
        return;

    viewStart = start;
    viewLength = length;
    cacheValid = false;
    repaint();
}

void WaveformView::zoomAround (float x, double factor)
{
    const auto anchor = xToSample (x);
    const auto newLength = viewLength * factor;
    const auto proportion = viewLength > 0.0 ? (anchor - viewStart) / viewLength : 0.0;
    setView (anchor - proportion * newLength, newLength);
}

void WaveformView::resized()
{
    cacheValid = false;
}

void WaveformView::rebuildCache (float scale)
{
    const auto area = getWaveArea();
    const int w = juce::jmax (1, juce::roundToInt ((float) area.getWidth() * scale));
    const int h = juce::jmax (1, juce::roundToInt ((float) area.getHeight() * scale));

    cache = juce::Image (juce::Image::ARGB, w, h, true);
    cacheScale = scale;
    cacheValid = true;

    const auto& audio = processor.getLearnSession().getAudio();
    const auto total = (juce::int64) audio.size();

    if (total == 0 || viewLength <= 0.0)
        return;

    juce::Graphics g (cache);
    const float mid = (float) h * 0.5f;
    const float gain = 0.92f * mid / audioPeak;
    const double samplesPerColumn = viewLength / (double) w;

    g.setColour (juce::Colour (0xff7d8aa3));

    for (int x = 0; x < w; ++x)
    {
        auto s0 = (juce::int64) std::floor (viewStart + (double) x * samplesPerColumn);
        auto s1 = (juce::int64) std::floor (viewStart + (double) (x + 1) * samplesPerColumn);
        s1 = juce::jmax (s1, s0 + 1);
        s0 = juce::jlimit ((juce::int64) 0, total, s0);
        s1 = juce::jlimit ((juce::int64) 0, total, s1);

        if (s0 >= s1)
            continue;

        float lo = audio[(size_t) s0];
        float hi = lo;

        for (auto i = s0 + 1; i < s1; ++i)
        {
            const auto v = audio[(size_t) i];
            lo = juce::jmin (lo, v);
            hi = juce::jmax (hi, v);
        }

        auto top = mid - hi * gain;
        auto bottom = mid - lo * gain;

        if (bottom - top < scale)
        {
            const auto c = (top + bottom) * 0.5f;
            top = c - scale * 0.5f;
            bottom = c + scale * 0.5f;
        }

        g.fillRect (juce::Rectangle<float> ((float) x, top, 1.0f, bottom - top));
    }
}

void WaveformView::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (Theme::background.brighter (0.03f));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (Theme::outline.withAlpha (0.7f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    if (! hasSession())
    {
        drawParagraph (g, placeholder, bounds.reduced (24.0f, 12.0f), placeholderLarge ? 20.0f : 15.0f,
                       placeholderLarge ? Theme::text : Theme::textDim,
                       juce::Justification::centred);
        return;
    }

    auto& session = processor.getLearnSession();
    const auto wave = getWaveArea();
    const auto lane = getMarkerLane();

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (getLocalBounds().reduced (1));

        // Centre line + waveform.
        g.setColour (Theme::outline.withAlpha (0.5f));
        g.fillRect (juce::Rectangle<float> ((float) wave.getX(), (float) wave.getCentreY(), (float) wave.getWidth(), 1.0f));

        const auto scale = g.getInternalContext().getPhysicalPixelScaleFactor();

        if (! cacheValid || cache.isNull() || ! juce::approximatelyEqual (scale, cacheScale))
            rebuildCache (scale);

        g.drawImage (cache, wave.toFloat());

        // Time ruler.
        if (audioRate > 0.0 && viewLength > 0.0)
        {
            const double seconds = viewLength / audioRate;
            const double pxPerSecond = (double) wave.getWidth() / seconds;
            const double steps[] = { 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0 };
            double step = 60.0;

            for (auto s : steps)
            {
                if (s * pxPerSecond >= 70.0)
                {
                    step = s;
                    break;
                }
            }

            const auto ruler = getLocalBounds().reduced (1).removeFromBottom (kRuler);
            const double firstTick = std::ceil (viewStart / audioRate / step) * step;
            g.setFont (font (11.0f));

            for (double t = firstTick; t * audioRate <= viewStart + viewLength; t += step)
            {
                const auto x = sampleToX (t * audioRate);
                g.setColour (Theme::outline);
                g.fillRect (juce::Rectangle<float> (x, (float) ruler.getY(), 1.0f, 4.0f));
                g.setColour (Theme::textFaint);
                g.drawText (secondsLabel (t, step), juce::Rectangle<float> (x + 3.0f, (float) ruler.getY(), 60.0f, (float) ruler.getHeight()),
                            juce::Justification::centredLeft, false);
            }

            // Scroll indicator when zoomed in.
            if (viewLength < (double) audioSize - 1.0)
            {
                const auto total = (double) audioSize;
                const auto trackW = (float) wave.getWidth();
                const auto x0 = (float) wave.getX() + (float) (viewStart / total) * trackW;
                const auto w = juce::jmax (12.0f, (float) (viewLength / total) * trackW);
                g.setColour (Theme::accent.withAlpha (0.5f));
                g.fillRoundedRectangle (juce::Rectangle<float> (x0, (float) ruler.getBottom() - 3.0f, w, 3.0f), 1.5f);
            }
        }

        // Hit markers. Draw the selected one last so it sits on top.
        auto drawHit = [&] (int i)
        {
            const auto x = sampleToX ((double) session.getHit (i).onsetSample);

            if (x < (float) wave.getX() - 10.0f || x > (float) wave.getRight() + 10.0f)
                return;

            const bool selected = i == selectedHit;
            const bool hovered = i == hoverHit;
            const auto slotId = session.getHitSlot (i);
            auto colour = slotColour (slotId);

            g.setColour (colour.withAlpha (selected ? 0.95f : (hovered ? 0.8f : 0.5f)));
            g.fillRect (juce::Rectangle<float> (x - (selected ? 1.0f : 0.5f), (float) wave.getY(),
                                                selected ? 2.0f : 1.0f, (float) wave.getHeight()));

            const float half = selected ? 7.0f : (hovered ? 6.0f : 5.0f);
            const auto top = (float) lane.getY() + 2.0f;
            juce::Path tri;
            tri.addTriangle (x - half, top, x + half, top, x, top + half * 1.6f);
            g.setColour (colour);
            g.fillPath (tri);

            if (session.hasHitOverride (i) || selected)
            {
                g.setColour (selected ? Theme::text : Theme::text.withAlpha (0.85f));
                g.strokePath (tri, juce::PathStrokeType (selected ? 1.6f : 1.2f));
            }
        };

        for (int i = 0; i < session.numHits(); ++i)
            if (i != selectedHit)
                drawHit (i);

        if (selectedHit >= 0 && selectedHit < session.numHits())
            drawHit (selectedHit);
    }

    if (session.numHits() == 0)
    {
        drawParagraph (g, "No hits found in this recording.", wave.toFloat().reduced (12.0f), 15.0f,
                       Theme::text, juce::Justification::centred);
    }
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    const auto hit = hitAt (e.getPosition());

    if (hit != hoverHit)
    {
        hoverHit = hit;
        setMouseCursor (hit >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void WaveformView::mouseExit (const juce::MouseEvent&)
{
    if (hoverHit >= 0)
    {
        hoverHit = -1;
        repaint();
    }
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    if (! hasSession())
        return;

    const auto hit = hitAt (e.getPosition());

    if (e.mods.isPopupMenu())
    {
        if (hit >= 0)
        {
            selectedHit = hit;
            repaint();
            showHitMenu (hit);
        }

        return;
    }

    if (hit < 0)
    {
        if (selectedHit >= 0)
        {
            selectedHit = -1;
            repaint();
        }

        return;
    }

    if (hit == selectedHit)
    {
        showHitMenu (hit);
        return;
    }

    selectedHit = hit;
    repaint();
    processor.auditionLearnHit (hit);
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (hasSession() && hitAt (e.getPosition()) < 0)
        setView (0.0, (double) audioSize);
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! hasSession())
    {
        Component::mouseWheelMove (e, wheel);
        return;
    }

    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const auto delta = std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
        zoomAround ((float) e.x, std::exp (-(double) delta * 2.5));
        return;
    }

    const bool zoomed = viewLength < (double) audioSize - 1.0;

    if (! zoomed)
    {
        Component::mouseWheelMove (e, wheel);
        return;
    }

    const auto delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? -wheel.deltaX : wheel.deltaY;
    setView (viewStart - (double) delta * viewLength * 0.5, viewLength);
}

void WaveformView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    if (hasSession() && scaleFactor > 0.0f)
        zoomAround ((float) e.x, 1.0 / (double) scaleFactor);
}

juce::String WaveformView::describeHit (int hit) const
{
    auto& session = processor.getLearnSession();

    if (hit < 0 || hit >= session.numHits())
        return {};

    juce::String text = "Hit " + juce::String (hit + 1);
    const auto group = session.numGroups() > 0 ? session.groupOfHit (hit) : -1;

    if (group >= 0 && group < session.numGroups())
        text << " - group " << (group + 1);

    const auto slotId = session.getHitSlot (hit);

    if (slotId == bbr::kUnassigned)
        text << " - no sound chosen";
    else if (slotId == bbr::kIgnoreSlotId)
        text << " - ignored";
    else
        text << " - " << processor.getSlotName (slotId);

    if (session.hasHitOverride (hit))
        text << " (set by you)";

    return text;
}

juce::String WaveformView::getTooltip()
{
    if (hoverHit < 0)
        return hasSession() ? "Click a marker to hear that hit. Ctrl/Cmd + scroll to zoom, double-click to see everything."
                            : juce::String();

    if (hoverHit == selectedHit)
        return describeHit (hoverHit) + "\nClick again (or right-click) to choose its sound.";

    return describeHit (hoverHit) + "\nClick to hear it, right-click to choose its sound.";
}

void WaveformView::showHitMenu (int hit)
{
    auto& session = processor.getLearnSession();

    if (hit < 0 || hit >= session.numHits())
        return;

    const auto slots = processor.getSlots();
    const bool overridden = session.hasHitOverride (hit);
    const auto current = session.getHitSlot (hit);
    const auto group = session.numGroups() > 0 ? session.groupOfHit (hit) : -1;
    const auto groupSlot = (group >= 0 && group < session.numGroups()) ? session.getGroupSlot (group) : bbr::kUnassigned;

    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());
    menu.addSectionHeader (describeHit (hit));
    menu.addItem (kMenuPlay, "Play this hit");
    menu.addSeparator();

    for (const auto& slot : slots)
        menu.addColouredItem (kMenuSlotBase + slot.id, slot.name + "   " + noteNameOf (slot.note), slotColour (slot.id),
                              true, overridden && current == slot.id);

    menu.addItem (kMenuIgnore, "Ignore (no note)", true, overridden && current == bbr::kIgnoreSlotId);
    menu.addSeparator();
    menu.addItem (kMenuFollow,
                  groupSlot == bbr::kUnassigned ? juce::String ("Follow group (no sound chosen yet)")
                                                : "Follow group (" + processor.getSlotName (groupSlot) + ")",
                  true, ! overridden);

    const auto x = juce::roundToInt (sampleToX ((double) session.getHit (hit).onsetSample));
    const auto target = localAreaToGlobal (juce::Rectangle<int> (x - 6, getMarkerLane().getY(), 12, kMarkerLane));
    const auto* expectedAudio = audioData;
    const auto expectedSize = audioSize;

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (target),
                        [safeThis = juce::Component::SafePointer<WaveformView> (this), hit, expectedAudio, expectedSize] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        auto& proc = safeThis->processor;
        auto& s = proc.getLearnSession();

        // The session may have been replaced while the menu was open.
        if (s.getAudio().data() != expectedAudio || s.getAudio().size() != expectedSize || hit >= s.numHits())
            return;

        if (result == kMenuPlay)
            proc.auditionLearnHit (hit);
        else if (result == kMenuIgnore)
            proc.setLearnHitSlot (hit, bbr::kIgnoreSlotId);
        else if (result == kMenuFollow)
            proc.setLearnHitSlot (hit, bbr::kUnassigned);
        else if (result >= kMenuSlotBase && proc.hasSlot (result - kMenuSlotBase))
            proc.setLearnHitSlot (hit, result - kMenuSlotBase);

        safeThis->refresh();
    });
}

} // namespace ui
