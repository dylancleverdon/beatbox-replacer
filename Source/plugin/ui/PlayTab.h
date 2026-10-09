#pragma once

#include <JuceHeader.h>

#include "../PluginProcessor.h"
#include "LookAndFeel.h"
#include "TakeView.h"

#include <memory>
#include <vector>

namespace ui
{

// One pad per slot (plus "Ignored") that flashes whenever a live hit for it is sent.
// Clicking a pad plays one of its training examples.
class PadRow : public juce::Component,
               public juce::SettableTooltipClient
{
public:
    explicit PadRow (BeatboxProcessor&);

    void refresh();   // slots changed
    void tick();      // 30 Hz: poll hit counters, decay flashes

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    struct Pad
    {
        int slotId = 0;
        juce::String name, note;
        juce::Colour colour;
        uint32_t lastCount = 0;
        double flashStartMs = -1.0e9;
        float level = 0.0f;
        juce::Rectangle<int> bounds;
    };

    BeatboxProcessor& processor;
    std::vector<Pad> pads;
    juce::String shownSig;
    int auditionCursor = 0;

    void layoutPads();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadRow)
};

// Name + slider + value box bound to an APVTS parameter, with an optional short note.
class LabeledSlider : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    LabeledSlider (juce::AudioProcessorValueTreeState&, const juce::String& parameterId,
                   const juce::String& name, const juce::String& unit, int decimals,
                   const juce::String& tooltip, const juce::String& note = {});
    ~LabeledSlider() override;

    static constexpr int kHeight = 46;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::Slider slider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    juce::String name, note;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LabeledSlider)
};

// Step 2: get MIDI out. Live MIDI (with routing help and pads), Capture (exact-timing clip to
// drag into Ableton) and the detection settings.
class PlayTab : public juce::Component
{
public:
    explicit PlayTab (BeatboxProcessor&);
    ~PlayTab() override;

    void refresh();   // processor state changed
    void tick();      // 30 Hz while visible

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    BeatboxProcessor& processor;

    // Live MIDI
    juce::ToggleButton liveToggle { "Send live MIDI" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> liveAttachment;
    StepList liveSteps { true, 13.0f };
    PadRow pads;
    int shownWindowMs = -1;

    // Capture
    juce::TextButton captureButton { "Capture" };
    juce::TextButton convertButton { "Convert audio file..." };
    juce::TextButton saveButton { "Save .mid..." };
    juce::TextButton clearButton { "Clear" };
    juce::ComboBox clipStartBox;
    TakeView takeView;
    DragClipHandle dragHandle;
    bool fromSongStart = false;
    bool wasRecording = false;
    double recordingStartMs = 0.0;
    int shownDropBar = -2;
    juce::String captureStatus, transientNote;
    double transientUntilMs = 0.0;

    // Detection
    LabeledSlider thresholdSlider, riseSlider, gapSlider, windowSlider, noteLengthSlider, fixedVelocitySlider;
    juce::ToggleButton dynamicToggle { "Velocity follows loudness" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> dynamicAttachment;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> livePanel, capturePanel, detectionPanel, captureStatusArea, clipStartLabelArea, detectionHintArea;

    void updateLiveSteps();
    void updateCaptureStatus();
    void updateClipStartItems();
    void showNote (const juce::String& text);
    void saveClicked();
    void convertClicked();
    void convertFile (const juce::File&);
    int countTakeNotes() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayTab)
};

} // namespace ui
