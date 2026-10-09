#include "SetupTab.h"

#include <functional>

namespace ui
{

namespace
{
    constexpr int kGap = 12;
    constexpr int kButtonHeight = 32;
    constexpr int kSubheadHeight = 24;

    const char* const kProfileIntro =
        "Your sounds and training examples are saved with the Live set. Export them to reuse in other sets "
        "(or share them), or save them as the default for new instances of the plugin.";

    const char* const kUpdatesHint =
        "Updates install in place - restart Ableton afterwards to use the new version. If that doesn't work, "
        "Run installer downloads the full installer (close Ableton before running it).";
}

//==================================================================================================
class SetupTab::Content : public juce::Component
{
public:
    Content (BeatboxProcessor& p, Updater& u)
        : processor (p), updater (u)
    {
        setupSteps.setItems ({
            "Put your beatbox on an *audio track* - record it, or monitor your microphone on it.",
            "Load *Beatbox Replacer* on a new *MIDI track*. In the device, turn on *Sidechain* and set "
            "*Audio From* to the beatbox track. The Input meter at the top should move when the beatbox plays.",
            "Make a third *MIDI track* with a *Drum Rack*. Set its *MIDI From* to the Beatbox Replacer track, "
            "the chooser below it to *BeatboxReplacer*, and *Monitor* to *In*.",
            "Arm the drum track and record. Or skip the routing: use *Capture* on the Play tab and drag the clip in."
        });
        addAndMakeVisible (setupSteps);

        notes.setItems ({
            "Live merges MIDI channels on internal routing, so your sounds are told apart by *note number* only. "
            "Give each sound its own note (Kick C1, Snare D1, Closed hat F#1, Open hat A#1).",
            "Needs *Live 10.1 or newer* to receive MIDI from plug-ins; *Live 11 and 12* work the same way. "
            "On older versions, use Capture and drag the clip in.",
            "*On a Mac, load the VST3* - Live can't receive MIDI from Audio Unit plug-ins (dragged clips still work). "
            "Don't see the VST3? Turn on *Use VST3 Plug-In System Folders* in Live's Settings > Plug-Ins."
        });
        addAndMakeVisible (notes);

        troubleshooting.setItems ({
            "*No signal* (the Input meter doesn't move): in this device turn on *Sidechain* and pick your beatbox "
            "track in *Audio From*. Make sure that track is playing or monitoring.",
            "*Wrong sounds*: Learn more examples of the sounds that get mixed up, and teach breaths, clicks and "
            "noises as *Ignore (no note)*.",
            "*Notes late in live mode*: live notes come 'Listen window' ms after each hit. Use *Capture* and drag "
            "the clip for exact timing, or lower the Listen window.",
            "*Nothing arrives on the drum track*: check *MIDI From* (this track, then BeatboxReplacer) and "
            "*Monitor = In*. On a Mac, use the VST3.",
            "*Double notes or missed hits*: raise *Min gap* for doubles; lower *Sensitivity gate* or *Attack rise* "
            "for quiet hits that get missed."
        });
        addAndMakeVisible (troubleshooting);

        exportButton.setTooltip ("Save your sounds and training examples to a file");
        exportButton.onClick = [this] { exportProfile(); };
        addAndMakeVisible (exportButton);

        importButton.setTooltip ("Load sounds and training examples from a file (replaces the current ones)");
        importButton.onClick = [this] { importProfile(); };
        addAndMakeVisible (importButton);

        defaultButton.setTooltip ("New instances of Beatbox Replacer will start with these sounds and examples");
        defaultButton.onClick = [this]
        {
            if (processor.saveDefaultProfile())
                setProfileResult ("Saved. New instances will start with these sounds.", false);
            else
                setProfileResult ("Couldn't save the default profile to "
                                  + BeatboxProcessor::getDefaultProfileFile().getFullPathName(), true);
        };
        addAndMakeVisible (defaultButton);

        checkButton.onClick = [this] { updater.checkForUpdates (true); };
        addAndMakeVisible (checkButton);

        makeAccentButton (installButton);
        installButton.onClick = [this] { updater.installUpdate(); };
        addChildComponent (installButton);

        installerButton.setTooltip ("Downloads the full installer and opens it. Close Ableton before you run it.");
        installerButton.onClick = [this] { updater.runFullInstaller(); };
        addChildComponent (installerButton);

        progressBar.setPercentageDisplay (true);
        addChildComponent (progressBar);

        updaterChanged();
    }

