#pragma once

#include <JuceHeader.h>

#include "../PluginProcessor.h"
#include "LookAndFeel.h"

#include <memory>
#include <vector>

namespace ui
{

// "Your sounds": one row per slot (colour, editable name, MIDI note, number of training
// examples, delete), "+ Add sound", the Ignore example count and "Clear all training".
class SlotsPanel : public juce::Component
{
public:
    explicit SlotsPanel (BeatboxProcessor&);
    ~SlotsPanel() override;

    // Call when slots or training may have changed.
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Row;

    BeatboxProcessor& processor;

    juce::Viewport viewport;
    juce::Component rowsContent;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<int> shownIds;

    juce::TextButton addButton { "+ Add sound" };
    juce::TextButton clearButton { "Clear all training" };

    int ignoreCount = 0, totalCount = 0;
    juce::Rectangle<int> columnHeaderArea, footerTextArea;

    void layoutRows();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotsPanel)
};

} // namespace ui
