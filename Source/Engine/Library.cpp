#include "Library.h"
#include <algorithm>
#include <string>

namespace rm
{
void Library::registerSupportedFormats (juce::AudioFormatManager& formats)
{
    formats.registerFormat (new juce::WavAudioFormat(), true);
    formats.registerFormat (new juce::AiffAudioFormat(), false);
}

static bool hasSupportedExtension (const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff";
}

std::shared_ptr<Library> Library::scan (const juce::File& folder,
                                        juce::AudioFormatManager& formats,
                                        const std::atomic<bool>& shouldCancel)
{
    auto lib = std::make_shared<Library>();
    lib->folder = folder;

    if (! folder.isDirectory())
        return lib;

    auto files = folder.findChildFiles (juce::File::findFiles, false);

    // Documented: files starting with '.' or '_' are ignored (this also skips macOS "._" files).
    files.removeIf ([] (const juce::File& f)
    {
        const auto name = f.getFileName();
        return name.startsWithChar ('.') || name.startsWithChar ('_') || ! hasSupportedExtension (f);
    });

    // Documented: sorted lexicographically. Byte-wise, case-sensitive comparison of the file name.
    std::vector<juce::File> sorted (files.begin(), files.end());
    std::sort (sorted.begin(), sorted.end(), [] (const juce::File& a, const juce::File& b)
    {
        return std::string (a.getFileName().toRawUTF8()) < std::string (b.getFileName().toRawUTF8());
    });

    for (const auto& f : sorted)
    {
        if (shouldCancel.load())
            break;

        if ((int) lib->stations.size() >= kMaxStationsPerBank)
            break;

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));

        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples < 2 || reader->numChannels < 1)
        {
            lib->skippedFiles.add (f.getFileName());
            continue;
        }

        StationEntry e;
        e.file             = f;
        e.info.sampleRate  = reader->sampleRate;
        e.info.numFrames   = (int64_t) reader->lengthInSamples;
        lib->stations.push_back (std::move (e));
    }

    return lib;
}
} // namespace rm
