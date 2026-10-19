// Radio Music (unofficial) - sample library scanning (banks, stations, subfolders).
// Clean-room implementation from the documented behaviour of Music Thing
// Modular's Radio Music mk2 (Tom Whitwell with Chris Johnson).
#pragma once

#include "LibraryRules.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
#include <vector>

namespace rm
{
constexpr int kMaxStationsPerBank = kMaxFilesPerBank;   // kept for older code

struct StationInfo
{
    double  sampleRate = 0.0;   // file sample rate in Hz
    int64_t numFrames  = 0;     // length in sample frames
};

// One audio file.
struct StationEntry
{
    juce::File  file;
    StationInfo info;
    int         skip = 0;       // "skipN" in the file name: ignore the next N resets while it plays
};

enum SlotMode { SlotFile = 0, SlotNext = 1, SlotRandom = 2 };

// One position on the tuning dial. Either a single file, or a subfolder that supplies one of several files
// (in order for a folder named "...next", otherwise at random).
struct Slot
{
    int          firstFile = 0;    // index into Library::stations (a slot's files are contiguous)
    int          numFiles  = 0;
    int          mode      = SlotFile;
    juce::String name;             // file name, or folder name
};

struct Bank
{
    int         number    = 0;     // 0..15
    juce::File  folder;
    int         firstSlot = 0;     // index into Library::slots
    int         numSlots  = 0;
    Settings    settings;          // top-level settings.txt overlaid with this bank's own settings.txt
    bool        ownSettingsFound = false;   // this bank folder has its own settings.txt
};

// An immutable snapshot of the chosen folder. Created by scan(), never modified afterwards.
// Documented: if numbered bank folders (0 to 15) exist, files in the root are ignored and each numbered folder is a bank.
// Otherwise the root folder itself is bank 0.
struct Library
{
    juce::File                 folder;
    std::vector<StationEntry>  stations;       // every file, bank by bank, slot by slot
    std::vector<Slot>          slots;          // every slot, bank by bank
    std::vector<Bank>          banks;          // banks that exist, in number order
    Settings                   rootSettings;
    bool                       hasNumberedBanks = false;
    bool                       rootSettingsFound = false;   // a settings.txt was found in the top-level folder
    juce::StringArray          skippedFiles;   // files that looked like audio but could not be read

    const Bank* findBank (int number) const noexcept
    {
        for (const auto& b : banks)
            if (b.number == number)
                return &b;
        return nullptr;
    }

    int lowestBankNumber() const noexcept { return banks.empty() ? 0 : banks.front().number; }

    // Scans a folder. Only reads file headers, never audio data.
    // Returns an empty library if the folder does not exist.
    static std::shared_ptr<Library> scan (const juce::File& folder,
                                          juce::AudioFormatManager& formats,
                                          const std::atomic<bool>& shouldCancel);

    // Registers the formats this plugin supports (WAV, AIFF and headerless 44.1 kHz 16-bit mono RAW).
    static void registerSupportedFormats (juce::AudioFormatManager& formats);
};
} // namespace rm
