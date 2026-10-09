#include "PlayTab.h"

#include <cmath>

namespace ui
{

namespace
{
    constexpr double kFlashMs = 150.0;
    constexpr int kClipFromCaptureBar = 1, kClipFromSongStart = 2;

    juce::String plural (int n, const char* singular, const char* pluralForm)
    {
        return juce::String (n) + " " + (n == 1 ? singular : pluralForm);
    }

    juce::String formatElapsed (double seconds)
    {
        const auto total = juce::jmax (0, (int) seconds);
        return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
    }
}

//==================================================================================================
PadRow::PadRow (BeatboxProcessor& p)
    : processor (p)
{
    setTooltip ("Pads light up when a live note is sent (grey = a hit that was ignored). "
                "Click a pad to hear one of its training examples.");
    refresh();
}

void PadRow::refresh()
{
    const auto slots = processor.getSlots();
    const auto sig = slotsSignature (slots);

    if (sig == shownSig && ! pads.empty())
        return;

    shownSig = sig;
    pads.clear();

    for (const auto& slot : slots)
    {
        Pad pad;
        pad.slotId = slot.id;
        pad.name = slot.name;
        pad.note = noteNameOf (slot.note);
        pad.colour = slotColour (slot.id);
        pad.lastCount = processor.getLiveHitCount (slot.id);
        pads.push_back (pad);
    }

    Pad ignore;
    ignore.slotId = bbr::kIgnoreSlotId;
    ignore.name = "Ignored";
    ignore.note = "no note";
    ignore.colour = Theme::unassigned;
    ignore.lastCount = processor.getLiveHitCount (bbr::kIgnoreSlotId);
    pads.push_back (ignore);

    layoutPads();
    repaint();
}

void PadRow::tick()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();

    for (auto& pad : pads)
    {
        const auto count = processor.getLiveHitCount (pad.slotId);

        if (count != pad.lastCount)
        {
            pad.lastCount = count;
            pad.flashStartMs = now;
        }

        const auto level = (float) juce::jlimit (0.0, 1.0, 1.0 - (now - pad.flashStartMs) / kFlashMs);

        if (! juce::approximatelyEqual (level, pad.level))
        {
            pad.level = level;
            repaint (pad.bounds);
        }
    }
}

void PadRow::layoutPads()
{
    const auto count = (int) pads.size();

    if (count == 0)
        return;

    const auto gap = 8;
    const auto width = juce::jmin (120, (getWidth() - gap * (count - 1)) / count);

    for (int i = 0; i < count; ++i)
        pads[(size_t) i].bounds = juce::Rectangle<int> (i * (width + gap), 0, juce::jmax (0, width), getHeight());
}

void PadRow::resized()
{
    layoutPads();
}

void PadRow::paint (juce::Graphics& g)
{
    for (const auto& pad : pads)
    {
        auto r = pad.bounds.toFloat().reduced (0.5f);
        const auto base = pad.colour;

        g.setColour (base.withAlpha (0.14f + 0.76f * pad.level));
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (base.withAlpha (0.55f + 0.45f * pad.level));
        g.drawRoundedRectangle (r, 7.0f, pad.level > 0.0f ? 2.0f : 1.0f);

        auto text = pad.bounds.reduced (8, 6);
        const bool lit = pad.level > 0.5f;
        g.setColour (lit ? Theme::background : Theme::text);
        g.setFont (font (13.0f, true));
        g.drawFittedText (pad.name, text.removeFromTop (text.getHeight() / 2 + 2), juce::Justification::bottomLeft, 1, 0.8f);
        g.setColour (lit ? Theme::background.withAlpha (0.8f) : Theme::textDim);
        g.setFont (font (11.0f));
        g.drawFittedText (pad.note, text, juce::Justification::topLeft, 1, 0.8f);
    }
}

void PadRow::mouseDown (const juce::MouseEvent& e)
{
    for (const auto& pad : pads)
    {
        if (! pad.bounds.contains (e.getPosition()))
            continue;

        const auto& hits = processor.getTrainingHits();
        std::vector<int> indices;

        for (size_t i = 0; i < hits.size(); ++i)
            if (hits[i].slotId == pad.slotId)
                indices.push_back ((int) i);

        if (! indices.empty())
            processor.auditionTrainingHit (indices[(size_t) (auditionCursor++ % (int) indices.size())]);

        return;
    }
}

