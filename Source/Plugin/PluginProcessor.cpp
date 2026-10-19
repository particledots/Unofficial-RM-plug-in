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

    // Milestone 2. Start and End are positions in the file, 0 = start of the file, 1 = end.
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "start", 1 }, "Start",
        NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "end", 1 }, "End",
        NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "loopMode", 1 }, "Loop mode",
        StringArray { "Off", "Forward", "Ping-pong" }, 1));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "pause", 1 }, "Pause", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "reset", 1 }, "Reset", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "radioStart", 1 }, "Radio start", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "startImmediate", 1 }, "Start immediate", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "hostSync", 1 }, "Host sync", false));

    // Milestone 3. Speed affects the tuned station only. Tape: percent of original speed (negative plays backwards).
    // Notes: semitones. The 90s mode is not implemented yet and plays at original speed.
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "speedMode", 1 }, "Speed mode",
        StringArray { "Tape", "Notes", "'90s" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "speedTape", 1 }, "Speed (tape)",
        NormalisableRange<float> (-400.0f, 400.0f, 0.0f), 100.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "speedNotes", 1 }, "Pitch (notes)",
        NormalisableRange<float> (-48.0f, 24.0f, 0.0f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("st")));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "quantisePitch", 1 }, "Quantise pitch", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "gritMode", 1 }, "Grit mode", false));

    // Milestone 4. Banks 0 to 15 (root folders named with a leading number).
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "bank", 1 }, "Bank", 0, 15, 0));

    // Milestone 6. '90s mode: the speed knob is the pitch (speedTape) and this is how fast the file moves forward.
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "advance90", 1 }, "Advance rate ('90s)",
        NormalisableRange<float> (0.0f, 400.0f, 0.0f), 100.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    // Milestone 5. Tuner modes, fade curve and the radio-mode settings (names follow the documented settings.txt keys).
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "tunerMode", 1 }, "Tuner mode",
        StringArray { "Sharp", "Soft", "Radio" }, 0));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "fadeMode", 1 }, "Fade curve",
        StringArray { "Constant power", "Linear" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "noiseVol", 1 }, "Radio noise",
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "whistleVol", 1 }, "Radio whistle",
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "radioStrength", 1 }, "Radio minimum strength",
        NormalisableRange<float> (0.0f, 256.0f, 1.0f), 128.0f));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "ssbEffect", 1 }, "Radio SSB effect", false));

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
    startParam     = apvts.getRawParameterValue ("start");
    endParam       = apvts.getRawParameterValue ("end");
    loopParam      = apvts.getRawParameterValue ("loopMode");
    pauseParam     = apvts.getRawParameterValue ("pause");
    resetParam     = apvts.getRawParameterValue ("reset");
    radioStartParam = apvts.getRawParameterValue ("radioStart");
    immediateParam = apvts.getRawParameterValue ("startImmediate");
    hostSyncParam  = apvts.getRawParameterValue ("hostSync");
    speedModeParam = apvts.getRawParameterValue ("speedMode");
    speedTapeParam = apvts.getRawParameterValue ("speedTape");
    speedNotesParam = apvts.getRawParameterValue ("speedNotes");
    quantiseParam  = apvts.getRawParameterValue ("quantisePitch");
    gritParam      = apvts.getRawParameterValue ("gritMode");
    bankParam      = apvts.getRawParameterValue ("bank");
    tunerParam     = apvts.getRawParameterValue ("tunerMode");
    fadeParam      = apvts.getRawParameterValue ("fadeMode");
    noiseParam     = apvts.getRawParameterValue ("noiseVol");
    whistleParam   = apvts.getRawParameterValue ("whistleVol");
    strengthParam  = apvts.getRawParameterValue ("radioStrength");
    ssbParam       = apvts.getRawParameterValue ("ssbEffect");
    advanceParam   = apvts.getRawParameterValue ("advance90");

    startTimerHz (10);
}

