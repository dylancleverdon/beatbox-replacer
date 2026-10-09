#include "Updater.h"

#include "../core/Version.h"

#include <optional>
#include <utility>
#include <vector>

namespace
{

constexpr int kConnectTimeoutMs = 15000;
constexpr int kMaxRedirects = 10;
constexpr int kStopTimeoutMs = 15000;           // downloads abort at once; a started file swap finishes
constexpr int kRenameAttempts = 5;
constexpr int kRenameRetryDelayMs = 200;
constexpr int kReadChunkBytes = 64 * 1024;
constexpr int kToolTimeoutMs = 120000;
constexpr juce::int64 kMaxJsonBytes = 4 * 1024 * 1024;
constexpr juce::int64 kMaxDownloadBytes = (juce::int64) 1024 * 1024 * 1024;
constexpr juce::int64 kCheckCacheMs = (juce::int64) 12 * 60 * 60 * 1000;
constexpr juce::int64 kStaleWorkFolderMs = (juce::int64) 24 * 60 * 60 * 1000;

const char* const kVst3BundleName = "BeatboxReplacer.vst3";
const char* const kAuBundleName = "BeatboxReplacer.component";
const char* const kProbePrefix = "bbr-write-probe";

// This platform's entry in latest.json and its release asset names (see Updater.h).
struct PlatformAssets
{
    const char* manifestKey;
    const char* zip;
    const char* installer;
};

#if JUCE_WINDOWS
constexpr PlatformAssets kPlatform { "windows", "BeatboxReplacer-Windows.zip", "BeatboxReplacer-Setup.exe" };
#elif JUCE_MAC
constexpr PlatformAssets kPlatform { "macos", "BeatboxReplacer-macOS.zip", "BeatboxReplacer-macOS.pkg" };
#else
constexpr PlatformAssets kPlatform { "", "", "" };
#endif

//==================================================================================================
juce::String getReleasesUrl()
{
    return juce::String ("https://github.com/") + BBR_GITHUB_OWNER + "/" + BBR_GITHUB_REPO + "/releases";
}

juce::String getAssetUrl (const juce::String& tag, const juce::String& assetName)
{
    return getReleasesUrl() + "/download/" + tag + "/" + assetName;
}

juce::String getApiLatestReleaseUrl()
{
    return juce::String ("https://api.github.com/repos/") + BBR_GITHUB_OWNER + "/" + BBR_GITHUB_REPO + "/releases/latest";
}

juce::String createRequestHeaders (bool forGitHubApi)
{
    juce::String headers ("User-Agent: BeatboxReplacer/" + Updater::getCurrentVersion());

    if (forGitHubApi)
    {
        headers << "\r\nAccept: application/vnd.github+json"
                << "\r\nX-GitHub-Api-Version: 2022-11-28";

        // Only ever sent to api.github.com: asset downloads redirect to a CDN that rejects it.
        const juce::String token (BBR_GITHUB_TOKEN);

        if (token.isNotEmpty())
            headers << "\r\nAuthorization: Bearer " << token;
    }

    return headers;
}

// Tags and asset names end up in URLs and file names, so only plain characters are accepted.
bool isPlainName (const juce::String& name)
{
    return name.isNotEmpty()
        && name.length() <= 128
        && ! name.startsWithChar ('.')
        && name.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_+");
}

// "v1.0.42" / "1.0.42" -> "1.0.42"; {} if the text doesn't start with a version number.
juce::String normaliseVersion (const juce::String& text)
{
    if (const auto version = bbr::Version::parse (text.toStdString()))
        return juce::String (version->toString());

    return {};
}

bool isBusy (Updater::Status s)
{
    return s == Updater::Status::checking || s == Updater::Status::downloading || s == Updater::Status::installing;
}

juce::String getRestartText (const juce::String& version)
{
    return "Update installed - restart Ableton to use version " + version;
}

//==================================================================================================
bool parseManifest (const juce::String& text, Updater::ReleaseInfo& release, juce::String& sha256)
{
    const auto json = juce::JSON::parse (text);

    if (! json.isObject())
        return false;

    auto tag = json.getProperty ("tag", {}).toString().trim();
    auto version = normaliseVersion (json.getProperty ("version", {}).toString());

    if (version.isEmpty())
        version = normaliseVersion (tag);

    if (tag.isEmpty())
        tag = "v" + version;

    if (version.isEmpty() || ! isPlainName (tag))
        return false;

    release = {};
    release.version = version;
    release.tag = tag;
    release.notes = json.getProperty ("notes", {}).toString();
    release.htmlUrl = getReleasesUrl() + "/tag/" + tag;
    sha256.clear();

    const juce::String platformKey (kPlatform.manifestKey);

    if (platformKey.isNotEmpty())
    {
        const auto assets = json.getProperty (juce::Identifier (platformKey), {});
        const auto zipName = assets.getProperty ("zip", {}).toString().trim();
        const auto installerName = assets.getProperty ("installer", {}).toString().trim();

        if (isPlainName (zipName))
        {
            release.bundleZipUrl = getAssetUrl (tag, zipName);
            sha256 = assets.getProperty ("sha256", {}).toString().trim().toLowerCase();
        }

        if (isPlainName (installerName))
            release.installerUrl = getAssetUrl (tag, installerName);
    }

    return true;
}

// Fallback: the REST API's /releases/latest (no checksums there).
bool parseApiRelease (const juce::String& text, Updater::ReleaseInfo& release)
{
    const auto json = juce::JSON::parse (text);

    if (! json.isObject())
        return false;

    const auto tag = json.getProperty ("tag_name", {}).toString().trim();
    const auto version = normaliseVersion (tag);

    if (version.isEmpty() || ! isPlainName (tag))
        return false;

    release = {};
    release.version = version;
    release.tag = tag;
    release.notes = json.getProperty ("body", {}).toString();
    release.htmlUrl = json.getProperty ("html_url", {}).toString();

    if (! release.htmlUrl.startsWith ("https://github.com/"))
        release.htmlUrl = getReleasesUrl() + "/tag/" + tag;

    const juce::String zipName (kPlatform.zip), installerName (kPlatform.installer);
    const auto assets = json.getProperty ("assets", {});

    if (const auto* list = assets.getArray())
    {
        for (const auto& asset : *list)
        {
            const auto assetName = asset.getProperty ("name", {}).toString();

            if (assetName.isEmpty())
                continue;

            if (assetName == zipName)
                release.bundleZipUrl = getAssetUrl (tag, assetName);

            if (assetName == installerName)
                release.installerUrl = getAssetUrl (tag, assetName);
        }
    }

    return true;
}

//==================================================================================================
// Process-wide memory: the editor's Updater comes and goes with the plugin window, but a pending
// restart and the last check result belong to the process.
struct CheckResult
{
    std::optional<Updater::ReleaseInfo> release;
    juce::String sha256;
};

struct ProcessState
{
    juce::CriticalSection lock;
    juce::String installedVersion;          // installed in place by this process, restart pending
    std::optional<CheckResult> lastCheck;   // last successful check
    juce::int64 lastCheckTimeMs = 0;
};

ProcessState& getProcessState()
{
    static ProcessState state;
    return state;
}

juce::String getVersionInstalledThisProcess()
{
    auto& state = getProcessState();
    const juce::ScopedLock sl (state.lock);
    return state.installedVersion;
}

//==================================================================================================
juce::File getTempFolder()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("BeatboxReplacer");
}