//==================================================================================================
LabeledSlider::LabeledSlider (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId,
                              const juce::String& displayName, const juce::String& unit, int decimals,
                              const juce::String& tooltip, const juce::String& shortNote)
    : name (displayName), note (shortNote)
{
    setTooltip (tooltip);
    slider.setTooltip (tooltip);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 22);
    addAndMakeVisible (slider);

    if (state.getParameter (parameterId) != nullptr)
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, parameterId, slider);
    else
        slider.setEnabled (false);

    // After the attachment, which installs the parameter's own text functions.
    slider.textFromValueFunction = [unit, decimals] (double value)
    {
        auto text = decimals > 0 ? juce::String (value, decimals) : juce::String (juce::roundToInt (value));
        return unit.isEmpty() ? text : text + " " + unit;
    };
    slider.valueFromTextFunction = [] (const juce::String& text)
    {
        return text.retainCharacters ("-+.0123456789").getDoubleValue();
    };
    slider.updateText();
}

LabeledSlider::~LabeledSlider() = default;

void LabeledSlider::paint (juce::Graphics& g)
{
    auto top = getLocalBounds().removeFromTop (18);
    const auto alpha = isEnabled() ? 1.0f : 0.45f;
    g.setColour (Theme::text.withMultipliedAlpha (alpha));
    g.setFont (font (13.0f, true));
    const auto nameWidth = juce::GlyphArrangement::getStringWidthInt (font (13.0f, true), name) + 10;
    g.drawText (name, top.removeFromLeft (nameWidth), juce::Justification::centredLeft, false);

    if (note.isNotEmpty())
    {
        g.setColour (Theme::textDim.withMultipliedAlpha (alpha));
        g.setFont (font (12.0f));
        g.drawText (note, top, juce::Justification::centredLeft, true);
    }
}

void LabeledSlider::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (18).withSizeKeepingCentre (getWidth(), 26));
}

//==================================================================================================
PlayTab::PlayTab (BeatboxProcessor& p)
    : processor (p),
      pads (p),
      takeView (p),
      dragHandle (p),
      thresholdSlider (p.apvts, ParamIDs::threshold, "Sensitivity gate", "dB", 1,
                       "The quietest level that can count as a hit. Lower it if quiet sounds (like soft hi-hats) "
                       "are missed; raise it if background noise or bleed makes notes."),
      riseSlider (p.apvts, ParamIDs::rise, "Attack rise", "dB", 1,
                  "How sharply the level has to jump to start a new hit. Lower = more sensitive. Raise it if one "
                  "sound triggers twice or a sustained sound keeps re-triggering."),
      gapSlider (p.apvts, ParamIDs::minGap, "Min gap", "ms", 0,
                 "The shortest time allowed between two hits. Raise it if single sounds give double notes; "
                 "lower it for fast rolls."),
      windowSlider (p.apvts, ParamIDs::window, "Listen window", "ms", 0,
                    "How much of each hit the plugin listens to before deciding which sound it is. Longer can be "
                    "more accurate, but live notes are sent this much later. Capture clips are not delayed. "
                    "Changing it re-analyses your training examples.",
                    "also the live delay"),
      noteLengthSlider (p.apvts, ParamIDs::noteLength, "Note length", "ms", 0,
                        "Length of each MIDI note. Drum Racks usually play the whole sample anyway."),
      fixedVelocitySlider (p.apvts, ParamIDs::fixedVelocity, "Fixed velocity", {}, 0,
                           "Velocity of every note when 'Velocity follows loudness' is off.")
{
    // ---- Live MIDI
    liveToggle.setTooltip ("Send a MIDI note for every hit as it happens. Notes arrive 'Listen window' ms after "
                           "each hit. Turn off if you only use Capture.");
    addAndMakeVisible (liveToggle);

    if (processor.apvts.getParameter (ParamIDs::live) != nullptr)
        liveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processor.apvts, ParamIDs::live, liveToggle);

    addAndMakeVisible (liveSteps);
    addAndMakeVisible (pads);

    // ---- Capture
    captureButton.setClickingTogglesState (true);
    captureButton.setColour (juce::TextButton::buttonOnColourId, Theme::recording);
    captureButton.setColour (juce::TextButton::textColourOnId, Theme::text);
    captureButton.setTooltip ("While Capture is on, every time Ableton plays, your hits are recorded with exact "
                              "timing. Stop playback to get the clip. Click again to turn it off.");
    captureButton.onClick = [this]
    {
        processor.setCaptureArmed (captureButton.getToggleState());
        refresh();
    };
    addAndMakeVisible (captureButton);

    convertButton.setTooltip ("Turn a beatbox recording (WAV, AIFF, FLAC, OGG, MP3) straight into a clip. "
                              "The clip starts at the start of the file.");
    convertButton.onClick = [this] { convertClicked(); };
    addAndMakeVisible (convertButton);

    saveButton.setTooltip ("Save the clip as a .mid file - handy if dragging doesn't work in your setup.");
    saveButton.onClick = [this] { saveClicked(); };
    addAndMakeVisible (saveButton);

    clearButton.setTooltip ("Throw the current clip away");
    clearButton.onClick = [this]
    {
        processor.clearTake();
        refresh();
    };
    addAndMakeVisible (clearButton);

    clipStartBox.setTooltip ("Where the clip begins. 'Capture bar' gives a short clip to drop at the bar where "
                             "you started capturing; 'Song start' gives a clip to drop at bar 1.");
    clipStartBox.onChange = [this]
    {
        fromSongStart = clipStartBox.getSelectedId() == kClipFromSongStart;
        dragHandle.setFromSongStart (fromSongStart);
    };
    addAndMakeVisible (clipStartBox);

    addAndMakeVisible (takeView);
    addAndMakeVisible (dragHandle);

    // ---- Detection
    for (auto* s : { &thresholdSlider, &riseSlider, &gapSlider, &windowSlider, &noteLengthSlider, &fixedVelocitySlider })
        addAndMakeVisible (s);

    dynamicToggle.setTooltip ("On: louder hits give higher velocities. Off: every note uses Fixed velocity.");
    addAndMakeVisible (dynamicToggle);

    if (processor.apvts.getParameter (ParamIDs::dynamic) != nullptr)
        dynamicAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processor.apvts, ParamIDs::dynamic, dynamicToggle);

    updateLiveSteps();
    refresh();
}

