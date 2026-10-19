#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
LibraryScanner::LibraryScanner (Callback cb)
    : juce::Thread ("RadioMusic scanner"), onFinished (std::move (cb))
{
    rm::Library::registerSupportedFormats (formats);
    startThread();
}

LibraryScanner::~LibraryScanner()
{
    cancel.store (true);
    stopThread (5000);
}

void LibraryScanner::requestScan (const juce::File& folder)
{
    {
        const juce::ScopedLock sl (lock);
        pendingFolder = folder;
        hasPending = true;
    }
    cancel.store (true);   // abandon any scan already running
    notify();
}

void LibraryScanner::run()
{
    while (! threadShouldExit())
    {
        wait (-1);

        while (! threadShouldExit())
        {
            juce::File folder;
            {
                const juce::ScopedLock sl (lock);
                if (! hasPending)
                    break;
                folder = pendingFolder;
                hasPending = false;
                cancel.store (false);
            }

            auto lib = rm::Library::scan (folder, formats, cancel);

            bool superseded;
            {
                const juce::ScopedLock sl (lock);
                superseded = hasPending;
            }

            if (! superseded && ! threadShouldExit())
                onFinished (lib);
        }
    }
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout RadioMusicProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "station", 1 }, "Station",
        NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "crossfade", 1 }, "Crossfade",
        NormalisableRange<float> (1.0f, 60000.0f, 0.0f, 0.3f), 25.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "volume", 1 }, "Volume",
        NormalisableRange<float> (-60.0f, 6.0f, 0.0f, 1.0f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    return layout;
}

RadioMusicProcessor::RadioMusicProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "RadioMusicState", createLayout()),
      scanner ([this] (std::shared_ptr<const rm::Library> lib) { libraryReady (std::move (lib)); })
{
    stationParam   = apvts.getRawParameterValue ("station");
    crossfadeParam = apvts.getRawParameterValue ("crossfade");
    volumeParam    = apvts.getRawParameterValue ("volume");
}

RadioMusicProcessor::~RadioMusicProcessor() = default;

bool RadioMusicProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void RadioMusicProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);
    volumeSmoothed.reset (sampleRate, 0.02);
    volumeSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (volumeParam->load(), -60.0f));
}

void RadioMusicProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();   // milestone 1 ignores MIDI

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numChannels < 1 || numSamples < 1)
        return;

    rm::RadioEngine::Params p;
    p.station     = stationParam->load();
    p.crossfadeMs = crossfadeParam->load();

    float* left = buffer.getWritePointer (0);
    engine.process (left, numSamples, p);

    const float dB = volumeParam->load();
    volumeSmoothed.setTargetValue (dB <= -59.9f ? 0.0f : juce::Decibels::decibelsToGain (dB, -60.0f));

    for (int i = 0; i < numSamples; ++i)
        left[i] *= volumeSmoothed.getNextValue();

    for (int ch = 1; ch < numChannels; ++ch)
        buffer.copyFrom (ch, 0, left, numSamples);
}

juce::AudioProcessorEditor* RadioMusicProcessor::createEditor()
{
    return new RadioMusicEditor (*this);
}

//==============================================================================
void RadioMusicProcessor::setSampleFolder (const juce::File& folder)
{
    apvts.state.setProperty ("sampleFolder", folder.getFullPathName(), nullptr);
    scanner.requestScan (folder);
}

juce::File RadioMusicProcessor::getSampleFolder() const
{
    const auto path = apvts.state.getProperty ("sampleFolder").toString();
    return path.isEmpty() ? juce::File() : juce::File (path);
}

std::shared_ptr<const rm::Library> RadioMusicProcessor::getLibrary() const
{
    const juce::ScopedLock sl (libraryLock);
    return library;
}

void RadioMusicProcessor::libraryReady (std::shared_ptr<const rm::Library> lib)
{
    {
        const juce::ScopedLock sl (libraryLock);
        library = lib;
    }
    engine.setLibrary (std::move (lib));
}

void RadioMusicProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void RadioMusicProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));

            const auto folder = getSampleFolder();
            if (folder != juce::File())
                scanner.requestScan (folder);
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RadioMusicProcessor();
}
