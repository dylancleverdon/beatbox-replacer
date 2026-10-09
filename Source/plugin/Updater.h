#pragma once

#include <JuceHeader.h>

// Self-updater: checks the GitHub "latest release" of this repo, and installs a newer build in
// place so the only thing left for the user is restarting the DAW.
//
// Release layout (produced by .github/workflows/build.yml; names never change between releases):
//   tag      v<major>.<minor>.<patch>      e.g. v1.0.42
//   assets   latest.json                   manifest, see below
//            BeatboxReplacer-Windows.zip   "BeatboxReplacer.vst3" bundle folder at the zip root
//            BeatboxReplacer-Setup.exe     full Windows installer (first install / fallback)
//            BeatboxReplacer-macOS.zip     "BeatboxReplacer.vst3" and "BeatboxReplacer.component"
//                                          at the zip root (made with ditto -c -k --keepParent,
//                                          so unix modes are stored)
//            BeatboxReplacer-macOS.pkg     full macOS installer (first install / fallback)
//
// latest.json:
//   { "version": "1.0.42", "tag": "v1.0.42", "notes": "...",
//     "windows": { "zip": "BeatboxReplacer-Windows.zip", "sha256": "<hex>", "installer": "BeatboxReplacer-Setup.exe" },
//     "macos":   { "zip": "BeatboxReplacer-macOS.zip",   "sha256": "<hex>", "installer": "BeatboxReplacer-macOS.pkg" } }
//
// Checking: GET https://github.com/<owner>/<repo>/releases/latest/download/latest.json (follows
// redirects, costs no GitHub API rate limit). If that fails, fall back to the REST API
// https://api.github.com/repos/<owner>/<repo>/releases/latest (needs a User-Agent header; 60
// requests/hour unauthenticated) and use asset names from there (no sha256 then). Asset URLs are
// https://github.com/<owner>/<repo>/releases/download/<tag>/<name>. Always send
// "User-Agent: BeatboxReplacer/<version>". Never send an Authorization header on asset
// downloads (redirect targets are a CDN); BBR_GITHUB_TOKEN, if non-empty, is only sent to
// api.github.com. Treat a response as success only when the stream exists and status == 200.
// All network I/O happens on a background juce::Thread.
//
// Installing in place (after downloading the zip to a temp file and verifying its SHA-256 when
// the manifest has one):
//   * Windows. bundle = currentExecutableFile (the DLL ...\BeatboxReplacer.vst3\Contents\
//     x86_64-win\BeatboxReplacer.vst3) -> up three levels. Extract the zip to %TEMP%. For every
//     file of the new bundle: COPY it to "<dest>.new" inside the installed bundle (a new file
//     created there inherits the folder's ACL; never move files in from %TEMP%), then
//       - if dest exists: try to delete it; if that fails (the loaded DLL is locked), rename it to
//         "<dest>.old-<unix time>" (Windows allows renaming a loaded DLL, not deleting or
//         overwriting it);
//       - rename "<dest>.new" to dest; on failure rename the .old file back.
//     Retry each rename ~5 times with 200 ms sleeps (antivirus / indexer). Leftover names must
//     never end in ".vst3" (hosts scan for that). Never rename the bundle folder itself.
//   * macOS. bundle = currentApplicationFile (".../BeatboxReplacer.vst3" or ".component").
//     Extract with ChildProcess "/usr/bin/ditto -x -k <zip> <staging>" (keeps modes/symlinks)
//     into a staging folder on the same volume, strip quarantine (xattr -dr com.apple.quarantine),
//     verify with "/usr/bin/codesign --verify --deep --strict <staged bundle>" and refuse to
//     install if it fails. Then for each installed bundle (the running one, plus the other format
//     in /Library/Audio/Plug-Ins/{VST3,Components} or ~/Library/... if present): stage the new
//     bundle's "Contents" as "<bundle>/Contents.new", rename "<bundle>/Contents" to
//     "<bundle>/Contents.old-<time>", rename "Contents.new" to "Contents", then delete the old
//     tree (unlinking is safe while the host has the old binary mapped). Never write into the
//     loaded binary in place. The pkg postinstall chowns the bundles to the console user, so
//     this needs no admin rights; the parents (/Library/Audio/Plug-Ins/...) stay root-owned,
//     which is why the swap happens one level down.
//   * Before offering in-place install, check write access (Windows: create and delete a probe
//     file in Contents\x86_64-win; macOS: File::hasWriteAccess on the bundle). If not writable,
//     or if anything fails, set needsFullInstaller so the UI offers runFullInstaller(), which
//     downloads Setup.exe / the .pkg to temp and opens it (Windows: File::startAsProcess, which
//     shows the UAC prompt; macOS: open with Installer).
//   * cleanupAfterPreviousUpdate() (called when the plugin loads) deletes leftover "*.old-*",
//     "*.new", "Contents.old-*" and "Contents.new" items inside the installed bundle(s),
//     ignoring failures (another host process may still map an old file).
//   * After a successful install the status is installedRestartRequired; the UI tells the user
//     to restart the DAW. The running process keeps using the old code until then.
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