PlayTab::~PlayTab() = default;

//==================================================================================================
void PlayTab::refresh()
{
    pads.refresh();
    captureButton.setToggleState (processor.isCaptureArmed(), juce::dontSendNotification);

    const bool hasTake = processor.hasTake();
    saveButton.setEnabled (hasTake);
    clearButton.setEnabled (hasTake);
    clipStartBox.setEnabled (hasTake);

    updateClipStartItems();
    dragHandle.refresh();
    takeView.refresh();
    updateCaptureStatus();
}

void PlayTab::tick()
{
    pads.tick();
    updateCaptureStatus();
    updateLiveSteps();
    fixedVelocitySlider.setEnabled (! dynamicToggle.getToggleState());
}

void PlayTab::updateLiveSteps()
{
    int windowMs = 20;

    if (auto* value = processor.apvts.getRawParameterValue (ParamIDs::window))
        windowMs = juce::roundToInt (value->load());

    if (windowMs == shownWindowMs)
        return;

    shownWindowMs = windowMs;
    liveSteps.setItems ({
        "On this device, open *Sidechain* (title bar) and set *Audio From* to your beatbox track.",
        "On a MIDI track with your Drum Rack, set *MIDI From* to this track, choose *BeatboxReplacer* below it, "
        "and set *Monitor* to *In*.",
        "Arm that track to record. Each note arrives *" + juce::String (windowMs)
            + " ms* after its hit - use Capture below for exact timing."
    });

    resized();
}

void PlayTab::updateClipStartItems()
{
    const auto dropBar = processor.hasTake() ? processor.getTakeDropBar (false) : -1;

    if (dropBar == shownDropBar && clipStartBox.getNumItems() > 0)
        return;

    shownDropBar = dropBar;
    clipStartBox.clear (juce::dontSendNotification);
    clipStartBox.addItem (dropBar > 0 ? "Capture bar (drop at bar " + juce::String (dropBar) + ")"
                                      : juce::String ("Capture bar"),
                          kClipFromCaptureBar);
    clipStartBox.addItem ("Song start (drop at bar 1)", kClipFromSongStart);
    clipStartBox.setSelectedId (fromSongStart ? kClipFromSongStart : kClipFromCaptureBar, juce::dontSendNotification);
}

int PlayTab::countTakeNotes() const
{
    const auto& take = processor.getLastTake();
    int notes = 0;

    for (const auto& hit : take.hits)
        if (hit.slotId >= 0 && processor.hasSlot (hit.slotId))
            ++notes;

    return notes;
}