// A fresh <temp>/BeatboxReplacer/update-<ms> folder; first sweeps ones a crash left behind.
juce::File createWorkFolder()
{
    const auto root = getTempFolder();
    const auto now = juce::Time::getCurrentTime();

    for (const auto& old : root.findChildFiles (juce::File::findDirectories, false, "update-*", juce::File::FollowSymlinks::no))
        if ((now - old.getLastModificationTime()).inMilliseconds() > kStaleWorkFolderMs)
            old.deleteRecursively();

    const auto folder = root.getNonexistentChildFile ("update-" + juce::String (juce::Time::currentTimeMillis()), {}, false);
    return folder.createDirectory().wasOk() ? folder : juce::File();
}

// Renames without ever replacing an existing file (File::moveFileTo deletes its target first).
// Retries because antivirus scanners and indexers briefly lock freshly written files on Windows.
bool renameWithRetries (const juce::File& from, const juce::File& to)
{
    for (int attempt = 0; attempt < kRenameAttempts; ++attempt)
    {
        if (attempt > 0)
            juce::Thread::sleep (kRenameRetryDelayMs);

        if (to.exists() || ! from.exists())
            return false;

        if (from.moveFileTo (to))
            return true;
    }

    return false;
}

// "<name>.old-<stamp>" next to f, not used yet. Never ends in ".vst3" (hosts scan for that).
juce::File getAsideFile (const juce::File& f, const juce::String& stamp)
{
    auto aside = f.getSiblingFile (f.getFileName() + ".old-" + stamp);

    for (int i = 2; aside.exists(); ++i)
        aside = f.getSiblingFile (f.getFileName() + ".old-" + stamp + "-" + juce::String (i));

    return aside;
}

bool isUpdateLeftover (const juce::String& name)
{
    return name.contains (".old-") || name.endsWith (".new") || name.startsWith (kProbePrefix);
}

// The installed bundle this code was loaded from, or {} (always {} where there are no bundles).
juce::File findRunningBundle()
{
   #if JUCE_WINDOWS
    // <bundle>\Contents\x86_64-win\BeatboxReplacer.vst3 is the DLL itself.
    const auto binary = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    const auto contents = binary.getParentDirectory().getParentDirectory();
    const auto bundle = contents.getParentDirectory();

    if (binary.existsAsFile() && contents.getFileName().equalsIgnoreCase ("Contents")
         && bundle.isDirectory() && bundle.hasFileExtension ("vst3"))
        return bundle;
   #elif JUCE_MAC
    const auto bundle = juce::File::getSpecialLocation (juce::File::currentApplicationFile);

    if ((bundle.hasFileExtension ("vst3") || bundle.hasFileExtension ("component"))
         && bundle.getChildFile ("Contents").isDirectory())
        return bundle;
   #endif

    return {};
}

