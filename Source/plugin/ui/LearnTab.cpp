#include "LearnTab.h"

namespace ui
{

namespace
{
    constexpr int kCardWidth = 178;
    constexpr int kCardGap = 10;
    constexpr int kMaxGroups = 16;

    using LearnState = BeatboxProcessor::LearnState;

    bool isRecordingState (LearnState s)
    {
        return s == LearnState::armed || s == LearnState::recording;
    }

    juce::String plural (int n, const char* singular, const char* pluralForm)
    {
        return juce::String (n) + " " + (n == 1 ? singular : pluralForm);
    }
}

//==================================================================================================
class LearnTab::GroupsContent : public juce::Component
{
public:
    bool hasCards = false;

    void paint (juce::Graphics& g) override
    {
        if (! hasCards)
            drawParagraph (g, "Groups of similar-sounding hits show up here after you record.",
                           getLocalBounds().toFloat().reduced (4.0f, 8.0f), 14.0f, Theme::textFaint,
                           juce::Justification::centredLeft);
    }
};

//==================================================================================================
class LearnTab::GroupCard : public juce::Component
{
public:
    GroupCard (BeatboxProcessor& p, int groupIndex)
        : processor (p), group (groupIndex)
    {
        playButton.setTooltip ("Play a typical hit from this group");
        playButton.onClick = [this]
        {
            auto& session = processor.getLearnSession();

            if (group >= session.numGroups())
                return;

            const auto hit = session.representativeHit (group);

            if (hit >= 0 && hit < session.numHits())
                processor.auditionLearnHit (hit);
        };
        addAndMakeVisible (playButton);

        slotBox.setTooltip ("Which of your sounds is this group? Choose Ignore for breaths, clicks and other noises "
                            "that should never make a note.");
        slotBox.onChange = [this]
        {
            const auto itemId = slotBox.getSelectedId();

            if (itemId == 0 || group >= processor.getLearnSession().numGroups())
                return;

            const auto chosen = slotForComboId (itemId);

            if (chosen >= 0 && ! processor.hasSlot (chosen))
                return;

            processor.setLearnGroupSlot (group, chosen);
        };
        addAndMakeVisible (slotBox);
    }

    void update (const std::vector<BeatboxProcessor::Slot>& slots, const juce::String& slotsSig)
    {
        auto& session = processor.getLearnSession();

        if (group >= session.numGroups())
            return;

        if (slotsSig != comboSig)
        {
            fillSlotCombo (slotBox, slots, true);
            comboSig = slotsSig;
        }

        slotId = session.getGroupSlot (group);
        size = session.groupSize (group);

        auto itemId = comboIdForSlot (slotId);

        if (slotBox.indexOfItemId (itemId) < 0)
            itemId = comboIdForSlot (bbr::kUnassigned);

        if (slotBox.getSelectedId() != itemId)
            slotBox.setSelectedId (itemId, juce::dontSendNotification);

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool unassigned = slotId == bbr::kUnassigned;

        g.setColour (Theme::panelRaised);
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (unassigned ? Theme::warning.withAlpha (0.7f) : Theme::outline);
        g.drawRoundedRectangle (r, 7.0f, 1.0f);

        g.setColour (slotColour (slotId));
        g.fillRoundedRectangle (r.removeFromLeft (6.0f).reduced (0.0f, 6.0f).translated (6.0f, 0.0f), 3.0f);

        auto text = getLocalBounds().reduced (20, 8).withTrimmedRight (28).removeFromTop (22);
        g.setColour (Theme::text);
        g.setFont (font (14.0f, true));
        g.drawText ("Group " + juce::String (group + 1), text, juce::Justification::centredLeft, true);

        auto sub = getLocalBounds().reduced (20, 8).withTrimmedTop (22).removeFromTop (18);
        g.setFont (font (12.0f));
        g.setColour (unassigned ? Theme::warning : Theme::textDim);
        g.drawText (plural (size, "hit", "hits") + (unassigned ? " - choose a sound" : ""), sub,
                    juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8).withTrimmedLeft (10);
        playButton.setBounds (r.removeFromTop (26).removeFromRight (26));
        slotBox.setBounds (r.removeFromBottom (28));
    }

private:
    BeatboxProcessor& processor;
    const int group;
    IconButton playButton { "Play", IconButton::Icon::play };
    juce::ComboBox slotBox;
    juce::String comboSig;
    int slotId = bbr::kUnassigned;
    int size = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GroupCard)
};