    ~Content() override = default;

    // Lays the content out for a width; returns the height it needs.
    int layout (int width, bool apply)
    {
        const auto leftW = (width - kGap) * 58 / 100;
        const auto rightW = width - kGap - leftW;
        const auto leftInner = leftW - 2 * kPanelPadding;
        const auto rightInner = rightW - 2 * kPanelPadding;

        // Left column: setup + troubleshooting.
        const auto stepsH = setupSteps.getHeightForWidth (leftInner);
        const auto notesH = notes.getHeightForWidth (leftInner);
        const auto troubleH = troubleshooting.getHeightForWidth (leftInner);

        const auto setupH = 2 * kPanelPadding + kPanelTitleHeight + stepsH + 12 + kSubheadHeight + notesH;
        const auto troubleTotalH = 2 * kPanelPadding + kPanelTitleHeight + troubleH;
        const auto leftTotal = setupH + kGap + troubleTotalH;

        // Right column: profile + updates.
        const auto introH = (int) paragraphHeight (kProfileIntro, 13.0f, (float) rightInner);
        const auto profileH = 2 * kPanelPadding + kPanelTitleHeight + introH + 10 + kButtonHeight + 8
                              + kButtonHeight + 8 + 36;
        const auto hintH = (int) paragraphHeight (kUpdatesHint, 12.0f, (float) rightInner);
        const bool showProgress = progressBar.isVisible();
        const bool showInstaller = installerButton.isVisible();
        const auto updatesH = 2 * kPanelPadding + kPanelTitleHeight + 20 + 40 + (showProgress ? 24 : 0) + 8
                              + kButtonHeight + (showInstaller ? 8 + kButtonHeight : 0) + 10 + hintH;
        const auto rightTotal = profileH + kGap + updatesH;

        if (apply)
        {
            setupPanel = { 0, 0, leftW, setupH };
            troublePanel = { 0, setupH + kGap, leftW, troubleTotalH };
            profilePanel = { leftW + kGap, 0, rightW, profileH };
            updatesPanel = { leftW + kGap, profileH + kGap, rightW, updatesH };

            auto s = setupPanel.reduced (kPanelPadding).withTrimmedTop (kPanelTitleHeight);
            setupSteps.setBounds (s.removeFromTop (stepsH));
            s.removeFromTop (12);
            notesHeadArea = s.removeFromTop (kSubheadHeight);
            notes.setBounds (s.removeFromTop (notesH));

            troubleshooting.setBounds (troublePanel.reduced (kPanelPadding).withTrimmedTop (kPanelTitleHeight));

            auto pr = profilePanel.reduced (kPanelPadding).withTrimmedTop (kPanelTitleHeight);
            profileIntroArea = pr.removeFromTop (introH);
            pr.removeFromTop (10);
            auto row = pr.removeFromTop (kButtonHeight);
            exportButton.setBounds (row.removeFromLeft ((row.getWidth() - 8) / 2));
            row.removeFromLeft (8);
            importButton.setBounds (row);
            pr.removeFromTop (8);
            defaultButton.setBounds (pr.removeFromTop (kButtonHeight));
            pr.removeFromTop (8);
            profileResultArea = pr.removeFromTop (36);

            auto u = updatesPanel.reduced (kPanelPadding).withTrimmedTop (kPanelTitleHeight);
            versionArea = u.removeFromTop (20);
            statusArea = u.removeFromTop (40);

            if (showProgress)
                progressBar.setBounds (u.removeFromTop (24).withSizeKeepingCentre (u.getWidth(), 16));

            u.removeFromTop (8);
            auto buttons = u.removeFromTop (kButtonHeight);
            checkButton.setBounds (buttons.removeFromLeft ((buttons.getWidth() - 8) / 2));
            buttons.removeFromLeft (8);
            installButton.setBounds (buttons);

            if (showInstaller)
            {
                u.removeFromTop (8);
                installerButton.setBounds (u.removeFromTop (kButtonHeight));
            }

            u.removeFromTop (10);
            hintArea = u.removeFromTop (hintH);
        }

        return juce::jmax (leftTotal, rightTotal);
    }

