#pragma once

#include <JuceHeader.h>

#include "../PluginProcessor.h"

namespace ui
{

// Shows the Learn recording with a marker per detected hit, coloured by the slot the hit will
// train. Click a marker to hear it up to the next marker; click it again (or right-click) to move
// it to another slot or delete it. Right-click empty space to add a marker or play from there;
// Delete/Backspace removes the selected marker. While the recording plays, a playhead follows it.
// Ctrl/Cmd + wheel (or pinch) zooms, the wheel scrolls, double-click shows everything.
class WaveformView : public juce::Component,
                     public juce::TooltipClient
{
public:
    explicit WaveformView (BeatboxProcessor&);
    ~WaveformView() override;

    // Call when the processor's learn session may have changed.
    void refresh();
    // Call regularly (30 Hz) so the playhead moves.
    void tick();

    // Text shown instead of the waveform when there is no recording (or while recording).
    void setPlaceholder (const juce::String& text, bool large = false);
    // When false the recording is hidden and the placeholder is shown.
    void setShowSession (bool shouldShow);

    int getSelectedHit() const noexcept         { return selectedHit; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;
    bool keyPressed (const juce::KeyPress&) override;
    juce::String getTooltip() override;

private:
    BeatboxProcessor& processor;

    // Identity of the audio the cache was built for.
    const float* audioData = nullptr;
    size_t audioSize = 0;
    double audioRate = 0.0;
    float audioPeak = 1.0f;

    double viewStart = 0.0, viewLength = 0.0;    // samples
    juce::Image cache;
    bool cacheValid = false;
    float cacheScale = 1.0f;

    int selectedHit = -1, hoverHit = -1;
    bool showSession = true;
    juce::String placeholder;
    bool placeholderLarge = false;
    int64_t playhead = -1;                       // sample of the recording playing, -1 if none

    bool hasSession() const;
    juce::Rectangle<int> getWaveArea() const;
    juce::Rectangle<int> getMarkerLane() const;
    float sampleToX (double sample) const;
    double xToSample (float x) const;
    int hitAt (juce::Point<int> pos) const;
    void setView (double start, double length);
    void zoomAround (float x, double factor);
    void rebuildCache (float scale);
    void showHitMenu (int hit);
    void showEmptyMenu (juce::Point<int> pos);
    void removeHit (int hit);
    // True if the session still holds the audio this view was showing (menus are async).
    bool sameAudio (const float* data, size_t size) const;
    juce::String describeHit (int hit) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

} // namespace ui