//==================================================================================================
LearnTab::LearnTab (BeatboxProcessor& p)
    : processor (p), waveform (p), slotsPanel (p)
{
    learnButton.onClick = [this] { learnClicked(); };
    addAndMakeVisible (learnButton);

    loadButton.setTooltip ("Learn from a recording instead (WAV, AIFF, FLAC, OGG or MP3). You can also drop a file here.");
    loadButton.onClick = [this] { loadClicked(); };
    addAndMakeVisible (loadButton);

    addAndMakeVisible (waveform);

    fewerGroupsButton.setTooltip ("Merge into fewer groups");
    fewerGroupsButton.onClick = [this] { recluster (-1); };
    addAndMakeVisible (fewerGroupsButton);

    moreGroupsButton.setTooltip ("Split into more groups - useful when two different sounds ended up in one group");
    moreGroupsButton.onClick = [this] { recluster (1); };
    addAndMakeVisible (moreGroupsButton);

    groupsContent = std::make_unique<GroupsContent>();
    groupsViewport.setViewedComponent (groupsContent.get(), false);
    groupsViewport.setScrollBarsShown (false, true);
    groupsViewport.setScrollBarThickness (8);
    addAndMakeVisible (groupsViewport);

    makeAccentButton (addButton);
    addButton.setTooltip ("Keep these hits as examples, on top of everything the plugin already knows");
    addButton.onClick = [this] { addClicked(); };
    addAndMakeVisible (addButton);

    replaceButton.setTooltip ("Throw away all existing examples and keep only the hits from this recording");
    replaceButton.onClick = [this] { replaceClicked(); };
    addAndMakeVisible (replaceButton);

    discardButton.setTooltip ("Throw this recording away");
    discardButton.onClick = [this]
    {
        processor.discardLearnSession();
        message = "Recording discarded.";
        refresh();
    };
    addAndMakeVisible (discardButton);

    addAndMakeVisible (slotsPanel);

    refresh();
}

LearnTab::~LearnTab()
{
    groupsViewport.setViewedComponent (nullptr, false);
}

bool LearnTab::isSupportedAudioFile (const juce::String& path)
{
    const auto ext = juce::File (path).getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".ogg" || ext == ".mp3";
}

//==================================================================================================
void LearnTab::refresh()
{
    auto& session = processor.getLearnSession();
    const auto state = processor.getLearnState();
    const auto slots = processor.getSlots();
    const auto slotsSig = slotsSignature (slots);
    const bool showSession = ! isRecordingState (state);

    waveform.setShowSession (showSession);
    waveform.refresh();

    const auto groupsToShow = showSession ? session.numGroups() : 0;
    const auto hits = showSession ? session.numHits() : 0;

    if (session.getAudio().data() != shownAudio || session.getAudio().size() != shownAudioSize
        || groupsToShow != shownGroups || hits != shownHits)
    {
        shownAudio = session.getAudio().data();
        shownAudioSize = session.getAudio().size();
        shownGroups = groupsToShow;
        shownHits = hits;
        rebuildCards();
    }

    for (auto& card : cards)
        card->update (slots, slotsSig);

    shownSlotsSig = slotsSig;
    shownState = state;

    const bool hasHits = hits > 0;
    addButton.setEnabled (hasHits);
    replaceButton.setEnabled (hasHits);
    discardButton.setEnabled (showSession && session.hasAudio());
    loadButton.setEnabled (! isRecordingState (state));
    fewerGroupsButton.setVisible (groupsToShow > 0);
    moreGroupsButton.setVisible (groupsToShow > 0);
    fewerGroupsButton.setEnabled (hasHits && groupsToShow > 1);
    moreGroupsButton.setEnabled (hasHits && groupsToShow < juce::jmin (hits, kMaxGroups));

    slotsPanel.refresh();
    updateLearnButton();
    updateGuidance();
    repaint();
}

void LearnTab::tick()
{
    const auto state = processor.getLearnState();

    if (state != shownState)
        refresh();
    else if (isRecordingState (state))
        updateLearnButton();
}

void LearnTab::rebuildCards()
{
    cards.clear();

    for (int g = 0; g < shownGroups; ++g)
    {
        auto card = std::make_unique<GroupCard> (processor, g);
        groupsContent->addAndMakeVisible (*card);
        card->sendLookAndFeelChange();
        cards.push_back (std::move (card));
    }

    groupsContent->hasCards = ! cards.empty();
    layoutCards();
    groupsContent->repaint();
}