    void resized() override
    {
        layout (getWidth(), true);
    }

    void paint (juce::Graphics& g) override
    {
        paintPanel (g, setupPanel, "Set up Ableton", "for live MIDI");
        g.setColour (Theme::text);
        g.setFont (font (14.0f, true));
        g.drawText ("Good to know", notesHeadArea, juce::Justification::centredLeft, false);

        paintPanel (g, troublePanel, "Troubleshooting");

        paintPanel (g, profilePanel, "Profile", "your sounds + training");
        drawParagraph (g, kProfileIntro, profileIntroArea.toFloat(), 13.0f, Theme::textDim);

        if (profileResult.isNotEmpty())
            drawParagraph (g, profileResult, profileResultArea.toFloat(), 13.0f,
                           profileResultIsError ? Theme::danger : Theme::good);

        paintPanel (g, updatesPanel, "Updates");
        g.setColour (Theme::textDim);
        g.setFont (font (13.0f));
        g.drawText ("Installed version: " + Updater::getCurrentVersion(), versionArea, juce::Justification::centredLeft, true);

        drawParagraph (g, statusText, statusArea.toFloat(), 14.0f,
                       statusIsError ? Theme::warning : Theme::text, juce::Justification::centredLeft);
        drawParagraph (g, kUpdatesHint, hintArea.toFloat(), 12.0f, Theme::textFaint);
    }

    void updaterChanged()
    {
        using Status = Updater::Status;

        const auto status = updater.getStatus();
        const auto release = updater.getAvailableRelease();
        const auto version = release.has_value() ? release->version : juce::String();
        const auto busy = status == Status::checking || status == Status::downloading || status == Status::installing;

        juce::String fallback;
        statusIsError = false;

        if (status == Status::idle)
        {
            fallback = "Not checked yet.";
        }
        else if (status == Status::checking)
        {
            fallback = "Checking for updates...";
        }
        else if (status == Status::upToDate)
        {
            fallback = "You're up to date.";
        }
        else if (status == Status::updateAvailable)
        {
            fallback = "Version " + version + " is available.";
        }
        else if (status == Status::downloading)
        {
            fallback = "Downloading version " + version + "...";
        }
        else if (status == Status::installing)
        {
            fallback = "Installing version " + version + "...";
        }
        else if (status == Status::installedRestartRequired)
        {
            fallback = "Update installed - restart Ableton to use it.";
        }
        else
        {
            fallback = updater.installFailedNeedsFullInstaller() ? "Couldn't update in place - use Run installer."
                                                                 : "Couldn't check for updates.";
            statusIsError = true;
        }

        const auto text = updater.getStatusText().trim();
        statusText = text.isNotEmpty() ? text : fallback;

        if (status == Status::failed && updater.installFailedNeedsFullInstaller() && ! statusText.containsIgnoreCase ("installer"))
            statusText << " Use Run installer to update.";

        checkButton.setButtonText (status == Status::checking ? "Checking..." : "Check now");
        checkButton.setEnabled (! busy);

        installButton.setButtonText (status == Status::failed ? "Try again" : "Install update");
        installButton.setVisible (release.has_value()
                                  && (status == Status::updateAvailable
                                      || (status == Status::failed && ! updater.installFailedNeedsFullInstaller())));

        installerButton.setButtonText (version.isNotEmpty() ? "Run installer (" + version + ")" : juce::String ("Run installer"));
        installerButton.setVisible (release.has_value() && ! busy && status != Status::installedRestartRequired);

        progressBar.setVisible (status == Status::downloading || status == Status::installing);
        tick();

        if (onLayoutChange != nullptr)
            onLayoutChange();

        repaint();
    }

    std::function<void()> onLayoutChange;