void PlayTab::showNote (const juce::String& text)
{
    transientNote = text;
    transientUntilMs = juce::Time::getMillisecondCounterHiRes() + 6000.0;
    updateCaptureStatus();
}

void PlayTab::updateCaptureStatus()
{
    const bool armed = processor.isCaptureArmed();
    const bool recording = processor.isCaptureRecording();
    const bool hasTake = processor.hasTake();
    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (recording && ! wasRecording)
        recordingStartMs = now;

    const bool justFinished = wasRecording && ! recording;
    wasRecording = recording;

    if (justFinished)
    {
        // The take arrives with the processor's change message; make sure the view follows.
        dragHandle.refresh();
        takeView.refresh();
    }

    juce::String text;

    if (recording)
        text = "Recording... " + formatElapsed ((now - recordingStartMs) / 1000.0) + " - stop playback to finish the take";
    else if (hasTake)
        text = "Take ready: " + plural (countTakeNotes(), "note", "notes") + " from bar "
               + juce::String (processor.getTakeDropBar (false))
               + (armed ? ". Still armed - play again to record a new take." : ". Drag it into Ableton below.");
    else if (armed)
        text = "Armed - press play in Ableton";
    else
        text = "Turn on Capture, then play your beatbox section in Ableton. Stop playback to get the clip.";

    if (transientNote.isNotEmpty())
    {
        if (now < transientUntilMs && ! recording)
            text = transientNote;
        else
            transientNote.clear();
    }

    const auto buttonText = recording ? "Capturing..." : (armed ? "Capture armed" : "Capture");

    if (captureButton.getButtonText() != buttonText)
        captureButton.setButtonText (buttonText);

    if (captureButton.getToggleState() != armed)
        captureButton.setToggleState (armed, juce::dontSendNotification);

    takeView.setPlaceholder (recording ? "Recording... the clip shows up here when you stop playback."
                                       : (armed ? "Armed. Press play in Ableton and beatbox (or play your beatbox track)."
                                                : "No clip yet. Turn on *Capture* and play your beatbox section in Ableton, "
                                                  "or convert an audio file."),
                             recording);

    if (text != captureStatus)
    {
        captureStatus = text;
        repaint (captureStatusArea);
    }
}

//==================================================================================================
void PlayTab::saveClicked()
{
    if (! processor.hasTake())
        return;

    const auto defaultName = "Beatbox " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M") + ".mid";
    chooser = std::make_unique<juce::FileChooser> ("Save the MIDI clip",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile (defaultName),
                                                   "*.mid");

    const auto startAtSongStart = fromSongStart;
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safeThis = juce::Component::SafePointer<PlayTab> (this), startAtSongStart] (const juce::FileChooser& fc)
    {
        if (safeThis == nullptr)
            return;

        auto file = fc.getResult();

        if (file == juce::File())
            return;

        if (! file.hasFileExtension ("mid;midi"))
            file = file.withFileExtension ("mid");

        if (safeThis->processor.writeTakeToMidiFile (file, startAtSongStart))
            safeThis->showNote ("Saved " + file.getFileName() + " - drop it at bar "
                                + juce::String (safeThis->processor.getTakeDropBar (startAtSongStart)) + ".");
        else
            showMessage (*safeThis, "Couldn't save the clip",
                         "Couldn't write " + file.getFullPathName() + ". Try another folder.", true);
    });
}

void PlayTab::convertClicked()
{
    chooser = std::make_unique<juce::FileChooser> ("Convert a beatbox recording to a MIDI clip",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safeThis = juce::Component::SafePointer<PlayTab> (this)] (const juce::FileChooser& fc)
    {
        if (safeThis == nullptr)
            return;

        const auto file = fc.getResult();

        if (! file.existsAsFile())
            return;

        if (safeThis->processor.hasTake())
        {
            confirm (*safeThis, "Replace the current clip?",
                     "Converting \"" + file.getFileName() + "\" replaces the clip you captured.", "Replace",
                     [safeThis, file]
                     {
                         if (safeThis != nullptr)
                             safeThis->convertFile (file);
                     });
            return;
        }

        safeThis->convertFile (file);
    });
}

