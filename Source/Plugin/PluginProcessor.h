// Radio Music (unofficial)
// An unofficial, clean-room plugin inspired by Music Thing Modular's Radio Music mk2,
// designed by Tom Whitwell with Chris Johnson of Plus Minus Audio.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Engine/RadioEngine.h"
#include "Engine/Library.h"
#include <functional>

//==============================================================================
// Scans a folder on its own thread so the user interface and audio never wait on the disk.
class LibraryScanner : private juce::Thread
{
public:
    using Callback = std::function<void (std::shared_ptr<const rm::Library>)>;

    explicit LibraryScanner (Callback onFinished);
    ~LibraryScanner() override;

    void requestScan (const juce::File& folder);

private:
    void run() override;

    Callback onFinished;
    juce::AudioFormatManager formats;
    juce::CriticalSection lock;
    juce::File pendingFolder;
    bool hasPending = false;
    std::atomic<bool> cancel { false };
};

//==============================================================================
class RadioMusicProcessor : public juce::AudioProcessor
{
public:
    RadioMusicProcessor();
    ~RadioMusicProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Radio Music Unofficial"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Folder handling (message thread)
    void setSampleFolder (const juce::File& folder);
    juce::File getSampleFolder() const;
    std::shared_ptr<const rm::Library> getLibrary() const;

    rm::RadioEngine& getEngine() { return engine; }

    // Restarts the tuned station from START (the Reset button). Safe from the message thread.
    void triggerReset() { engine.requestReset(); }

    // Output level for the meter (peak of the last block, any thread).
    float getOutputPeak() const noexcept { return outputPeak.load(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void libraryReady (std::shared_ptr<const rm::Library> lib);

    rm::RadioEngine engine;

    mutable juce::CriticalSection libraryLock;
    std::shared_ptr<const rm::Library> library;

    std::atomic<float>* stationParam   = nullptr;
    std::atomic<float>* crossfadeParam = nullptr;
    std::atomic<float>* volumeParam    = nullptr;
    std::atomic<float>* startParam     = nullptr;
    std::atomic<float>* endParam       = nullptr;
    std::atomic<float>* loopParam      = nullptr;
    std::atomic<float>* pauseParam     = nullptr;
    std::atomic<float>* resetParam     = nullptr;
    std::atomic<float>* radioStartParam = nullptr;
    std::atomic<float>* immediateParam = nullptr;
    std::atomic<float>* hostSyncParam  = nullptr;

    bool prevReset = false;
    std::atomic<float> outputPeak { 0.0f };

    juce::SmoothedValue<float> volumeSmoothed;

    LibraryScanner scanner;   // declared last: destroyed first, so it cannot call back into a half-destroyed processor

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RadioMusicProcessor)
};