void LearnTab::layoutCards()
{
    const auto count = (int) cards.size();
    const auto needed = count * (kCardWidth + kCardGap) - (count > 0 ? kCardGap : 0);
    const auto viewW = groupsViewport.getWidth();
    const auto h = groupsViewport.getHeight() - (needed > viewW ? groupsViewport.getScrollBarThickness() + 2 : 0);

    groupsContent->setSize (juce::jmax (viewW, needed), juce::jmax (0, h));

    for (int i = 0; i < count; ++i)
        cards[(size_t) i]->setBounds (i * (kCardWidth + kCardGap), 0, kCardWidth, juce::jmax (0, h));
}

void LearnTab::updateLearnButton()
{
    const auto state = processor.getLearnState();
    juce::String text, tip;
    auto colour = Theme::accent;
    auto textColour = Theme::background;

    if (state == LearnState::armed)
    {
        text = "Waiting for playback... (press play in Ableton)";
        tip = "Click to cancel.";
        colour = Theme::warning.darker (0.55f);
        textColour = Theme::text;
    }
    else if (state == LearnState::recording)
    {
        text = "Recording " + juce::String (processor.getLearnRecordedSeconds(), 1) + " s - stop playback when done";
        tip = "Click to stop now. Recording stops by itself after "
              + juce::String (juce::roundToInt (BeatboxProcessor::kMaxLearnSeconds)) + " s.";
        colour = Theme::recording;
        textColour = Theme::text;
    }
    else if (state == LearnState::ready)
    {
        text = "Learn again";
        tip = "Record a new pass. The current one is thrown away unless you add it to training first.";
        colour = Theme::panelRaised;
        textColour = Theme::text;
    }
    else
    {
        text = "Learn";
        tip = "Records your beatbox (from the sidechain) while Ableton plays. Beatbox each of your sounds "
              "several times, then stop playback.";
    }

    if (learnButton.getButtonText() != text)
        learnButton.setButtonText (text);

    learnButton.setTooltip (tip);
    learnButton.setColour (juce::TextButton::buttonColourId, colour);
    learnButton.setColour (juce::TextButton::textColourOffId, textColour);

    if (state == LearnState::recording)
        waveform.setPlaceholder ("*Recording " + juce::String (processor.getLearnRecordedSeconds(), 1) + " s*\n"
                                 "Beatbox each of your sounds 5-10 times, then stop playback.", true);
    else if (state == LearnState::armed)
        waveform.setPlaceholder ("*Waiting for playback*\nPress play in Ableton. Recording starts with playback "
                                 "and stops when you stop.", true);
    else if (state == LearnState::ready && ! processor.getLearnSession().hasAudio())
        waveform.setPlaceholder ("Nothing was recorded. Check that the Input meter at the top moves when you "
                                 "beatbox, then press Learn again.", false);
    else
        waveform.setPlaceholder ("Your recording shows up here.\nPress *Learn* and play your beatbox in Ableton, "
                                 "or drop an audio file here.", false);
}

void LearnTab::updateGuidance()
{
    auto& session = processor.getLearnSession();
    const auto state = processor.getLearnState();
    juce::String text;

    if (state == LearnState::armed)
    {
        text = "Press *play* in Ableton (or start your beatbox clip). Recording starts with playback and stops "
               "when you stop. Click the button to cancel.";
    }
    else if (state == LearnState::recording)
    {
        text = "Beatbox each of your sounds 5-10 times with small gaps between hits. Stop playback when you're done.";
    }
    else if (session.numHits() > 0)
    {
        text = "Found *" + plural (session.numHits(), "hit", "hits") + "* in *"
               + plural (session.numGroups(), "group", "groups")
               + "*. Choose a sound for each group, click single hits to fix mistakes, then *Add to training*.";

        const auto counts = session.slotCounts();
        const auto unassigned = counts.size() > (size_t) bbr::kMaxSlots + 1 ? counts[(size_t) bbr::kMaxSlots + 1] : 0;

        if (unassigned > 0)
            text << " " << plural (unassigned, "hit has", "hits have") << " no sound yet and will be skipped.";
    }
    else if (state == LearnState::ready)
    {
        text = "No hits found. Make sure your beatbox reaches this device's sidechain (the Input meter at the top "
               "should move), then press Learn again. Very quiet? Lower the Sensitivity gate on the Play tab.";
    }
    else if (message.isNotEmpty())
    {
        text = message;
    }
    else if (processor.getTrainingHits().empty())
    {
        text = "*Step 1:* press *Learn*, then play a part of your song where you beatbox each of your sounds "
               "several times. Stop playback when you're done.";
    }
    else
    {
        text = "Press *Learn* to teach more examples (great for sounds that get mixed up), or go to *2  Play* "
               "to turn your beatbox into MIDI.";
    }

    if (text != guidance)
    {
        guidance = text;
        repaint (guidanceArea);
    }
}

