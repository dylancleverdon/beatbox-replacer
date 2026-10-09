#pragma once

#include <JuceHeader.h>

#include "../PluginProcessor.h"
#include "LookAndFeel.h"
#include "SlotsPanel.h"
#include "WaveformView.h"

#include <memory>
#include <vector>

namespace ui
{

// Step 1: teach the plugin your sounds. Record (or load) some beatboxing, check the automatic
// groups, pick a sound for each group, fix single hits, then add the result to the training set.
class LearnTab : public juce::Component,
                 public juce::FileDragAndDropTarget
{
public:
    explicit LearnTab (BeatboxProcessor&);
    ~LearnTab() override;

    void refresh();   // processor state changed
    void tick();      // 30 Hz while visible

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    class GroupCard;
    class GroupsContent;

    BeatboxProcessor& processor;

    juce::TextButton learnButton;
    juce::TextButton loadButton { "Load audio file..." };
    WaveformView waveform;

    IconButton fewerGroupsButton { "Fewer groups", IconButton::Icon::minus };
    IconButton moreGroupsButton { "More groups", IconButton::Icon::plus };
    juce::Viewport groupsViewport;
    std::unique_ptr<GroupsContent> groupsContent;
    std::vector<std::unique_ptr<GroupCard>> cards;

    juce::TextButton addButton { "Add to training" };
    juce::TextButton replaceButton { "Replace training" };
    juce::TextButton discardButton { "Discard" };

    SlotsPanel slotsPanel;

    std::unique_ptr<juce::FileChooser> chooser;
    bool fileDragOver = false;
    juce::String message;            // result of the last action, shown when idle
    juce::String guidance;
    BeatboxProcessor::LearnState shownState = BeatboxProcessor::LearnState::idle;
    juce::String shownSlotsSig;
    const float* shownAudio = nullptr;
    size_t shownAudioSize = 0;
    int shownGroups = -1, shownHits = -1;

    juce::Rectangle<int> mainPanel, guidanceArea, groupsHeaderArea, groupCountArea;

    void learnClicked();
    void loadClicked();
    void loadFile (const juce::File&);
    void addClicked();
    void replaceClicked();
    void recluster (int delta);
    void updateLearnButton();
    void updateGuidance();
    void rebuildCards();
    void layoutCards();

    static bool isSupportedAudioFile (const juce::String& path);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LearnTab)
};

} // namespace ui