void PlayTab::convertFile (const juce::File& file)
{
    juce::MouseCursor::showWaitCursor();
    const auto error = processor.captureFromFile (file);
    juce::MouseCursor::hideWaitCursor();

    if (error.isNotEmpty())
    {
        showMessage (*this, "Couldn't convert " + file.getFileName(), error, true);
    }
    else
    {
        if (fromSongStart)
        {
            fromSongStart = false;
            dragHandle.setFromSongStart (false);
        }

        showNote ("Converted " + file.getFileName() + ": " + plural (countTakeNotes(), "note", "notes")
                  + ". The clip starts at the start of the file.");
    }

    refresh();
}

//==================================================================================================
void PlayTab::resized()
{
    auto r = getLocalBounds();
    detectionPanel = r.removeFromRight (juce::jlimit (260, 320, r.getWidth() * 30 / 100));
    r.removeFromRight (12);

    // Live MIDI
    const auto liveInnerWidth = r.getWidth() - 2 * kPanelPadding;
    const auto stepsHeight = liveSteps.getHeightForWidth (liveInnerWidth);
    livePanel = r.removeFromTop (2 * kPanelPadding + kPanelTitleHeight + stepsHeight + 10 + 46);
    r.removeFromTop (12);
    capturePanel = r;

    {
        auto l = livePanel.reduced (kPanelPadding);
        auto title = l.removeFromTop (kPanelTitleHeight - 4);
        liveToggle.setBounds (title.removeFromRight (juce::jmin (170, title.getWidth() / 2)));
        l.removeFromTop (4);
        liveSteps.setBounds (l.removeFromTop (stepsHeight));
        l.removeFromTop (10);
        pads.setBounds (l.removeFromTop (46));
    }

    // Capture
    {
        auto c = capturePanel.reduced (kPanelPadding);
        auto title = c.removeFromTop (kPanelTitleHeight - 2);
        convertButton.setBounds (title.removeFromRight (juce::jmin (170, title.getWidth() / 3)));
        title.removeFromRight (8);
        clearButton.setBounds (title.removeFromRight (64));
        title.removeFromRight (8);
        saveButton.setBounds (title.removeFromRight (108));
        c.removeFromTop (8);

        auto row1 = c.removeFromTop (36);
        captureButton.setBounds (row1.removeFromLeft (150).withSizeKeepingCentre (150, 32));
        row1.removeFromLeft (12);
        captureStatusArea = row1;
        c.removeFromTop (8);

        auto row3 = c.removeFromBottom (40);
        const auto comboWidth = juce::jmin (230, row3.getWidth() * 42 / 100);
        clipStartBox.setBounds (row3.removeFromRight (comboWidth).withSizeKeepingCentre (comboWidth, 30));
        clipStartLabelArea = row3.removeFromRight (96);
        row3.removeFromRight (4);
        dragHandle.setBounds (row3);
        c.removeFromBottom (8);
        takeView.setBounds (c);
    }

    // Detection
    {
        auto d = detectionPanel.reduced (kPanelPadding);
        d.removeFromTop (kPanelTitleHeight + 2);

        for (auto* s : { &thresholdSlider, &riseSlider, &gapSlider, &windowSlider, &noteLengthSlider })
        {
            s->setBounds (d.removeFromTop (LabeledSlider::kHeight));
            d.removeFromTop (6);
        }

        d.removeFromTop (2);
        dynamicToggle.setBounds (d.removeFromTop (28));
        d.removeFromTop (6);
        fixedVelocitySlider.setBounds (d.removeFromTop (LabeledSlider::kHeight));
        d.removeFromTop (8);
        detectionHintArea = d;
    }
}

void PlayTab::paint (juce::Graphics& g)
{
    paintPanel (g, livePanel, "Live MIDI", "notes as you beatbox");

    paintPanel (g, capturePanel, "Capture a clip", {});
    drawParagraph (g, captureStatus, captureStatusArea.toFloat(), 13.0f,
                   processor.isCaptureRecording() ? Theme::text : Theme::textDim,
                   juce::Justification::centredLeft);

    g.setColour (Theme::textDim);
    g.setFont (font (13.0f));
    g.drawText ("Clip starts at", clipStartLabelArea, juce::Justification::centredRight, false);

    paintPanel (g, detectionPanel, "Detection", {});

    if (detectionHintArea.getHeight() > 30)
        drawParagraph (g, "Hover over a setting to see what it does. Double-click a slider to reset it.",
                       detectionHintArea.toFloat(), 12.0f, Theme::textFaint);
}

} // namespace ui