    void tick()
    {
        if (! progressBar.isVisible())
            return;

        const auto progress = (double) updater.getProgress();
        progressValue = (updater.getStatus() == Updater::Status::downloading && progress > 0.0) ? juce::jmin (1.0, progress) : -1.0;
    }

private:
    BeatboxProcessor& processor;
    Updater& updater;

    StepList setupSteps { true, 14.0f };
    StepList notes { false, 13.0f };
    StepList troubleshooting { false, 13.0f };

    juce::TextButton exportButton { "Export..." };
    juce::TextButton importButton { "Import..." };
    juce::TextButton defaultButton { "Save as default" };
    juce::String profileResult;
    bool profileResultIsError = false;

    juce::TextButton checkButton { "Check now" };
    juce::TextButton installButton { "Install update" };
    juce::TextButton installerButton { "Run installer" };
    double progressValue = -1.0;
    juce::ProgressBar progressBar { progressValue };
    juce::String statusText;
    bool statusIsError = false;

    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> setupPanel, troublePanel, profilePanel, updatesPanel;
    juce::Rectangle<int> notesHeadArea, profileIntroArea, profileResultArea, versionArea, statusArea, hintArea;

    void setProfileResult (const juce::String& text, bool isError)
    {
        profileResult = text;
        profileResultIsError = isError;
        repaint (profileResultArea);
    }

    void exportProfile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Export your sounds and training",
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                           .getChildFile ("Beatbox Replacer profile.xml"),
                                                       "*.xml");

        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safeThis = juce::Component::SafePointer<Content> (this)] (const juce::FileChooser& fc)
        {
            if (safeThis == nullptr)
                return;

            auto file = fc.getResult();

            if (file == juce::File())
                return;

            if (! file.hasFileExtension ("xml"))
                file = file.withFileExtension ("xml");

            if (safeThis->processor.exportProfile (file))
                safeThis->setProfileResult ("Exported to " + file.getFileName() + ".", false);
            else
                safeThis->setProfileResult ("Couldn't write " + file.getFullPathName() + ".", true);
        });
    }

    void importProfile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Import sounds and training",
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                       "*.xml");

        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safeThis = juce::Component::SafePointer<Content> (this)] (const juce::FileChooser& fc)
        {
            if (safeThis == nullptr)
                return;

            const auto file = fc.getResult();

            if (! file.existsAsFile())
                return;

            auto doImport = [safeThis, file]
            {
                if (safeThis == nullptr)
                    return;

                const auto error = safeThis->processor.importProfile (file);

                if (error.isNotEmpty())
                    safeThis->setProfileResult ("Couldn't import " + file.getFileName() + ": " + error, true);
                else
                    safeThis->setProfileResult ("Imported " + file.getFileName() + ": "
                                                + juce::String ((int) safeThis->processor.getSlots().size()) + " sounds, "
                                                + juce::String ((int) safeThis->processor.getTrainingHits().size())
                                                + " examples.", false);
            };

            if (safeThis->processor.getTrainingHits().empty())
            {
                doImport();
                return;
            }

            confirm (*safeThis, "Replace your sounds?",
                     "Importing \"" + file.getFileName() + "\" replaces your current sounds and training examples.",
                     "Import", doImport);
        });
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Content)
};

//==================================================================================================
SetupTab::SetupTab (BeatboxProcessor& p, Updater& u)
    : content (std::make_unique<Content> (p, u))
{
    content->onLayoutChange = [this] { resized(); };
    viewport.setViewedComponent (content.get(), false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);
}

SetupTab::~SetupTab()
{
    viewport.setViewedComponent (nullptr, false);
}

void SetupTab::refresh()
{
    content->repaint();
}

void SetupTab::updaterChanged()
{
    content->updaterChanged();
}

void SetupTab::tick()
{
    content->tick();
}

void SetupTab::resized()
{
    viewport.setBounds (getLocalBounds());

    auto width = viewport.getWidth();
    auto height = content->layout (width, false);

    if (height > viewport.getHeight())
    {
        width -= viewport.getScrollBarThickness() + 4;
        height = content->layout (width, false);
    }

    content->setSize (juce::jmax (0, width), height);
    content->layout (content->getWidth(), true);   // also when the size didn't change
    content->repaint();
}

} // namespace ui
