// Radio Music (unofficial) - sample library scanning.
// Clean-room implementation from the documented behaviour of Music Thing
// Modular's Radio Music mk2 (Tom Whitwell with Chris Johnson).
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
#include <vector>

namespace rm
{
constexpr int kMaxStationsPerBank = 250;   // documented limit: 250 audio files in one bank

struct StationInfo
{
    double  sampleRate = 0.0;   // file sample rate in Hz
    int64_t numFrames  = 0;     // length in sample frames
};

struct StationEntry
{
    juce::File  file;
    StationInfo info;
};

// An immutable snapshot of one folder ("bank"). Created by scan(), never modified afterwards.
struct Library
{
    juce::File                 folder;
    std::vector<StationEntry>  stations;
    juce::StringArray          skippedFiles;   // files that looked like audio but could not be read

    // Scans one folder (not recursive). Only reads file headers, never audio data.
    // Returns an empty library if the folder does not exist.
    static std::shared_ptr<Library> scan (const juce::File& folder,
                                          juce::AudioFormatManager& formats,
                                          const std::atomic<bool>& shouldCancel);

    // Registers the formats this plugin supports (WAV and AIFF) on a manager.
    static void registerSupportedFormats (juce::AudioFormatManager& formats);
};
} // namespace rm
