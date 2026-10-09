#include "PluginEditor.h"

#include "PluginProcessor.h"
#include "Updater.h"
#include "ui/HeaderBar.h"
#include "ui/LearnTab.h"
#include "ui/LookAndFeel.h"
#include "ui/PlayTab.h"
#include "ui/SetupTab.h"

namespace
{
    constexpr int kMinWidth = 820, kMinHeight = 560;
    constexpr int kMaxWidth = 2400, kMaxHeight = 1600;
    constexpr int kTabBarHeight = 46;
    constexpr int kMargin = 12;
    constexpr int kNumTabs = 3;
}

BeatboxEditor::BeatboxEditor (BeatboxProcessor& p)
    : AudioProcessorEditor (p),
      bbProcessor (p),
      lookAndFeel (std::make_unique<ui::BbrLookAndFeel>())
{
    header = std::make_unique<ui::HeaderBar> (bbProcessor, updater.getObject());
    header->onLayoutChanged = [this] { resized(); };
    addAndMakeVisible (*header);

    learnTabButton.setTooltip ("Step 1: teach the plugin your sounds");
    playTabButton.setTooltip ("Step 2: get MIDI - live notes, or a clip to drag into Ableton");
    setupTabButton.setTooltip ("Ableton routing, profiles, updates and help");

    int index = 0;

    for (auto* button : { &learnTabButton, &playTabButton, &setupTabButton })
    {
        ui::makeTabButton (*button);
        button->onClick = [this, index] { showTab (index); };
        addAndMakeVisible (button);
        ++index;
    }

    learnTab = std::make_unique<ui::LearnTab> (bbProcessor);
    playTab = std::make_unique<ui::PlayTab> (bbProcessor);
    setupTab = std::make_unique<ui::SetupTab> (bbProcessor, updater.getObject());
    addChildComponent (*learnTab);
    addChildComponent (*playTab);
    addChildComponent (*setupTab);

    // After the tree is built: this sends lookAndFeelChanged() to every child, so parts that
    // widgets build from the LookAndFeel (slider text boxes, combo labels) use ours.
    setLookAndFeel (lookAndFeel.get());

    bbProcessor.addChangeListener (this);
    updater->addChangeListener (this);

    // Read the saved size first: setResizeLimits may resize (and so call resized()) on its own.
    const auto savedWidth = juce::jlimit (kMinWidth, kMaxWidth, bbProcessor.editorWidth);
    const auto savedHeight = juce::jlimit (kMinHeight, kMaxHeight, bbProcessor.editorHeight);
    setResizable (true, true);
    setResizeLimits (kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    setSize (savedWidth, savedHeight);
    sizeRestored = true;

    showTab (bbProcessor.lastEditorTab);

    updater->checkForUpdates();
    startTimerHz (30);
}

BeatboxEditor::~BeatboxEditor()
{
    stopTimer();
    updater->removeChangeListener (this);
    bbProcessor.removeChangeListener (this);

    juce::PopupMenu::dismissAllActiveMenus();

    // Message boxes live inside this window: dismiss them (as "Cancel") before it goes away.
    for (int i = getNumChildComponents(); --i >= 0;)
    {
        if (auto* alert = dynamic_cast<juce::AlertWindow*> (getChildComponent (i)))
        {
            if (alert->isCurrentlyModal())
                alert->exitModalState (0);

            removeChildComponent (alert);
        }
    }

    setLookAndFeel (nullptr);
}

juce::Component* BeatboxEditor::getTabComponent (int index) const
{
    if (index == 0)
        return learnTab.get();

    if (index == 1)
        return playTab.get();

    return setupTab.get();
}

void BeatboxEditor::showTab (int index)
{
    currentTab = juce::jlimit (0, kNumTabs - 1, index);
    bbProcessor.lastEditorTab = currentTab;

    learnTabButton.setToggleState (currentTab == 0, juce::dontSendNotification);
    playTabButton.setToggleState (currentTab == 1, juce::dontSendNotification);
    setupTabButton.setToggleState (currentTab == 2, juce::dontSendNotification);

    for (int i = 0; i < kNumTabs; ++i)
        if (auto* tab = getTabComponent (i))
            tab->setVisible (i == currentTab);

    if (currentTab == 0)
        learnTab->refresh();
    else if (currentTab == 1)
        playTab->refresh();
    else
        setupTab->refresh();

    repaint (tabBarArea);
}

juce::String BeatboxEditor::getTabHint() const
{
    const auto numSlots = (int) bbProcessor.getSlots().size();
    const auto numExamples = (int) bbProcessor.getTrainingHits().size();

    if (numExamples == 0)
        return "Start with 1  Learn: teach the plugin your sounds";

    return juce::String (numSlots) + (numSlots == 1 ? " sound, " : " sounds, ")
           + juce::String (numExamples) + (numExamples == 1 ? " example learned" : " examples learned");
}

void BeatboxEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::Theme::background);

    g.setColour (ui::Theme::outline.withAlpha (0.6f));
    g.fillRect (tabBarArea.withTop (tabBarArea.getBottom() - 1));

    auto hint = tabBarArea.withTrimmedLeft (3 * 118 + 16);
    g.setColour (ui::Theme::textDim);
    g.setFont (ui::font (13.0f));
    g.drawFittedText (getTabHint(), hint, juce::Justification::centredRight, 1, 0.85f);
}

void BeatboxEditor::resized()
{
    auto r = getLocalBounds();
    header->setBounds (r.removeFromTop (header->getPreferredHeight()));

    tabBarArea = r.removeFromTop (kTabBarHeight).reduced (kMargin, 0);
    auto tabs = tabBarArea.withTrimmedTop (8).withTrimmedBottom (1);
    learnTabButton.setBounds (tabs.removeFromLeft (114));
    tabs.removeFromLeft (4);
    playTabButton.setBounds (tabs.removeFromLeft (114));
    tabs.removeFromLeft (4);
    setupTabButton.setBounds (tabs.removeFromLeft (114));

    const auto content = r.reduced (kMargin, 0).withTrimmedTop (10).withTrimmedBottom (kMargin);
    learnTab->setBounds (content);
    playTab->setBounds (content);
    setupTab->setBounds (content);

    if (sizeRestored)
    {
        bbProcessor.editorWidth = getWidth();
        bbProcessor.editorHeight = getHeight();
    }
}

void BeatboxEditor::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &bbProcessor)
    {
        learnTab->refresh();
        playTab->refresh();
        setupTab->refresh();
        header->tick();
        repaint (tabBarArea);
    }
    else if (source == &updater.getObject())
    {
        header->updaterChanged();
        setupTab->updaterChanged();
    }
}

void BeatboxEditor::timerCallback()
{
    header->tick();

    if (currentTab == 0)
        learnTab->tick();
    else if (currentTab == 1)
        playTab->tick();
    else
        setupTab->tick();
}
