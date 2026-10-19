// Radio Music (unofficial) - reader for headerless .raw files (milestone 4).
// Clean-room implementation from the documented behaviour of Music Thing
// Modular's Radio Music mk2 (Tom Whitwell with Chris Johnson).
//
// Documented: ".raw" files are 44.1 kHz, 16-bit, mono. The byte order and signedness are not documented;
// we read signed 16-bit little-endian, the common convention. This is unverified against the hardware.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace rm
{
class RawAudioReader : public juce::AudioFormatReader
{
public:
    explicit RawAudioReader (juce::InputStream* in)
        : juce::AudioFormatReader (in, "Raw 16-bit mono 44.1 kHz")
    {
        sampleRate = 44100.0;
        bitsPerSample = 16;
        numChannels = 1;
        usesFloatingPointData = false;
        lengthInSamples = in != nullptr ? in->getTotalLength() / 2 : 0;
    }

    bool readSamples (int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                      juce::int64 startSampleInSource, int numSamples) override
    {
        if (numSamples <= 0)
            return true;

        clearSamplesBeyondAvailableLength (destChannels, numDestChannels, startOffsetInDestBuffer,
                                           startSampleInSource, numSamples, lengthInSamples);

        if (startSampleInSource < 0 || startSampleInSource >= lengthInSamples)
            return true;

        const int available = (int) std::min<juce::int64> (numSamples, lengthInSamples - startSampleInSource);

        input->setPosition (startSampleInSource * 2);

        const int blockFrames = 2048;
        std::vector<unsigned char> bytes ((size_t) blockFrames * 2);

        int done = 0;
        while (done < available)
        {
            const int n = std::min (blockFrames, available - done);
            const int got = input->read (bytes.data(), n * 2) / 2;
            if (got <= 0)
                return false;

            for (int i = 0; i < got; ++i)
            {
                const auto s = (int16_t) (uint16_t) (bytes[(size_t) i * 2] | (bytes[(size_t) i * 2 + 1] << 8));
                const int v = (int) s * 65536;   // left-justified 32-bit, as JUCE expects

                for (int ch = 0; ch < numDestChannels; ++ch)
                    if (destChannels[ch] != nullptr)
                        destChannels[ch][startOffsetInDestBuffer + done + i] = v;
            }

            done += got;
            if (got < n)
                return false;
        }

        return true;
    }
};

class RawAudioFormat : public juce::AudioFormat
{
public:
    RawAudioFormat() : juce::AudioFormat ("Raw", juce::StringArray { ".raw" }) {}

    juce::Array<int> getPossibleSampleRates() override { return { 44100 }; }
    juce::Array<int> getPossibleBitDepths() override   { return { 16 }; }
    bool canDoStereo() override { return false; }
    bool canDoMono() override   { return true; }

    juce::AudioFormatReader* createReaderFor (juce::InputStream* in, bool deleteStreamIfOpeningFails) override
    {
        if (in == nullptr)
            return nullptr;

        if (in->getTotalLength() < 4)
        {
            if (deleteStreamIfOpeningFails)
                delete in;
            return nullptr;
        }

        return new RawAudioReader (in);
    }

    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>&,
                                                              const juce::AudioFormatWriterOptions&) override
    {
        return nullptr;   // reading only
    }
};
} // namespace rm
