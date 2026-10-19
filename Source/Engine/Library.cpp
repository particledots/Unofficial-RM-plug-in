#include "Library.h"
#include "RawFormat.h"
#include <algorithm>
#include <string>

namespace rm
{
void Library::registerSupportedFormats (juce::AudioFormatManager& formats)
{
    formats.registerFormat (new juce::WavAudioFormat(), true);
    formats.registerFormat (new juce::AiffAudioFormat(), false);
    formats.registerFormat (new RawAudioFormat(), false);
}

static bool hasSupportedExtension (const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".raw";
}

// Documented: files and folders starting with '.' or '_' are ignored (this also skips macOS "._" files).
static bool isIgnoredName (const juce::String& name)
{
    return name.startsWithChar ('.') || name.startsWithChar ('_');
}

// Documented: sorted lexicographically. Byte-wise, case-sensitive comparison of the name.
static bool nameLess (const juce::String& a, const juce::String& b)
{
    return std::string (a.toRawUTF8()) < std::string (b.toRawUTF8());
}

static juce::Array<juce::File> sortedChildren (const juce::File& dir, int whatToLookFor)
{
    auto children = dir.findChildFiles (whatToLookFor, false);
    children.removeIf ([] (const juce::File& f) { return isIgnoredName (f.getFileName()); });

    std::vector<juce::File> v (children.begin(), children.end());
    std::sort (v.begin(), v.end(), [] (const juce::File& a, const juce::File& b)
    {
        return nameLess (a.getFileName(), b.getFileName());
    });

    juce::Array<juce::File> out;
    for (const auto& f : v)
        out.add (f);
    return out;
}

// All supported audio files inside a subfolder, including nested subfolders (documented: subfolders can be nested;
// how nesting affects selection is not documented, so a nested folder just contributes its files to the same slot).
static void collectFiles (const juce::File& dir, const juce::File& base, int depth, std::vector<juce::File>& out)
{
    if (depth > 8)
        return;

    for (const auto& f : sortedChildren (dir, juce::File::findFilesAndDirectories))
    {
        if (f.isDirectory())
            collectFiles (f, base, depth + 1, out);
        else if (hasSupportedExtension (f))
            out.push_back (f);
    }
}

namespace
{
struct Scanner
{
    Library& lib;
    juce::AudioFormatManager& formats;
    const std::atomic<bool>& cancel;
    int bankFiles = 0;

    bool full() const { return bankFiles >= kMaxFilesPerBank || (int) lib.stations.size() >= kMaxFilesTotal; }

    // Reads a file's header. Returns false (and records it) if it cannot be used.
    bool addFile (const juce::File& f)
    {
        if (full())
            return false;

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));

        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples < 2 || reader->numChannels < 1)
        {
            lib.skippedFiles.add (f.getFileName());
            return false;
        }

        StationEntry e;
        e.file            = f;
        e.info.sampleRate = reader->sampleRate;
        e.info.numFrames  = (int64_t) reader->lengthInSamples;
        e.skip            = parseSkipCount (f.getFileNameWithoutExtension());
        lib.stations.push_back (std::move (e));
        ++bankFiles;
        return true;
    }

    void scanBank (int number, const juce::File& dir, bool separateSettings)
    {
        Bank bank;
        bank.number    = number;
        bank.folder    = dir;
        bank.firstSlot = (int) lib.slots.size();
        bank.settings  = lib.rootSettings;

        if (separateSettings)
        {
            const auto sf = dir.getChildFile ("settings.txt");
            bank.ownSettingsFound = sf.existsAsFile();
            if (bank.ownSettingsFound)
                bank.settings = mergeSettings (bank.settings, parseSettings (sf.loadFileAsString()));
        }

        bankFiles = 0;

        for (const auto& entry : sortedChildren (dir, juce::File::findFilesAndDirectories))
        {
            if (cancel.load() || full())
                break;

            if (entry.isDirectory())
            {
                std::vector<juce::File> files;
                collectFiles (entry, entry, 0, files);

                Slot slot;
                slot.firstFile = (int) lib.stations.size();
                slot.mode      = endsWithNext (entry.getFileName()) ? SlotNext : SlotRandom;
                slot.name      = entry.getFileName();

                for (const auto& f : files)
                    addFile (f);

                slot.numFiles = (int) lib.stations.size() - slot.firstFile;
                if (slot.numFiles > 0)
                    lib.slots.push_back (std::move (slot));
            }
            else if (hasSupportedExtension (entry))
            {
                Slot slot;
                slot.firstFile = (int) lib.stations.size();
                slot.mode      = SlotFile;
                slot.name      = entry.getFileName();

                if (addFile (entry))
                {
                    slot.numFiles = 1;
                    lib.slots.push_back (std::move (slot));
                }
            }
        }

        bank.numSlots = (int) lib.slots.size() - bank.firstSlot;
        lib.banks.push_back (std::move (bank));
    }
};
} // namespace

std::shared_ptr<Library> Library::scan (const juce::File& folder,
                                        juce::AudioFormatManager& formats,
                                        const std::atomic<bool>& shouldCancel)
{
    auto lib = std::make_shared<Library>();
    lib->folder = folder;

    if (! folder.isDirectory())
        return lib;

    const auto rootSettingsFile = folder.getChildFile ("settings.txt");
    lib->rootSettingsFound = rootSettingsFile.existsAsFile();
    if (lib->rootSettingsFound)
        lib->rootSettings = parseSettings (rootSettingsFile.loadFileAsString());

    // Documented: root folders whose names start with a number from 0 to 15 are banks. If any exist, root files are ignored.
    // If two folders claim the same number, the first in name order is used (not documented).
    std::vector<std::pair<int, juce::File>> numbered;
    for (const auto& d : sortedChildren (folder, juce::File::findDirectories))
    {
        const int n = parseBankNumber (d.getFileName());
        if (n < 0)
            continue;

        const bool taken = std::any_of (numbered.begin(), numbered.end(), [n] (const auto& p) { return p.first == n; });
        if (! taken)
            numbered.emplace_back (n, d);
    }

    Scanner scanner { *lib, formats, shouldCancel };

    if (numbered.empty())
    {
        scanner.scanBank (0, folder, false);
    }
    else
    {
        lib->hasNumberedBanks = true;
        std::sort (numbered.begin(), numbered.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& nb : numbered)
        {
            if (shouldCancel.load())
                break;
            scanner.scanBank (nb.first, nb.second, true);
        }
    }

    return lib;
}
} // namespace rm
