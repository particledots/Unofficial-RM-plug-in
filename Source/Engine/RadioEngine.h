// Radio Music (unofficial) - playback engine (milestone 1).
// Clean-room implementation from the documented behaviour of Music Thing
// Modular's Radio Music mk2 (Tom Whitwell with Chris Johnson).
//
// Threading model
//   * process() runs on the audio thread. It never allocates, locks or touches the disk.
//   * A background "stream" thread opens files and reads audio into per-voice lock-free
//     ring buffers. Only the tuned file (plus the file being crossfaded away from) is open.
//   * setLibrary() / prepare() run on non-audio threads.
#pragma once

#include "Library.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace rm
{
class RadioEngine
{
public:
    static constexpr int kNumVoices = 4;

    RadioEngine();
    ~RadioEngine();

    // Call when the host (re)starts audio, never from the audio thread. Resets the radio clock to 0.
    void prepare (double sessionSampleRate);

    // Installs a new folder. Safe to call from any non-audio thread, one caller at a time.
    void setLibrary (std::shared_ptr<const Library> library);

    struct Params
    {
        float station     = 0.0f;    // 0..1, mapped across the stations in the folder
        float crossfadeMs = 25.0f;   // milliseconds
    };

    // Audio thread. Writes numSamples of mono audio into out (overwrites it).
    void process (float* out, int numSamples, const Params& params) noexcept;

    // Read-only status for the user interface (any thread).
    int      getDisplayedStation() const noexcept  { return uiStation.load(); }   // -1 = none
    int      getNumStations() const noexcept       { return uiCount.load(); }
    uint32_t getUnderrunCount() const noexcept     { return underruns.load(); }
    int64_t  getClockSamples() const noexcept      { return clockSamples.load(); }
    double   getClockSeconds() const noexcept      { return uiClockSeconds.load(); }

    // For tests only: jump the radio clock. Call while audio is not being processed.
    void setClockSamplesForTesting (int64_t samples) noexcept { clockSamples.store (samples); }

private:
    //==========================================================================
    static constexpr int kFifoFrames    = 1 << 17;   // frames of read-ahead per voice (mono float)
    static constexpr int kPrefillFrames = 8192;      // frames that must be buffered before a voice starts
    static constexpr int kChunkFrames   = 8192;      // frames read from disk per request
    static constexpr int kRenderChunk   = 256;       // audio-thread processing granularity
    static constexpr double kMaxStep    = 16.0;      // max file-rate / session-rate ratio supported

    struct Table                      // numeric station data the audio thread may read
    {
        uint32_t id    = 0;
        int      count = 0;
        std::array<StationInfo, kMaxStationsPerBank> info {};
    };

    struct Voice
    {
        Voice() : data ((size_t) kFifoFrames, 0.0f) {}

        // ---- shared between audio thread and stream thread ----
        std::atomic<uint32_t> reqGen    { 0 };
        std::atomic<int>      reqStation { -1 };
        std::atomic<int64_t>  reqStart  { 0 };
        std::atomic<uint32_t> reqTable  { 0 };
        std::atomic<uint32_t> readyGen  { 0 };
        std::atomic<uint32_t> failedGen { 0 };
        juce::AbstractFifo    fifo      { kFifoFrames };
        std::vector<float>    data;

        // ---- stream thread only ----
        std::unique_ptr<juce::AudioFormatReader> reader;
        uint32_t handledGen = 0;
        int64_t  readPos    = 0;
        int64_t  fileLen    = 0;
        bool     streaming  = false;

        // ---- audio thread only ----
        enum class State { Idle, Loading, Playing };
        State    state   = State::Idle;
        uint32_t gen     = 0;
        int      station = -1;
        double   fileRate = 0.0;
        int64_t  numFrames = 0;
        int64_t  seekStart = 0;
        double   step = 1.0, phase = 0.0;
        int64_t  consumed = 0;
        float    s0 = 0.0f, s1 = 0.0f;
        float    level = 0.0f, levelTarget = 0.0f, levelInc = 0.0f;
    };

    class StreamThread;

    // audio-thread helpers
    const Table& acquireTable() noexcept;
    void invalidateAll() noexcept;
    void setIdle (Voice& v) noexcept;
    void requestVoice (Voice& v, int station, const Table& t, int64_t clockAtChunkStart) noexcept;
    int  allocVoice() noexcept;
    void handleTargetChange (int desired, const Table& t, int64_t clockStart, float xfInc) noexcept;
    void tryAlign (int voiceIndex, const Table& t, int64_t clockStart, float xfInc) noexcept;
    void renderVoice (Voice& v, float* out, int n, const Table& t, int64_t clockStart) noexcept;
    bool popFrames (Voice& v, float* dst, int num) noexcept;
    static double expectedFrame (double fileRate, int64_t numFrames, int64_t clock, double sessionRate) noexcept;

    // stream-thread helper
    bool serviceVoice (Voice& v);

    //==========================================================================
    std::array<Voice, kNumVoices> voices;

    std::array<Table, 3> tables;
    std::atomic<int>     activeTable  { 0 };
    std::atomic<int>     readingTable { -1 };
    juce::CriticalSection setLock;
    uint32_t              nextLibId = 0;

    juce::CriticalSection          libLock;
    std::shared_ptr<const Library> streamLib;
    uint32_t                       streamLibId = 0;
    juce::AudioFormatManager       streamFormats;   // used by the stream thread only
    std::vector<float>             streamTmp;       // used by the stream thread only

    std::unique_ptr<StreamThread> streamThread;

    // audio-thread state
    double   sessionRate = 44100.0;
    uint32_t knownTableId = 0;
    int      current = -1, pending = -1;
    int      committedStation = -1, pendingStation = -1;
    std::array<bool, kMaxStationsPerBank> badStation {};
    std::vector<float> scratch;

    std::atomic<int64_t>  clockSamples { 0 };
    std::atomic<int>      uiStation { -1 };
    std::atomic<int>      uiCount   { 0 };
    std::atomic<uint32_t> underruns { 0 };
    std::atomic<double>   uiClockSeconds { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RadioEngine)
};
} // namespace rm
