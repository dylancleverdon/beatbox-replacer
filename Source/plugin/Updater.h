#pragma once

#include <JuceHeader.h>

// Self-updater: checks the GitHub "latest release" of this repo, and installs a newer build in
// place so the only thing left for the user is restarting the DAW.
//
// Release layout (produced by .github/workflows/build.yml):
//   tag      v<major>.<minor>.<patch>      e.g. v1.0.42
//   assets   BeatboxReplacer-Windows.zip   the BeatboxReplacer.vst3 bundle folder at the zip root
//            BeatboxReplacer-Setup.exe     full Windows installer (fallback)
//            BeatboxReplacer-macOS.zip     BeatboxReplacer.vst3 and BeatboxReplacer.component at the zip root
//            BeatboxReplacer-macOS.pkg     full macOS installer (fallback)
//
// In-place install strategy:
//   * Locate installed bundles: the bundle containing the running binary
//     (File::currentExecutableFile -> walk up to the folder ending in ".vst3" / ".component"),
//     plus, on macOS, the other format in /Library/Audio/Plug-Ins/{VST3,Components} and
//     ~/Library/Audio/Plug-Ins/{VST3,Components} if present.
//   * For every file in the new bundle (recursively): write it next to the destination as
//     "<name>.new", then
//       - Windows: if the destination exists and can't be deleted (a loaded DLL is locked),
//         rename it to "<name>.old" (or ".old2", ".old3"... if taken) -- Windows allows renaming
//         a loaded DLL -- then rename "<name>.new" to the destination.
//       - macOS: rename "<name>.new" over the destination (atomic, new inode; never write into
//         the loaded binary in place, that can get the host killed by code signing).
//   * The installers grant the user write access to the bundle folders (Inno Setup
//     "users-modify" on Windows, chown to the console user in the macOS postinstall) so this
//     works without admin rights.
//   * cleanupAfterPreviousUpdate() (called when the plugin loads) deletes leftover "*.old*"
//     and "*.new" files inside the installed bundles.
//   * If the in-place install fails (e.g. permissions), the UI offers runFullInstaller(), which
//     downloads the Setup.exe / .pkg and opens it.
//
// One Updater is shared by all plugin instances in the process (use juce::SharedResourcePointer).
// All public methods are called on the message thread; work happens on a background thread and
// listeners are notified on the message thread via ChangeBroadcaster.
class Updater : public juce::ChangeBroadcaster
{
public:
    enum class Status
    {
        idle,
        checking,
        upToDate,
        updateAvailable,
        downloading,
        installing,
        installedRestartRequired,
        failed
    };

    struct ReleaseInfo
    {
        juce::String version;        // "1.0.42"
        juce::String tag;            // "v1.0.42"
        juce::String notes;          // release body (markdown)
        juce::String htmlUrl;        // release page
        juce::String bundleZipUrl;   // asset for this platform's in-place update
        juce::String installerUrl;   // asset for this platform's full installer
    };

    Updater();
    ~Updater() override;

    // Starts an async check unless one already ran in this process (or force is true).
    void checkForUpdates (bool force = false);

    // Starts the async in-place install of the available release. No-op unless status is
    // updateAvailable (or failed after an available release was found).
    void installUpdate();

    // Downloads the full installer for this platform to the temp folder and launches it.
    void runFullInstaller();

    Status getStatus() const;
    juce::String getStatusText() const;     // human-readable, e.g. "Version 1.0.43 is available"
    float getProgress() const;              // 0..1 while downloading, else 0
    std::optional<ReleaseInfo> getAvailableRelease() const;
    bool installFailedNeedsFullInstaller() const;

    static juce::String getCurrentVersion(); // JucePlugin_VersionString

    // Deletes "*.old*" / "*.new" leftovers inside the installed bundle(s). Safe to call often;
    // never throws; ignores files it can't delete (they may still be loaded).
    static void cleanupAfterPreviousUpdate();

private:
    class Worker;
    std::unique_ptr<Worker> worker;

    mutable juce::CriticalSection lock;
    Status status = Status::idle;
    juce::String statusText;
    std::atomic<float> progress { 0.0f };
    std::optional<ReleaseInfo> available;
    bool needsFullInstaller = false;
    bool hasChecked = false;

    void setStatus (Status s, const juce::String& text);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Updater)
};
