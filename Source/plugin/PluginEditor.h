#pragma once

#include <JuceHeader.h>

class BeatboxProcessor;

// Main plugin window: header bar (status, input meter, update banner) above three tabs:
// Learn, Play and Setup. Uses only the public API of BeatboxProcessor and Updater.
class BeatboxEditor : public juce::AudioProcessorEditor
{
public:
    explicit BeatboxEditor (BeatboxProcessor&);
    ~BeatboxEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    BeatboxProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatboxEditor)
};