// The running bundle first, then (macOS) the copies in the standard plug-in folders.
juce::Array<juce::File> findInstalledBundles()
{
    juce::Array<juce::File> bundles;

    const auto add = [&bundles] (const juce::File& bundle)
    {
        if (bundle.getFullPathName().isEmpty() || ! bundle.getChildFile ("Contents").isDirectory())
            return;

        const auto resolved = bundle.getLinkedTarget();

        if (! bundles.contains (resolved))
            bundles.add (resolved);
    };

    add (findRunningBundle());

   #if JUCE_MAC
    for (const auto& root : { juce::File ("/Library/Audio/Plug-Ins"),
                              juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins") })
    {
        add (root.getChildFile ("VST3").getChildFile (kVst3BundleName));
        add (root.getChildFile ("Components").getChildFile (kAuBundleName));
    }
   #endif

    return bundles;
}

// searchInside: Windows leftovers sit next to each replaced file anywhere in the bundle; on macOS
// only "Contents.old-*" / "Contents.new" at the bundle root, so a signed Contents is never touched.
void deleteUpdateLeftovers (const juce::File& bundle, bool searchInside)
{
    if (! bundle.isDirectory())
        return;

    for (const auto& item : bundle.findChildFiles (juce::File::findFilesAndDirectories, searchInside, "*", juce::File::FollowSymlinks::no))
        if (isUpdateLeftover (item.getFileName()))
            item.deleteRecursively(); // fails harmlessly while some process still has the file loaded
}

// Write-access check, done before downloading anything.
bool canModifyBundle (const juce::File& bundle)
{
   #if JUCE_WINDOWS
    // Create and delete a probe file next to the DLL.
    const auto binaryFolder = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

    if (! binaryFolder.isAChildOf (bundle))
        return false;

    const auto probe = binaryFolder.getNonexistentChildFile (kProbePrefix, ".tmp", false);
    const bool created = probe.create().wasOk() && probe.existsAsFile();
    return probe.deleteFile() && created;
   #else
    return bundle.hasWriteAccess() && bundle.getChildFile ("Contents").hasWriteAccess();
   #endif
}

// Unpacks every entry into an empty folder and checks the extracted file sizes.
bool extractZip (const juce::File& zipFile, const juce::File& destination)
{
    if (! destination.deleteRecursively() || destination.createDirectory().failed())
        return false;

    juce::ZipFile zip (zipFile);

    if (zip.getNumEntries() == 0)
        return false;

    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        if (zip.uncompressEntry (i, destination, juce::ZipFile::OverwriteFiles::yes, juce::ZipFile::FollowSymlinks::no).failed())
            return false;

        if (const auto* entry = zip.getEntry (i))
        {
            const auto path = entry->filename.replaceCharacter ('\\', '/');

            if (path.isNotEmpty() && ! path.endsWithChar ('/') && ! entry->isSymbolicLink
                 && destination.getChildFile (path).getSize() != entry->uncompressedSize)
                return false;
        }
    }

    return true;
}

// The bundle folder called bundleName at the top of folder (or, failing that, anywhere inside it).
juce::File findBundleIn (const juce::File& folder, const juce::String& bundleName)
{
    const auto atRoot = folder.getChildFile (bundleName);

    if (atRoot.getChildFile ("Contents").isDirectory())
        return atRoot;

    for (const auto& dir : folder.findChildFiles (juce::File::findDirectories, true, bundleName, juce::File::FollowSymlinks::no))
        if (dir.getChildFile ("Contents").isDirectory())
            return dir;

    return {};
}

// Runs a command-line tool (macOS: ditto, xattr, codesign, open, killall) and waits for it.
bool runTool (const juce::StringArray& args, juce::String& output)
{
    output.clear();
    juce::ChildProcess process;

    if (! process.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return false;

    output = process.readAllProcessOutput(); // returns once the tool closes its output

    if (! process.waitForProcessToFinish (kToolTimeoutMs))
    {
        process.kill();
        return false;
    }

    return process.getExitCode() == 0;
}

bool isValidlySigned (const juce::File& bundle)
{
    juce::String output;
    const bool ok = runTool ({ "/usr/bin/codesign", "--verify", "--deep", "--strict", bundle.getFullPathName() }, output);

    if (! ok)
    {
        DBG ("codesign --verify failed for " << bundle.getFullPathName() << ": " << output);
    }

    return ok;
}

} // namespace

//==================================================================================================
// One job at a time on a background thread. Status changes go through owner.lock and are followed
// by sendChangeMessage(), which is safe from any thread.
class Updater::Worker final : public juce::Thread,
                              private juce::AsyncUpdater
{
public:
    enum class Job
    {
        none,
        check,
        install,
        runInstaller
    };

    explicit Worker (Updater& u)
        : juce::Thread ("BeatboxReplacer updater"), owner (u)
    {
    }

    ~Worker() override
    {
        shutdown();
        cancelPendingUpdate();
    }

    // Message thread. Queues a job (it runs after the current one, if any); false if one is queued.
    bool post (Job job)
    {
        {
            const juce::ScopedLock sl (jobLock);

            if (pendingJob != Job::none)
                return false;

            pendingJob = job;
        }

        if (! isThreadRunning() && ! startThread())
        {
            const juce::ScopedLock sl (jobLock);
            pendingJob = Job::none;
            return false;
        }

        notify();
        return true;
    }

    // Stops the thread, cancelling any download. A file swap that has started is finished first.
    void shutdown()
    {
        signalThreadShouldExit();

        {
            const juce::ScopedLock sl (streamLock);

            if (activeStream != nullptr)
                activeStream->cancel();
        }

        stopThread (kStopTimeoutMs);
    }

    // Turns a check result (fresh or cached) into the updater's status.
    static void applyCheckResult (Updater& updater, const CheckResult& result)
    {
        const auto current = Updater::getCurrentVersion();
        const auto installed = getVersionInstalledThisProcess();
        const auto& latest = result.release;
        const bool newer = latest.has_value() && bbr::isNewerVersion (latest->version.toStdString(), current.toStdString());
        const bool newerThanInstalled = newer && bbr::isNewerVersion (latest->version.toStdString(), installed.toStdString());

        {
            const juce::ScopedLock sl (updater.lock);

            updater.needsFullInstaller = false;
            updater.progress = 0.0f;

            if (newer)
            {
                updater.available = latest;
                updater.availableSha256 = result.sha256;
            }
            else
            {
                updater.available.reset();
                updater.availableSha256.clear();
            }

            if (installed.isNotEmpty() && ! newerThanInstalled)
            {
                updater.status = Status::installedRestartRequired;
                updater.statusText = getRestartText (installed);
            }
            else if (newer)
            {
                updater.status = Status::updateAvailable;
                updater.statusText = "Version " + latest->version + " is available";
            }
            else
            {
                updater.status = Status::upToDate;
                updater.statusText = "Up to date (" + current + ")";
            }
        }

        updater.sendChangeMessage();
    }

private:
    Updater& owner;
    juce::CriticalSection jobLock, streamLock;
    Job pendingJob = Job::none;                     // guarded by jobLock
    juce::File installerToLaunch;                   // guarded by jobLock
    juce::WebInputStream* activeStream = nullptr;   // guarded by streamLock

    // Lets shutdown() cancel a connect or read that is blocking this thread.
    struct StreamRegistration
    {
        StreamRegistration (Worker& w, juce::WebInputStream& stream)
            : worker (w)
        {
            const juce::ScopedLock sl (worker.streamLock);
            worker.activeStream = &stream;

            if (worker.threadShouldExit())
                stream.cancel();
        }

        ~StreamRegistration()
        {
            const juce::ScopedLock sl (worker.streamLock);
            worker.activeStream = nullptr;
        }

        Worker& worker;

        JUCE_DECLARE_NON_COPYABLE (StreamRegistration)
    };

    void run() override
    {
        while (! threadShouldExit())
        {
            auto job = Job::none;

            {
                const juce::ScopedLock sl (jobLock);
                std::swap (job, pendingJob);
            }

            switch (job)
            {
                case Job::check:        checkJob(); break;
                case Job::install:      installJob(); break;
                case Job::runInstaller: installerJob(); break;
                case Job::none:         wait (-1.0); break;
            }
        }
    }

    void report (Status newStatus, const juce::String& text, bool fullInstallerNeeded)
    {
        {
            const juce::ScopedLock sl (owner.lock);
            owner.status = newStatus;
            owner.statusText = text;
            owner.needsFullInstaller = fullInstallerNeeded;
        }

        owner.sendChangeMessage();
    }

    //==============================================================================================
    // GETs address into out. True only for a complete HTTP 200 response. httpStatus stays 0 when
    // nothing answered (offline, DNS, TLS, timeout, cancelled). With a progressLabel, reports
    // "<label> 45%" while downloading.
    bool fetch (const juce::String& address, bool forGitHubApi, juce::OutputStream& out,
                juce::int64 maxBytes, int& httpStatus, const juce::String& progressLabel = {})
    {
        httpStatus = 0;

        juce::WebInputStream stream (juce::URL (address), false);
        stream.withExtraHeaders (createRequestHeaders (forGitHubApi))
              .withConnectionTimeout (kConnectTimeoutMs)
              .withNumRedirectsToFollow (kMaxRedirects);

        const StreamRegistration registration (*this, stream);

        if (! stream.connect (nullptr) || stream.isError())
            return false;

        httpStatus = stream.getStatusCode();

        if (httpStatus != 200)
            return false;

        const auto totalBytes = stream.getTotalLength(); // -1 if unknown

        if (totalBytes > maxBytes)
            return false;

        std::vector<char> buffer ((size_t) kReadChunkBytes);
        juce::int64 bytesRead = 0;
        int lastPercent = -1;
        auto lastReportMs = juce::Time::getMillisecondCounter();

        while (! threadShouldExit())
        {
            const auto numRead = stream.read (buffer.data(), kReadChunkBytes);

            if (numRead <= 0)
                break;

            bytesRead += numRead;

            if (bytesRead > maxBytes || ! out.write (buffer.data(), (size_t) numRead))
                return false;

            if (progressLabel.isNotEmpty() && totalBytes > 0)
            {
                const auto fraction = juce::jlimit (0.0, 1.0, (double) bytesRead / (double) totalBytes);
                const auto percent = (int) (fraction * 100.0);
                const auto now = juce::Time::getMillisecondCounter();
                owner.progress = (float) fraction;

                if (percent != lastPercent && (now - lastReportMs >= 250u || percent == 100))
                {
                    lastPercent = percent;
                    lastReportMs = now;
                    owner.setStatus (Status::downloading, progressLabel + " " + juce::String (percent) + "%");
                }
            }
        }

        if (threadShouldExit() || stream.isError())
            return false;

        return totalBytes < 0 || bytesRead == totalBytes;
    }

    bool fetchText (const juce::String& address, bool forGitHubApi, juce::String& text, int& httpStatus)
    {
        juce::MemoryOutputStream data;

        if (! fetch (address, forGitHubApi, data, kMaxJsonBytes, httpStatus))
            return false;

        text = data.toString();
        return true;
    }

    // Downloads address to target (replacing it). Returns {} or a message for the status line.
    juce::String download (const juce::String& address, const juce::File& target, const juce::String& what)
    {
        if (! target.deleteFile() || target.getParentDirectory().createDirectory().failed())
            return "Couldn't write to the temp folder";

        int httpStatus = 0;
        bool received = false, saved = false;
        owner.progress = 0.0f;

        {
            juce::FileOutputStream out (target);

            if (out.failedToOpen())
                return "Couldn't write to the temp folder";

            received = fetch (address, false, out, kMaxDownloadBytes, httpStatus, "Downloading " + what + "...");
            out.flush();
            saved = out.getStatus().wasOk();
        }

        if (received && saved)
            return {};

        target.deleteFile();

        if (! saved)
            return "Couldn't save the " + what + " (is the disk full?)";

        if (httpStatus == 0)
            return "Couldn't download the " + what + " (no internet?)";

        if (httpStatus != 200)
            return "Couldn't download the " + what + " (error " + juce::String (httpStatus) + ")";

        return "The " + what + " download was interrupted - try again";
    }

    //==============================================================================================
    void checkJob()
    {
        CheckResult result;
        int manifestStatus = 0, apiStatus = 0;

        {
            juce::String text;
            Updater::ReleaseInfo release;

            if (fetchText (getReleasesUrl() + "/latest/download/latest.json", false, text, manifestStatus)
                 && parseManifest (text, release, result.sha256))
                result.release = release;
        }

        if (! result.release.has_value() && ! threadShouldExit())
        {
            juce::String text;
            Updater::ReleaseInfo release;
            result.sha256.clear();

            if (fetchText (getApiLatestReleaseUrl(), true, text, apiStatus) && parseApiRelease (text, release))
                result.release = release;
        }

        if (threadShouldExit())
            return;

        if (! result.release.has_value())
        {
            juce::String text ("Couldn't check for updates (no internet?)");

            if (apiStatus == 404 && (manifestStatus == 404 || manifestStatus == 0))
                text = "No releases found";
            else if (apiStatus != 0 || manifestStatus != 0)
                text = "Couldn't check for updates right now - try again later (error "
                       + juce::String (apiStatus != 0 ? apiStatus : manifestStatus) + ")";

            {
                const juce::ScopedLock sl (owner.lock);
                owner.available.reset();
                owner.availableSha256.clear();
            }

            report (Status::failed, text, false);
            return;
        }

        {
            auto& state = getProcessState();
            const juce::ScopedLock sl (state.lock);
            state.lastCheck = result;
            state.lastCheckTimeMs = juce::Time::currentTimeMillis();
        }

        applyCheckResult (owner, result);
    }

    //==============================================================================================
    void installJob()
    {
        std::optional<Updater::ReleaseInfo> release;
        juce::String expectedSha256;

        {
            const juce::ScopedLock sl (owner.lock);
            release = owner.available;
            expectedSha256 = owner.availableSha256;
        }

        if (! release.has_value())
        {
            report (Status::failed, "There's no update to install", false);
            return;
        }

       #if JUCE_WINDOWS || JUCE_MAC
        installInPlace (*release, expectedSha256);
       #else
        report (Status::failed, "In-place update isn't supported on this platform", false);
       #endif
    }

    void installInPlace (const Updater::ReleaseInfo& release, const juce::String& expectedSha256)
    {
        const auto running = findRunningBundle();

        if (running == juce::File())
        {
            report (Status::failed, "Couldn't find the installed plug-in - use Run installer to update", true);
            return;
        }

        if (release.bundleZipUrl.isEmpty())
        {
            report (Status::failed, "This release has no in-place update - use Run installer to update", true);
            return;
        }

        if (! canModifyBundle (running))
        {
            report (Status::failed, "No permission to change the installed plug-in - use Run installer to update", true);
            return;
        }

        const auto workFolder = createWorkFolder();

        if (workFolder == juce::File())
        {
            report (Status::failed, "Couldn't write to the temp folder", false);
            return;
        }

        const auto zipFile = workFolder.getChildFile ("update.zip");
        auto error = download (release.bundleZipUrl, zipFile, "update");

        if (error.isEmpty() && expectedSha256.isNotEmpty()
             && juce::SHA256 (zipFile).toHexString().toLowerCase() != expectedSha256.trim().toLowerCase())
            error = "The download was damaged (checksum mismatch) - try again";

        if (error.isNotEmpty() || threadShouldExit())
        {
            workFolder.deleteRecursively();

            if (! threadShouldExit())
                report (Status::failed, error, false);

            return;
        }

        report (Status::installing, "Installing...", false);

        juce::String warning;
       #if JUCE_WINDOWS
        error = installWindowsBundle (zipFile, workFolder, running);
       #else
        error = installMacBundles (zipFile, workFolder, running, warning);
       #endif

        workFolder.deleteRecursively();

        if (error.isNotEmpty())
        {
            report (Status::failed, error, true);
            return;
        }

        {
            auto& state = getProcessState();
            const juce::ScopedLock sl (state.lock);
            state.installedVersion = release.version;
        }

        report (Status::installedRestartRequired, getRestartText (release.version) + warning, false);
    }

    // Windows (see Updater.h): every new file is copied to "<dest>.new" inside the installed bundle
    // (so it inherits the folder's ACL), then all of them are swapped in; any failure rolls back.
    static juce::String installWindowsBundle (const juce::File& zipFile, const juce::File& workFolder,
                                              const juce::File& installedBundle)
    {
        const auto extracted = workFolder.getChildFile ("extracted");

        if (! extractZip (zipFile, extracted))
            return "The update couldn't be unpacked - use Run installer to update";

        const auto freshBundle = findBundleIn (extracted, kVst3BundleName);
        const auto binary = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

        if (freshBundle == juce::File()
             || ! freshBundle.getChildFile (binary.getRelativePathFrom (installedBundle)).existsAsFile())
            return "The update is incomplete - use Run installer to update";

        return replaceBundleFiles (installedBundle, freshBundle);
    }

    static juce::String replaceBundleFiles (const juce::File& installedBundle, const juce::File& freshBundle)
    {
        struct Item
        {
            juce::File target, staged, aside;
            bool swapped = false;
        };

        std::vector<Item> items;

        const auto discardStaged = [&items]
        {
            for (const auto& item : items)
                item.staged.deleteFile();
        };

        // 1. Stage: nothing installed changes yet.
        for (const auto& source : freshBundle.findChildFiles (juce::File::findFiles, true, "*", juce::File::FollowSymlinks::no))
        {
            Item item;
            item.target = installedBundle.getChildFile (source.getRelativePathFrom (freshBundle));
            item.staged = item.target.getSiblingFile (item.target.getFileName() + ".new");

            if (item.target.existsAsFile() && item.target.hasIdenticalContentTo (source))
                continue;

            if (item.target.getParentDirectory().createDirectory().failed()
                 || ! item.staged.deleteFile()
                 || ! source.copyFileTo (item.staged)
                 || item.staged.getSize() != source.getSize())
            {
                item.staged.deleteFile();
                discardStaged();
                return "Couldn't copy the new files into the plug-in folder - use Run installer to update";
            }

            items.push_back (item);
        }

        // 2. Swap: move the old file aside (a loaded DLL can be renamed, not deleted or overwritten),
        //    then move the new one into place.
        const auto stamp = juce::String (juce::Time::currentTimeMillis() / 1000);
        bool ok = true;

        for (auto& item : items)
        {
            if (item.target.exists())
            {
                item.aside = getAsideFile (item.target, stamp);

                if (! renameWithRetries (item.target, item.aside))
                {
                    item.aside = juce::File();
                    ok = false;
                    break;
                }
            }

            if (! renameWithRetries (item.staged, item.target))
            {
                if (item.aside != juce::File())
                    renameWithRetries (item.aside, item.target);

                item.aside = juce::File();
                ok = false;
                break;
            }

            item.swapped = true;
        }

        if (! ok)
        {
            for (auto it = items.rbegin(); it != items.rend(); ++it)
            {
                if (! it->swapped)
                    continue;

                if (! it->target.deleteFile())
                    renameWithRetries (it->target, it->staged);

                if (it->aside != juce::File())
                    renameWithRetries (it->aside, it->target);
            }

            discardStaged();
            return "Couldn't replace the plug-in files - use Run installer to update";
        }

        // 3. Old files go now, except the loaded DLL: cleanupAfterPreviousUpdate() gets that later.
        for (const auto& item : items)
            if (item.aside != juce::File())
                item.aside.deleteFile();

        return {};
    }

    // macOS (see Updater.h): unpack with ditto, refuse anything codesign doesn't accept, then swap
    // the Contents folder of every installed bundle. The running bundle comes first; if it fails
    // nothing has changed. Failures with other copies only add a note to the status text.
    static juce::String installMacBundles (const juce::File& zipFile, const juce::File& workFolder,
                                           const juce::File& runningBundle, juce::String& warning)
    {
        const auto staging = workFolder.getChildFile ("staging");
        juce::String output;

        if (staging.createDirectory().failed()
             || ! runTool ({ "/usr/bin/ditto", "-x", "-k", zipFile.getFullPathName(), staging.getFullPathName() }, output))
            return "The update couldn't be unpacked - use Run installer to update";

        runTool ({ "/usr/bin/xattr", "-dr", "com.apple.quarantine", staging.getFullPathName() }, output);

        const auto stagedVst3 = findBundleIn (staging, kVst3BundleName);
        const auto stagedAu = findBundleIn (staging, kAuBundleName);
        const bool vst3Ok = stagedVst3 != juce::File() && isValidlySigned (stagedVst3);
        const bool auOk = stagedAu != juce::File() && isValidlySigned (stagedAu);

        const auto runningResolved = runningBundle.getLinkedTarget();
        const auto stamp = juce::String (juce::Time::currentTimeMillis() / 1000);
        juce::StringArray skipped;
        bool updatedAu = false;

        for (const auto& bundle : findInstalledBundles())
        {
            const bool isAu = bundle.hasFileExtension ("component");
            const auto& staged = isAu ? stagedAu : stagedVst3;
            juce::String error;

            if (staged == juce::File())
                error = "The update is incomplete - use Run installer to update";
            else if (! (isAu ? auOk : vst3Ok))
                error = "The downloaded update failed the code-signature check - use Run installer to update";
            else if (! canModifyBundle (bundle))
                error = "No permission to change the installed plug-in - use Run installer to update";
            else
                error = swapBundleContents (bundle, staged, stamp);

            if (error.isEmpty())
            {
                updatedAu = updatedAu || isAu;
                continue;
            }

            if (bundle == runningResolved)
                return error;

            skipped.add (bundle.getParentDirectory().getFullPathName());
        }

        if (updatedAu)
            runTool ({ "/usr/bin/killall", "-9", "AudioComponentRegistrar" }, output); // refreshes the AU cache

        if (! skipped.isEmpty())
            warning = " (couldn't update the copy in " + skipped.joinIntoString (", ") + ")";

        return {};
    }

    // Never writes into the loaded binary: the new Contents is staged next to the old one and the
    // two are swapped by rename. Unlinking the old tree is safe while the host has it mapped.
    static juce::String swapBundleContents (const juce::File& bundle, const juce::File& stagedBundle, const juce::String& stamp)
    {
        const auto contents = bundle.getChildFile ("Contents");
        const auto fresh = bundle.getChildFile ("Contents.new");
        const auto failure = "Couldn't replace " + bundle.getFullPathName() + " - use Run installer to update";
        juce::String output;

        if (! fresh.deleteRecursively()
             || ! runTool ({ "/usr/bin/ditto", stagedBundle.getChildFile ("Contents").getFullPathName(), fresh.getFullPathName() }, output))
        {
            fresh.deleteRecursively();
            return failure;
        }

        const auto old = getAsideFile (contents, stamp);

        if (! renameWithRetries (contents, old))
        {
            fresh.deleteRecursively();
            return failure;
        }

        if (! renameWithRetries (fresh, contents))
        {
            renameWithRetries (old, contents);
            fresh.deleteRecursively();
            return failure;
        }

        old.deleteRecursively();
        return {};
    }

    //==============================================================================================
    void installerJob()
    {
        const auto release = owner.getAvailableRelease();
        const juce::String defaultName (kPlatform.installer);
        auto address = release.has_value() ? release->installerUrl : juce::String();

        if (address.isEmpty() && defaultName.isNotEmpty())
            address = getReleasesUrl() + "/latest/download/" + defaultName;

        if (address.isEmpty())
        {
            report (Status::failed, "There's no installer for this platform", false);
            return;
        }

        auto fileName = address.fromLastOccurrenceOf ("/", false, false);

        if (! isPlainName (fileName))
            fileName = defaultName;

        auto installer = getTempFolder().getChildFile (fileName);

        if (! installer.deleteFile()) // an earlier installer may still be open
            installer = installer.getNonexistentSibling (false);

        const auto error = download (address, installer, "installer");

        if (threadShouldExit())
            return;

        if (error.isNotEmpty())
        {
            report (Status::failed, error, true);
            return;
        }

        report (Status::installing, "Opening the installer...", false);

       #if JUCE_WINDOWS
        // ShellExecute (which brings up the UAC prompt) belongs on the message thread.
        {
            const juce::ScopedLock sl (jobLock);
            installerToLaunch = installer;
        }

        triggerAsyncUpdate();
       #elif JUCE_MAC
        juce::String output;
        runTool ({ "/usr/bin/xattr", "-d", "com.apple.quarantine", installer.getFullPathName() }, output);
        installerLaunched (installer, runTool ({ "/usr/bin/open", installer.getFullPathName() }, output));
       #else
        installerLaunched (installer, false);
       #endif
    }

    void handleAsyncUpdate() override
    {
        juce::File installer;

        {
            const juce::ScopedLock sl (jobLock);
            std::swap (installer, installerToLaunch);
        }

        if (installer != juce::File())
            installerLaunched (installer, installer.startAsProcess());
    }

    void installerLaunched (const juce::File& installer, bool opened)
    {
        if (opened)
        {
            report (Status::installedRestartRequired, "The installer is open - follow it, then restart Ableton", false);
            return;
        }

        juce::MessageManager::callAsync ([installer] { installer.revealToUser(); });
        report (Status::failed, "Couldn't open the installer - it was saved to "
                                    + installer.getParentDirectory().getFullPathName(), true);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Worker)
};

//==================================================================================================
Updater::Updater()
    : worker (std::make_unique<Worker> (*this))
{
    const auto installed = getVersionInstalledThisProcess();

    if (installed.isNotEmpty())
    {
        status = Status::installedRestartRequired;
        statusText = getRestartText (installed);
    }
}

Updater::~Updater()
{
    worker->shutdown();
    worker.reset();
}

void Updater::checkForUpdates (bool force)
{
    {
        const juce::ScopedLock sl (lock);

        if (isBusy (status) || (hasChecked && ! force))
            return;

        hasChecked = true;
    }

    if (! force)
    {
        std::optional<CheckResult> cached;

        {
            auto& state = getProcessState();
            const juce::ScopedLock sl (state.lock);

            if (state.lastCheck.has_value() && juce::Time::currentTimeMillis() - state.lastCheckTimeMs < kCheckCacheMs)
                cached = state.lastCheck;
        }

        if (cached.has_value())
        {
            Worker::applyCheckResult (*this, *cached);
            return;
        }
    }

    {
        const juce::ScopedLock sl (lock);
        status = Status::checking;
        statusText = "Checking for updates...";
        needsFullInstaller = false;
    }

    if (worker->post (Worker::Job::check))
        sendChangeMessage();
    else
        setStatus (Status::failed, "Couldn't check for updates - try again");
}

void Updater::installUpdate()
{
    {
        const juce::ScopedLock sl (lock);

        if (! available.has_value() || (status != Status::updateAvailable && status != Status::failed))
            return;

        status = Status::downloading;
        statusText = "Downloading update...";
        needsFullInstaller = false;
        progress = 0.0f;
    }

    if (worker->post (Worker::Job::install))
        sendChangeMessage();
    else
        setStatus (Status::failed, "Couldn't start the update - try again");
}

void Updater::runFullInstaller()
{
    {
        const juce::ScopedLock sl (lock);

        if (isBusy (status))
            return;

        status = Status::downloading;
        statusText = "Downloading installer...";
        needsFullInstaller = false;
        progress = 0.0f;
    }

    if (worker->post (Worker::Job::runInstaller))
        sendChangeMessage();
    else
        setStatus (Status::failed, "Couldn't start the download - try again");
}

Updater::Status Updater::getStatus() const
{
    const juce::ScopedLock sl (lock);
    return status;
}

juce::String Updater::getStatusText() const
{
    const juce::ScopedLock sl (lock);
    return statusText;
}

float Updater::getProgress() const
{
    {
        const juce::ScopedLock sl (lock);

        if (status != Status::downloading)
            return 0.0f;
    }

    return progress.load();
}

std::optional<Updater::ReleaseInfo> Updater::getAvailableRelease() const
{
    const juce::ScopedLock sl (lock);
    return available;
}

bool Updater::installFailedNeedsFullInstaller() const
{
    const juce::ScopedLock sl (lock);
    return needsFullInstaller;
}

juce::String Updater::getCurrentVersion()
{
    return JucePlugin_VersionString;
}

void Updater::cleanupAfterPreviousUpdate()
{
    try
    {
       #if JUCE_WINDOWS
        const bool searchInside = true;
       #else
        const bool searchInside = false;
       #endif

        for (const auto& bundle : findInstalledBundles())
            deleteUpdateLeftovers (bundle, searchInside);
    }
    catch (...)
    {
    }
}

void Updater::setStatus (Status s, const juce::String& text)
{
    {
        const juce::ScopedLock sl (lock);
        status = s;
        statusText = text;
    }

    sendChangeMessage();
}
