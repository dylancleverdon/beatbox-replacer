#pragma once

#include <JuceHeader.h>

#include "../PluginProcessor.h"
#include "../Updater.h"
#include "LookAndFeel.h"

#include <functional>

namespace ui
{

// Top strip: name + version, input meter with a plain-words status, transport dot. Below it,
// when needed: the "load the VST3 in Live" warning (AU build in Live) and the update banner.
class HeaderBar : public juce::Component
{
public:
    HeaderBar (BeatboxProcessor&, Updater&);
    ~HeaderBar() override;

    // Height including whichever banners are showing.
    int getPreferredHeight() const;

    // 30 Hz: meter, status text, transport, download progress.
    void tick();
    // Updater status changed.
    void updaterChanged();

    // Called when a banner appears or disappears (the editor re-lays out).
    std::function<void()> onLayoutChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    BeatboxProcessor& processor;
    Updater& updater;

    float meterDb = -100.0f;
    bool receiving = false, playing = false;
    const bool showAuWarning;

    bool updateBannerVisible = false;
    bool bannerIsError = false;
    juce::String updateText, dismissedKey, currentKey;
    juce::TextButton installButton { "Install update" };
    juce::TextButton notesButton { "What's new" };
    juce::TextButton installerButton { "Run installer" };
    IconButton dismissButton { "Hide", IconButton::Icon::cross };
    double progressValue = -1.0;
    juce::ProgressBar progressBar { progressValue };

    juce::Rectangle<int> mainRow, titleArea, meterArea, statusArea, transportArea, auBannerArea, updateBannerArea, updateTextArea;

    static constexpr int kMainHeight = 60;
    static constexpr int kBannerHeight = 38;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBar)
};

} // namespace ui
