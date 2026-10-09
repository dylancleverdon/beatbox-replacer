#pragma once

#include <JuceHeader.h>

#include "../PluginProcessor.h"

namespace ui
{

// Piano roll of the last Capture take: one lane per slot, bar/beat grid in the take's time
// signature, notes coloured by slot. Ignored / unassigned hits are drawn faintly underneath.
class TakeView : public juce::Component
{
public:
    explicit TakeView (BeatboxProcessor&);

    // Text shown when there is no take (or while a new one is recording).
    void setPlaceholder (const juce::String& text, bool hideTake);
    void refresh()                 { repaint(); }

    void paint (juce::Graphics&) override;

private:
    BeatboxProcessor& processor;
    juce::String placeholder;
    bool hideTake = false;

    double noteLengthBeats (double bpm) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakeView)
};

// "Drag MIDI clip into Ableton". On drag it writes the clip to a new .mid file (complete and
// closed) and starts an external file drag. No claims about where it landed: hosts don't say.
// The clip is the last Capture take, or (Source::learn) the labelled hits of the Learn recording.
class DragClipHandle : public juce::Component,
                       public juce::SettableTooltipClient,
                       private juce::Timer
{
public:
    enum class Source { take, learn };

    explicit DragClipHandle (BeatboxProcessor&, Source = Source::take);
    ~DragClipHandle() override;

    void setFromSongStart (bool shouldStartAtSongStart);
    // Call when the take may have changed.
    void refresh();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void enablementChanged() override;

private:
    BeatboxProcessor& processor;
    const Source source;
    bool fromSongStart = false;
    bool dragArmed = false;        // set on mouseDown; one drag attempt per press
    bool dragInProgress = false;   // cleared by the completion callback (or when starting fails)
    juce::String feedback;

    bool hasClip() const;
    int dropBar() const;
    juce::File writeForDrag() const;
    void showFeedback (const juce::String& text, int milliseconds = 4000);
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DragClipHandle)
};

} // namespace ui
