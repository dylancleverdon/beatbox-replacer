#pragma once

#include <JuceHeader.h>

#include <memory>

class BeatboxProcessor;
class Updater;

namespace ui
{
    class BbrLookAndFeel;
    class HeaderBar;
    class LearnTab;
    class PlayTab;
    class SetupTab;
}

// Main plugin window: header bar (status, input meter, update banner) above three tabs:
// Learn, Play and Setup. Uses only the public API of BeatboxProcessor and Updater.
class BeatboxEditor : public juce::AudioProcessorEditor,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    explicit BeatboxEditor (BeatboxProcessor&);
    ~BeatboxEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // Named bbProcessor so it doesn't shadow AudioProcessorEditor::processor (-Wshadow-field).
    BeatboxProcessor& bbProcessor;
    juce::SharedResourcePointer<Updater> updater;

    // Declared before the components so it outlives them.
    std::unique_ptr<ui::BbrLookAndFeel> lookAndFeel;

    std::unique_ptr<ui::HeaderBar> header;
    juce::TextButton learnTabButton { "1  Learn" };
    juce::TextButton playTabButton { "2  Play" };
    juce::TextButton setupTabButton { "Setup" };
    std::unique_ptr<ui::LearnTab> learnTab;
    std::unique_ptr<ui::PlayTab> playTab;
    std::unique_ptr<ui::SetupTab> setupTab;
    juce::TooltipWindow tooltipWindow { this, 600 };

    int currentTab = 0;
    bool sizeRestored = false;   // don't save sizes the constructor passes through
    juce::Rectangle<int> tabBarArea;

    void showTab (int index);
    juce::Component* getTabComponent (int index) const;
    juce::String getTabHint() const;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatboxEditor)
};
