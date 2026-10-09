#pragma once

#include <JuceHeader.h>

#include "../PluginProcessor.h"
#include "../Updater.h"
#include "LookAndFeel.h"

#include <memory>

namespace ui
{

// Ableton routing steps, profile export/import, updates and troubleshooting. Scrolls when the
// window is too short.
class SetupTab : public juce::Component
{
public:
    SetupTab (BeatboxProcessor&, Updater&);
    ~SetupTab() override;

    void refresh();          // processor state changed
    void updaterChanged();   // updater status changed
    void tick();             // 30 Hz while visible (download progress)

    void resized() override;

private:
    class Content;

    juce::Viewport viewport;
    std::unique_ptr<Content> content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SetupTab)
};

} // namespace ui
