#include "HeaderBar.h"

namespace ui
{

namespace
{
    bool isAuInLive (const BeatboxProcessor& p)
    {
        return juce::PluginHostType().isAbletonLive()
               && p.wrapperType == juce::AudioProcessor::wrapperType_AudioUnit;
    }

    void paintBanner (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour colour)
    {
        auto r = area.reduced (12, 4).toFloat();
        g.setColour (colour.withAlpha (0.14f));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (colour);
        g.fillRoundedRectangle (r.removeFromLeft (4.0f), 2.0f);
    }
}

HeaderBar::HeaderBar (BeatboxProcessor& p, Updater& u)
    : processor (p), updater (u), showAuWarning (isAuInLive (p))
{
    makeAccentButton (installButton);
    installButton.onClick = [this] { updater.installUpdate(); };
    addChildComponent (installButton);

    notesButton.onClick = [this]
    {
        if (const auto release = updater.getAvailableRelease())
            showMessage (*this, "What's new in version " + release->version, release->notes.trim());
    };
    addChildComponent (notesButton);

    installerButton.setTooltip ("Downloads the full installer and opens it. Close Ableton before you run it.");
    installerButton.onClick = [this] { updater.runFullInstaller(); };
    addChildComponent (installerButton);

    dismissButton.setTooltip ("Hide this message");
    dismissButton.setIconColour (Theme::textDim);
    dismissButton.onClick = [this]
    {
        dismissedKey = currentKey;
        updaterChanged();
    };
    addChildComponent (dismissButton);

    progressBar.setPercentageDisplay (true);
    addChildComponent (progressBar);

    updaterChanged();
    tick();
}

HeaderBar::~HeaderBar() = default;

int HeaderBar::getPreferredHeight() const
{
    return kMainHeight + (showAuWarning ? kBannerHeight : 0) + (updateBannerVisible ? kBannerHeight : 0);
}

void HeaderBar::tick()
{
    const auto db = juce::jlimit (-100.0f, 6.0f, processor.getInputLevelDb());
    const auto isReceiving = processor.isReceivingAudio();
    const auto isPlaying = processor.isHostPlaying();

    if (std::abs (db - meterDb) > 0.2f || isReceiving != receiving)
    {
        meterDb = db;
        receiving = isReceiving;
        repaint (meterArea.getUnion (statusArea));
    }

    if (isPlaying != playing)
    {
        playing = isPlaying;
        repaint (transportArea);
    }

    if (progressBar.isVisible())
    {
        const auto status = updater.getStatus();
        const auto progress = (double) updater.getProgress();
        progressValue = (status == Updater::Status::downloading && progress > 0.0) ? juce::jmin (1.0, progress) : -1.0;
    }
}

void HeaderBar::updaterChanged()
{
    using Status = Updater::Status;

    const auto status = updater.getStatus();
    const auto release = updater.getAvailableRelease();
    const auto version = release.has_value() ? release->version : juce::String();

    bool visible = false, showInstall = false, showNotes = false, showInstaller = false, showProgress = false, canDismiss = false;
    bannerIsError = false;
    juce::String text;

    if (status == Status::updateAvailable && release.has_value())
    {
        visible = showInstall = canDismiss = true;
        showNotes = release->notes.trim().isNotEmpty();
        text = "Version " + version + " is available";
        installButton.setButtonText ("Install update");
        installButton.setTooltip ("Downloads and installs the update. Restart Ableton afterwards to use it.");

        auto notes = release->notes.trim();

        if (notes.length() > 700)
            notes = notes.substring (0, 700).trimEnd() + "...";

        notesButton.setTooltip (notes);
    }
    else if (status == Status::downloading || status == Status::installing)
    {
        visible = showProgress = true;
        text = (status == Status::downloading ? "Downloading version " : "Installing version ") + version + "...";
    }
    else if (status == Status::installedRestartRequired)
    {
        visible = canDismiss = true;
        text = "Update installed - restart Ableton to use it";
    }
    else if (status == Status::failed && release.has_value())
    {
        visible = canDismiss = showInstaller = true;
        bannerIsError = true;

        if (updater.installFailedNeedsFullInstaller())
        {
            text = "Couldn't update in place";
        }
        else
        {
            showInstall = true;
            installButton.setButtonText ("Try again");
            installButton.setTooltip ({});
            text = "Update failed";

            const auto detail = updater.getStatusText().trim();

            if (detail.isNotEmpty())
                text << ": " << detail;
        }
    }

    currentKey = juce::String ((int) status) + "|" + version;

    if (visible && canDismiss && currentKey == dismissedKey)
        visible = false;

    updateText = text;
    installButton.setVisible (visible && showInstall);
    notesButton.setVisible (visible && showNotes);
    installerButton.setVisible (visible && showInstaller);
    progressBar.setVisible (visible && showProgress);
    dismissButton.setVisible (visible && canDismiss);

    if (showProgress)
        tick();

    const bool layoutChanged = visible != updateBannerVisible;
    updateBannerVisible = visible;

    resized();
    repaint();

    if (layoutChanged && onLayoutChanged != nullptr)
        onLayoutChanged();
}

void HeaderBar::resized()
{
    auto r = getLocalBounds();
    mainRow = r.removeFromTop (kMainHeight);
    auBannerArea = showAuWarning ? r.removeFromTop (kBannerHeight) : juce::Rectangle<int>();
    updateBannerArea = updateBannerVisible ? r.removeFromTop (kBannerHeight) : juce::Rectangle<int>();

    auto m = mainRow.reduced (16, 8);
    titleArea = m.removeFromLeft (212);
    transportArea = m.removeFromRight (92);
    m.removeFromRight (12);
    meterArea = m.removeFromLeft (juce::jmin (190, m.getWidth() / 2));
    m.removeFromLeft (14);
    statusArea = m;

    auto b = updateBannerArea.reduced (24, 8);

    if (dismissButton.isVisible())
    {
        dismissButton.setBounds (b.removeFromRight (22).withSizeKeepingCentre (22, 22));
        b.removeFromRight (8);
    }

    auto place = [&b] (juce::Component& c, int width)
    {
        if (! c.isVisible())
            return;

        c.setBounds (b.removeFromRight (juce::jmin (width, juce::jmax (0, b.getWidth() / 2))));
        b.removeFromRight (8);
    };

    place (installerButton, 120);
    place (installButton, 124);
    place (notesButton, 104);

    if (progressBar.isVisible())
    {
        progressBar.setBounds (b.removeFromRight (juce::jmin (220, b.getWidth() / 2)).withSizeKeepingCentre (juce::jmin (220, b.getWidth() / 2), 16));
        b.removeFromRight (12);
    }

    updateTextArea = b.withTrimmedLeft (4);
}

void HeaderBar::paint (juce::Graphics& g)
{
    g.setColour (Theme::panel);
    g.fillRect (mainRow);
    g.setColour (Theme::outline);
    g.fillRect (mainRow.removeFromBottom (1));

    // Logo + name + version.
    {
        auto t = titleArea;
        auto logo = t.removeFromLeft (34).withSizeKeepingCentre (30, 30).toFloat();
        g.setColour (Theme::accent);
        g.fillRoundedRectangle (logo, 8.0f);
        g.setColour (Theme::background);
        const float barW = 4.0f;
        const float heights[] = { 0.35f, 0.7f, 0.5f, 0.25f };

        for (int i = 0; i < 4; ++i)
        {
            const auto h = logo.getHeight() * heights[i];
            g.fillRoundedRectangle (logo.getX() + 5.0f + (float) i * 5.5f, logo.getCentreY() - h * 0.5f, barW, h, 1.5f);
        }

        t.removeFromLeft (10);
        g.setColour (Theme::text);
        g.setFont (font (19.0f, true));
        g.drawText ("Beatbox Replacer", t.removeFromTop (t.getHeight() * 3 / 5), juce::Justification::bottomLeft, true);
        g.setColour (Theme::textFaint);
        g.setFont (font (12.0f));
        g.drawText ("v" + Updater::getCurrentVersion(), t, juce::Justification::topLeft, true);
    }

    // Input meter.
    {
        auto m = meterArea;
        g.setFont (font (12.0f));
        g.setColour (Theme::textDim);
        g.drawText ("Input", m.removeFromLeft (38), juce::Justification::centredLeft, false);

        auto dbText = m.removeFromRight (52);
        auto bar = m.withSizeKeepingCentre (m.getWidth(), 8).toFloat();
        g.setColour (Theme::outline);
        g.fillRoundedRectangle (bar, 4.0f);

        const auto level = juce::jlimit (0.0f, 1.0f, (meterDb + 60.0f) / 60.0f);

        if (level > 0.0f)
        {
            const auto colour = meterDb > -1.0f ? Theme::danger : (meterDb > -9.0f ? Theme::warning : Theme::good);
            g.setColour (colour);
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (8.0f, bar.getWidth() * level)), 4.0f);
        }