//==================================================================================================
void LearnTab::learnClicked()
{
    const auto state = processor.getLearnState();

    if (isRecordingState (state))
    {
        processor.stopLearn();
        refresh();
        return;
    }

    auto start = [safeThis = juce::Component::SafePointer<LearnTab> (this)]
    {
        if (safeThis == nullptr)
            return;

        if (safeThis->processor.getLearnState() == LearnState::ready)
            safeThis->processor.discardLearnSession();

        safeThis->message.clear();
        safeThis->processor.startLearn();
        safeThis->refresh();
    };

    if (state == LearnState::ready && processor.getLearnSession().numHits() > 0)
    {
        confirm (*this, "Record again?",
                 "These hits haven't been added to training yet. Throw them away and record a new pass?",
                 "Record again", start);
        return;
    }

    start();
}

void LearnTab::loadClicked()
{
    if (isRecordingState (processor.getLearnState()))
        return;

    chooser = std::make_unique<juce::FileChooser> ("Choose a recording of your beatboxing",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safeThis = juce::Component::SafePointer<LearnTab> (this)] (const juce::FileChooser& fc)
    {
        if (safeThis == nullptr)
            return;

        const auto file = fc.getResult();

        if (file.existsAsFile())
            safeThis->loadFile (file);
    });
}

void LearnTab::loadFile (const juce::File& file)
{
    if (isRecordingState (processor.getLearnState()))
    {
        showMessage (*this, "Still recording", "Stop the current Learn recording first, then load the file.");
        return;
    }

    auto load = [safeThis = juce::Component::SafePointer<LearnTab> (this), file]
    {
        if (safeThis == nullptr)
            return;

        juce::MouseCursor::showWaitCursor();
        const auto error = safeThis->processor.learnFromFile (file);
        juce::MouseCursor::hideWaitCursor();

        if (safeThis == nullptr)
            return;

        if (error.isNotEmpty())
            showMessage (*safeThis, "Couldn't use " + file.getFileName(), error, true);
        else
            safeThis->message.clear();

        safeThis->refresh();
    };

    if (processor.getLearnState() == LearnState::ready && processor.getLearnSession().numHits() > 0)
    {
        confirm (*this, "Replace the current recording?",
                 "These hits haven't been added to training yet. Throw them away and learn from \""
                     + file.getFileName() + "\"?",
                 "Replace", load);
        return;
    }

    load();
}

void LearnTab::addClicked()
{
    const auto counts = processor.getLearnSession().slotCounts();
    int labelled = 0;

    for (size_t i = 0; i < counts.size() && i <= (size_t) bbr::kMaxSlots; ++i)
        labelled += counts[i];

    if (labelled == 0)
    {
        showMessage (*this, "Choose sounds first",
                     "None of the hits has a sound yet. Pick a sound (or Ignore) for each group, then try again.");
        return;
    }

    message = "Added " + plural (labelled, "example", "examples") + ". Teach more with Learn, or try it on the *2  Play* tab.";
    processor.commitLearnSession (false);
    refresh();
}

void LearnTab::replaceClicked()
{
    auto replace = [safeThis = juce::Component::SafePointer<LearnTab> (this)]
    {
        if (safeThis == nullptr)
            return;

        const auto counts = safeThis->processor.getLearnSession().slotCounts();
        int labelled = 0;

        for (size_t i = 0; i < counts.size() && i <= (size_t) bbr::kMaxSlots; ++i)
            labelled += counts[i];

        safeThis->message = "Training replaced: the plugin now knows " + plural (labelled, "example", "examples")
                            + " from this recording.";
        safeThis->processor.commitLearnSession (true);
        safeThis->refresh();
    };

    const auto existing = (int) processor.getTrainingHits().size();

    if (existing == 0)
    {
        replace();
        return;
    }

    confirm (*this, "Replace all training?",
             "This deletes the " + plural (existing, "example", "examples")
                 + " you've already taught and keeps only the hits from this recording.",
             "Replace", replace);
}