RadioMusicProcessor::~RadioMusicProcessor()
{
    stopTimer();
}

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
    // MIDI: a note-on for note 36 (C1 in Logic) is a Reset, like the button. Nothing else is used.
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (msg.isNoteOn() && msg.getNoteNumber() == kResetNote)
            engine.requestReset();
    }
    midi.clear();

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numChannels < 1 || numSamples < 1)
        return;

    rm::RadioEngine::Params p;
    p.station     = stationParam->load();
    p.crossfadeMs = crossfadeParam->load();
    p.startNorm   = startParam->load();
    p.endNorm     = endParam->load();
    p.loopMode    = juce::jlimit (0, 2, (int) std::lround (loopParam->load()));
    p.paused      = pauseParam->load() > 0.5f;
    p.radioStart  = radioStartParam->load() > 0.5f;
    p.startImmediate = immediateParam->load() > 0.5f;
    p.speedMode   = juce::jlimit (0, 2, (int) std::lround (speedModeParam->load()));
    p.speedTape   = speedTapeParam->load();
    p.speedNotes  = speedNotesParam->load();
    p.advance90   = advanceParam->load();
    p.quantisePitch = quantiseParam->load() > 0.5f;
    p.grit        = gritParam->load() > 0.5f;
    p.bank        = juce::jlimit (0, rm::kNumBanks - 1, (int) std::lround (bankParam->load()));
    p.tunerMode   = juce::jlimit (0, 2, (int) std::lround (tunerParam->load()));
    p.fadeMode    = juce::jlimit (0, 1, (int) std::lround (fadeParam->load()));
    p.noiseVol    = noiseParam->load();
    p.whistleVol  = whistleParam->load();
    p.minStrength = juce::jlimit (0, 256, (int) std::lround (strengthParam->load()));
    p.ssb         = ssbParam->load() > 0.5f;
    p.startDivider = startDividerSetting.load (std::memory_order_relaxed);
    p.reselectOnTune = reselectSetting.load (std::memory_order_relaxed);

    // Reset: a rising edge on the Reset parameter (host automation) restarts the tuned station.
    const bool resetNow = resetParam->load() > 0.5f;
    if (resetNow && ! prevReset)
        engine.requestReset();
    prevReset = resetNow;

    // Host sync: the radio clock follows the DAW's sample position. If the host gives no position, stay free-running.
    if (hostSyncParam->load() > 0.5f)
    {
        if (auto* head = getPlayHead())
        {
            if (auto pos = head->getPosition())
            {
                if (auto samples = pos->getTimeInSamples())
                {
                    p.hostSync    = true;
                    p.hostPlaying = pos->getIsPlaying();
                    p.hostSamples = *samples;
                }
            }
        }
    }

    float* left = buffer.getWritePointer (0);
    engine.process (left, numSamples, p);

    const float dB = volumeParam->load();
    volumeSmoothed.setTargetValue (dB <= -59.9f ? 0.0f : juce::Decibels::decibelsToGain (dB, -60.0f));

    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        left[i] *= volumeSmoothed.getNextValue();
        peak = std::max (peak, std::fabs (left[i]));
    }
    outputPeak.store (peak, std::memory_order_relaxed);

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
    keepBankOnReady.store (false);
    applyOnReady.store (true);        // the user chose this folder: use its settings.txt (and its first bank) once it is read
    scanner.requestScan (folder);
}

void RadioMusicProcessor::rescanFolder()
{
    const auto folder = getSampleFolder();
    if (folder == juce::File())
        return;

    keepBankOnReady.store (true);
    applyOnReady.store (true);
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
    libraryChanged.store (true);
}

void RadioMusicProcessor::setParameterValue (const char* id, float realValue)
{
    if (auto* prm = apvts.getParameter (id))
        prm->setValueNotifyingHost (prm->convertTo0to1 (realValue));
}