        g.setColour (Theme::textDim);
        g.drawText (meterDb <= -99.0f ? juce::String ("-inf dB") : juce::String (juce::roundToInt (meterDb)) + " dB",
                    dbText, juce::Justification::centredRight, false);
    }

    // Status.
    {
        auto s = statusArea;
        auto dot = s.removeFromLeft (14).withSizeKeepingCentre (8, 8).toFloat();
        g.setColour (receiving ? Theme::good : Theme::warning);
        g.fillEllipse (dot);
        s.removeFromLeft (4);
        g.setFont (font (13.0f, receiving));
        g.setColour (receiving ? Theme::text : Theme::warning);
        g.drawFittedText (receiving ? juce::String ("Hearing your beatbox")
                                    : juce::String ("No sidechain signal - open this device's Sidechain section and choose your beatbox track"),
                          s, juce::Justification::centredLeft, 2, 0.85f);
    }

    // Transport.
    {
        auto t = transportArea;
        auto dot = t.removeFromLeft (16).withSizeKeepingCentre (10, 10).toFloat();

        if (playing)
        {
            g.setColour (Theme::good);
            g.fillEllipse (dot);
        }
        else
        {
            g.setColour (Theme::textFaint);
            g.drawEllipse (dot.reduced (0.75f), 1.5f);
        }

        t.removeFromLeft (4);
        g.setColour (playing ? Theme::text : Theme::textDim);
        g.setFont (font (13.0f));
        g.drawText (playing ? "Playing" : "Stopped", t, juce::Justification::centredLeft, false);
    }

    if (! auBannerArea.isEmpty())
    {
        paintBanner (g, auBannerArea, Theme::warning);
        g.setColour (Theme::text);
        g.setFont (font (13.0f));
        g.drawFittedText ("Live can't receive MIDI from Audio Unit plug-ins. Load the VST3 version instead "
                          "(drag-out clips still work).",
                          auBannerArea.reduced (28, 4), juce::Justification::centredLeft, 2, 0.85f);
    }

    if (! updateBannerArea.isEmpty())
    {
        paintBanner (g, updateBannerArea, bannerIsError ? Theme::warning : Theme::accent);
        g.setColour (Theme::text);
        g.setFont (font (13.0f, true));
        g.drawFittedText (updateText, updateTextArea, juce::Justification::centredLeft, 2, 0.85f);
    }
}

} // namespace ui