void LearnTab::recluster (int delta)
{
    auto& session = processor.getLearnSession();
    const auto hits = session.numHits();

    if (hits == 0)
        return;

    const auto k = juce::jlimit (1, juce::jmax (1, juce::jmin (hits, kMaxGroups)), session.numGroups() + delta);

    if (k != session.numGroups())
    {
        processor.reclusterLearnSession (k);
        refresh();
    }
}

//==================================================================================================
void LearnTab::resized()
{
    auto r = getLocalBounds();
    auto right = r.removeFromRight (juce::jlimit (270, 340, r.getWidth() * 31 / 100));
    r.removeFromRight (12);
    slotsPanel.setBounds (right);
    mainPanel = r;

    auto inner = r.reduced (kPanelPadding + 2, kPanelPadding);
    auto top = inner.removeFromTop (50);
    const auto loadWidth = 160;
    learnButton.setBounds (top.removeFromLeft (juce::jmin (390, top.getWidth() - loadWidth - 12)));
    top.removeFromLeft (12);
    loadButton.setBounds (top.removeFromLeft (loadWidth).withSizeKeepingCentre (loadWidth, 34));

    inner.removeFromTop (8);
    guidanceArea = inner.removeFromTop (42);
    inner.removeFromTop (6);

    auto bottom = inner.removeFromBottom (34);
    discardButton.setBounds (bottom.removeFromRight (100));
    bottom.removeFromRight (8);
    replaceButton.setBounds (bottom.removeFromRight (150));
    bottom.removeFromRight (8);
    addButton.setBounds (bottom.removeFromRight (160));

    inner.removeFromBottom (10);
    groupsViewport.setBounds (inner.removeFromBottom (92));
    inner.removeFromBottom (4);
    groupsHeaderArea = inner.removeFromBottom (28);

    auto h = groupsHeaderArea;
    h.removeFromLeft (64);
    fewerGroupsButton.setBounds (h.removeFromLeft (26).withSizeKeepingCentre (26, 26));
    groupCountArea = h.removeFromLeft (28);
    moreGroupsButton.setBounds (h.removeFromLeft (26).withSizeKeepingCentre (26, 26));

    inner.removeFromBottom (6);
    waveform.setBounds (inner);
    layoutCards();
}

void LearnTab::paint (juce::Graphics& g)
{
    paintPanel (g, mainPanel);

    drawParagraph (g, guidance, guidanceArea.toFloat(), 14.0f, Theme::text,
                   juce::Justification::centredLeft);

    g.setColour (Theme::text);
    g.setFont (font (14.0f, true));
    g.drawText ("Groups", groupsHeaderArea.withWidth (64), juce::Justification::centredLeft, false);

    if (shownGroups > 0)
    {
        g.drawText (juce::String (shownGroups), groupCountArea, juce::Justification::centred, false);

        auto hint = groupsHeaderArea.withTrimmedLeft (64 + 26 + 28 + 26 + 14);
        g.setFont (font (12.0f));
        g.setColour (Theme::textDim);
        g.drawFittedText ("Similar hits are grouped automatically. Pick a sound for each group - the play button "
                          "plays a typical hit. Use - and + if two sounds share a group.",
                          hint, juce::Justification::centredLeft, 2, 0.85f);
    }
}

void LearnTab::paintOverChildren (juce::Graphics& g)
{
    if (! fileDragOver)
        return;

    auto r = mainPanel.toFloat().reduced (2.0f);
    g.setColour (Theme::background.withAlpha (0.75f));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (Theme::accent);
    g.drawRoundedRectangle (r.reduced (1.0f), 8.0f, 2.0f);
    drawParagraph (g, "*Drop to learn from this file*", r, 20.0f, Theme::text, juce::Justification::centred);
}

//==================================================================================================
bool LearnTab::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isSupportedAudioFile (f))
            return true;

    return false;
}

void LearnTab::fileDragEnter (const juce::StringArray&, int, int)
{
    fileDragOver = true;
    repaint();
}

void LearnTab::fileDragExit (const juce::StringArray&)
{
    fileDragOver = false;
    repaint();
}

void LearnTab::filesDropped (const juce::StringArray& files, int, int)
{
    fileDragOver = false;
    repaint();

    for (const auto& f : files)
    {
        if (isSupportedAudioFile (f))
        {
            loadFile (juce::File (f));
            return;
        }
    }
}

} // namespace ui