// settings.txt gives starting values for the matching knobs. Only keys that are present are applied. Message thread.
void RadioMusicProcessor::applyBankSettings (const rm::Library& lib, int bankNumber)
{
    const auto* bank = lib.findBank (bankNumber);
    if (bank == nullptr)
        return;

    const auto& s = bank->settings;
    using namespace rm;

    if (hasSetting (s, "speedMode"))       setParameterValue ("speedMode", (float) juce::jlimit (0, 2, settingOr (s, "speedMode", 0)));
    if (hasSetting (s, "quantisePitchPot")) setParameterValue ("quantisePitch", settingOr (s, "quantisePitchPot", 1) != 0 ? 1.0f : 0.0f);
    if (hasSetting (s, "loopMode"))        setParameterValue ("loopMode", (float) juce::jlimit (0, 2, settingOr (s, "loopMode", 1)));
    if (hasSetting (s, "radioStart"))      setParameterValue ("radioStart", settingOr (s, "radioStart", 1) != 0 ? 1.0f : 0.0f);
    if (hasSetting (s, "crossfadeTime"))   setParameterValue ("crossfade", (float) juce::jlimit (1, 60000, settingOr (s, "crossfadeTime", 25)));
    if (hasSetting (s, "highQuality"))     setParameterValue ("gritMode", settingOr (s, "highQuality", 1) != 0 ? 0.0f : 1.0f);
    if (hasSetting (s, "startPotImmediate")) setParameterValue ("startImmediate", settingOr (s, "startPotImmediate", 0) != 0 ? 1.0f : 0.0f);
    if (hasSetting (s, "tunerMode"))       setParameterValue ("tunerMode", (float) juce::jlimit (0, 2, settingOr (s, "tunerMode", 0)));
    if (hasSetting (s, "fadeMode"))        setParameterValue ("fadeMode", settingOr (s, "fadeMode", 0) != 0 ? 1.0f : 0.0f);
    if (hasSetting (s, "noiseVol"))        setParameterValue ("noiseVol", (float) juce::jlimit (0, 100, settingOr (s, "noiseVol", 50)));
    if (hasSetting (s, "whistleVol"))      setParameterValue ("whistleVol", (float) juce::jlimit (0, 100, settingOr (s, "whistleVol", 50)));
    if (hasSetting (s, "minRadioStationStrength")) setParameterValue ("radioStrength", (float) juce::jlimit (0, 256, settingOr (s, "minRadioStationStrength", 128)));
    if (hasSetting (s, "ssbEffect"))       setParameterValue ("ssbEffect", settingOr (s, "ssbEffect", 0) != 0 ? 1.0f : 0.0f);
}

// Settings that have no knob always follow the files on disk (the folder's own settings.txt for the current bank).
void RadioMusicProcessor::updateRuntimeSettings (const rm::Library* lib, int bankNumber)
{
    int divider = rm::kStartDivider;
    bool reselect = false;

    if (lib != nullptr)
        if (const auto* bank = lib->findBank (bankNumber))
        {
            divider  = juce::jlimit (1, rm::kStartSteps, rm::settingOr (bank->settings, "startCVDivider", rm::kStartDivider));
            reselect = rm::settingOr (bank->settings, "reselectSubdirOnStationChange", 0) != 0;
        }

    startDividerSetting.store (divider);
    reselectSetting.store (reselect);
}

void RadioMusicProcessor::timerCallback()
{
    int bank = juce::jlimit (0, rm::kNumBanks - 1, (int) std::lround (bankParam->load()));

    if (libraryChanged.exchange (false))
    {
        const auto lib = getLibrary();

        if (lib != nullptr && applyOnReady.exchange (false))
        {
            const bool keep = keepBankOnReady.exchange (false) && lib->findBank (bank) != nullptr;
            if (! keep)
                bank = lib->lowestBankNumber();
            setParameterValue ("bank", (float) bank);
            lastBank = bank;
            applyBankSettings (*lib, bank);
        }

        updateRuntimeSettings (lib.get(), bank);
    }

    if (bank != lastBank)            // the bank was changed by the user or by automation
    {
        lastBank = bank;
        const auto lib = getLibrary();
        if (lib != nullptr)
            applyBankSettings (*lib, bank);
        updateRuntimeSettings (lib.get(), bank);
    }
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

            // A restored project keeps its own knob values: do not apply settings.txt because the bank "changed".
            lastBank = juce::jlimit (0, rm::kNumBanks - 1, (int) std::lround (bankParam->load()));
            applyOnReady.store (false);

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
