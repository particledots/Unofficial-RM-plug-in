// Radio Music (unofficial) - playback engine (milestone 3).
// Clean-room implementation from the documented behaviour of Music Thing
// Modular's Radio Music mk2 (Tom Whitwell with Chris Johnson).
//
// Threading model
//   * process() runs on the audio thread. It never allocates, locks or touches the disk.
//   * A background "stream" thread opens files and reads audio into per-voice lock-free
//     ring buffers. Only the tuned file (plus the file being crossfaded away from) is open.
//     The ring buffer holds audio in playing order, so reverse and ping-pong playback are
//     produced by the stream thread and the audio thread always reads forwards.
//   * setLibrary() / prepare() run on non-audio threads. requestReset() may be called from any thread.
#pragma once

#include "Library.h"
#include "Path.h"
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

    enum SpeedMode { SpeedTape = 0, SpeedNotes = 1, Speed90s = 2 };

    RadioEngine();
    ~RadioEngine();

    // Call when the host (re)starts audio, never from the audio thread. Resets the radio clock to 0.
    void prepare (double sessionSampleRate);

    // Installs a new folder. Safe to call from any non-audio thread, one caller at a time.
    void setLibrary (std::shared_ptr<const Library> library);

    // Restarts the tuned station from the START position. Safe from any thread.
    void requestReset() noexcept { resetFlag.store (true); }

    struct Params
    {
        float station     = 0.0f;    // 0..1, mapped across the stations in the folder
        float crossfadeMs = 25.0f;   // milliseconds
        float startNorm   = 0.0f;    // 0..1 position in the file (quantised to 1024 steps)
        float endNorm     = 1.0f;    // 0..1 position in the file (quantised to 1024 steps)
        int   loopMode    = LoopForward;
        bool  paused      = false;   // tape-style stop (speed slews to zero) and start
        bool  radioStart  = true;    // true: every file keeps advancing; false: station restarts at START when tuned
        bool  startImmediate = false; // true: START moves take effect at once instead of at the next Reset

        // Banks and settings.txt (milestone 4).
        int   bank        = 0;           // 0..15
        int   startDivider = kStartDivider;   // START is quantised to 1024 / startDivider steps (documented startCVDivider)
        bool  reselectOnTune = false;    // documented reselectSubdirOnStationChange: subfolders pick a new file when tuned to

        // Speed (affects the tuned station only; background stations always advance at 1x).
        int   speedMode   = SpeedTape;   // SpeedTape, SpeedNotes or Speed90s (90s: plays at original speed for now)
        float speedTape   = 100.0f;      // percent of original speed, -400..+400 (negative plays backwards)
        float speedNotes  = 0.0f;        // semitones, -48..+24
        bool  quantisePitch = true;      // notes mode: round to whole semitones
        bool  grit        = false;       // false: cubic interpolation. true: zero-order hold, 8-bit, ~20 kHz

        // Radio clock source. Free-running by default; with hostSync the clock follows the DAW's sample position.
        bool    hostSync    = false;
        bool    hostPlaying = true;
        int64_t hostSamples = 0;
    };

    // Audio thread. Writes numSamples of mono audio into out (overwrites it).
    void process (float* out, int numSamples, const Params& params) noexcept;

    // Read-only status for the user interface (any thread).
    int      getDisplayedStation() const noexcept  { return uiStation.load(); }   // slot on the dial in this bank, -1 = none
    int      getDisplayedFile() const noexcept     { return uiFile.load(); }      // index into Library::stations of the playing file, -1 = none
    int      getNumStations() const noexcept       { return uiCount.load(); }     // slots in this bank
    uint32_t getUnderrunCount() const noexcept     { return underruns.load(); }
    int64_t  getClockSamples() const noexcept      { return clockSamples.load(); }
    double   getClockSeconds() const noexcept      { return uiClockSeconds.load(); }
    double   getPlayPosition() const noexcept      { return uiPlayFrac.load(); }  // 0..1 in the tuned file, -1 = none
    double   getWindowLo() const noexcept          { return uiWinLo.load(); }     // 0..1
    double   getWindowHi() const noexcept          { return uiWinHi.load(); }     // 0..1
    bool     isWindowReversed() const noexcept     { return uiWinRev.load(); }

    // For tests only: jump the radio clock. Call while audio is not being processed.
    void setClockSamplesForTesting (int64_t samples) noexcept { clockSamples.store (samples); }

private:
    //==========================================================================
    static constexpr int kFifoFrames    = 1 << 17;   // frames of read-ahead per voice (mono float)
    static constexpr int kPrefillFrames = 8192;      // frames that must be buffered before a voice starts
    static constexpr int kChunkFrames   = 8192;      // frames produced per stream-thread request
    static constexpr int kCacheFrames   = 1 << 16;   // loop windows up to this long are cached by the stream thread
    static constexpr int kRenderChunk   = 256;       // audio-thread processing granularity
    static constexpr double kMaxStep    = 16.0;      // max file-rate / session-rate ratio supported
    static constexpr double kMaxSpeed   = 4.0;       // max playback speed (400%, +24 semitones)
    static constexpr double kFastFadeSeconds = 0.025;   // fade used for Reset, loop and window changes (never longer than the crossfade setting)
    static constexpr double kPauseSlewSeconds = 0.3;    // tape-style stop/start time
    static constexpr double kNotesPauseSeconds = 0.005; // notes mode stops (and starts) almost at once
    static constexpr double kSpeedSmoothSeconds = 0.02; // speed changes glide over about this long
    static constexpr double kGritRate    = 20000.0;     // grit mode output rate
    static constexpr double kGritFilterHz = 8000.0;     // grit mode post-DAC filter
    static constexpr double kSettleSeconds    = 0.04;   // a moved End (or immediate Start) must hold still this long before it is used

    static constexpr int kMaxFiles = kMaxFilesTotal;

    struct SlotInfo
    {
        int firstFile = 0;
        int numFiles  = 0;
        int mode      = SlotFile;
    };

    struct Table                      // numeric station data the audio thread may read
    {
        uint32_t id = 0;
        std::array<int, kNumBanks> bankFirst {};     // first slot of each bank
        std::array<int, kNumBanks> bankCount {};     // slots in each bank (0 = the bank does not exist)
        std::array<SlotInfo, kMaxFiles>    slots {};
        std::array<StationInfo, kMaxFiles> info {};  // per file
        std::array<uint8_t, kMaxFiles>     skip {};  // per file
    };

    // Everything that decides what a voice plays. If it changes, the engine crossfades to a new voice.
    struct Sig
    {
        int      station   = -1;
        int      startStep = 0;
        int      endStep   = 0;
        int      loopMode  = LoopForward;
        uint32_t epoch     = 0;       // bumped on Reset or a restart on station change
        int      dir       = 1;       // +1 plays along the path, -1 plays it backwards

        bool operator== (const Sig& o) const noexcept
        {
            return station == o.station && startStep == o.startStep && endStep == o.endStep
                   && loopMode == o.loopMode && epoch == o.epoch && dir == o.dir;
        }
        bool operator!= (const Sig& o) const noexcept { return ! (*this == o); }
    };

    struct Voice
    {
        Voice() : data ((size_t) kFifoFrames, 0.0f) {}

        // ---- shared between audio thread and stream thread ----
        std::atomic<uint32_t> reqGen    { 0 };
        std::atomic<int>      reqStation { -1 };
        std::atomic<int64_t>  reqLo     { 0 };
        std::atomic<int64_t>  reqHi     { 0 };
        std::atomic<int>      reqMode   { LoopForward };
        std::atomic<int>      reqRev    { 0 };
        std::atomic<int64_t>  reqTau    { 0 };
        std::atomic<int>      reqDir    { 1 };
        std::atomic<uint32_t> reqTable  { 0 };
        std::atomic<uint32_t> readyGen  { 0 };
        std::atomic<uint32_t> failedGen { 0 };
        juce::AbstractFifo    fifo      { kFifoFrames };
        std::vector<float>    data;

        // ---- stream thread only ----
        std::unique_ptr<juce::AudioFormatReader> reader;
        uint32_t handledGen = 0;
        int64_t  fileLen    = 0;
        bool     streaming  = false;
        PathCfg  scfg;
        int64_t  spos   = 0;
        bool     sback  = false;
        bool     sended = false;
        bool     cacheValid = false;
        std::vector<float> cache;

        // ---- audio thread only ----
        enum class State { Idle, Loading, Playing };
        State    state   = State::Idle;
        uint32_t gen     = 0;
        Sig      sig;
        PathCfg  cfg;
        double   fileRate = 0.0;
        int64_t  numFrames = 0;
        int64_t  tauStart = 0;
        double   step = 1.0, phase = 0.0;
        int64_t  consumed = 0;
        float    h[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // previous, current, next, next-but-one frames (for interpolation)
        float    level = 0.0f, levelTarget = 0.0f, levelInc = 0.0f;
        float    fadeInc = 0.0f;
    };

    class StreamThread;

    // audio-thread helpers
    const Table& acquireTable() noexcept;
    void invalidateAll() noexcept;
    void setIdle (Voice& v) noexcept;
    void requestVoice (Voice& v, const Sig& sig, const Table& t, int64_t clock, float fadeInc) noexcept;
    int  allocVoice() noexcept;
    void handleTargetChange (Sig desired, const Table& t, int64_t clock, float xfInc, float fastInc) noexcept;
    void tryAlign (int voiceIndex, const Table& t, int64_t clock) noexcept;
    void renderVoice (Voice& v, float* out, int n, const Table& t, int64_t clock,
                      const float* eff, double sumEff, const float* gain, bool cubic) noexcept;
    bool popFrames (Voice& v, float* dst, int num) noexcept;
    double tauFor (int station, int64_t clock, double fileRate) const noexcept;
    void publishStatus (const Table& t, int64_t clock, int desiredSlot, int desiredFile, int slotCount) noexcept;
    void reselect (int globalSlot, const Table& t) noexcept;

    // stream-thread helpers
    bool serviceVoice (Voice& v);
    void fetchFrames (Voice& v, float* dst, int64_t start, int count);
    void generate (Voice& v, int num);

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
    Sig      committedSig, pendingSig;
    int      lastDesiredSlot = -2;                 // global slot index (bank first slot + dial position)
    std::array<bool, kMaxFiles>     badStation {};   // per file
    std::array<double, kMaxFiles>   refClock {};     // per file: radio-clock sample at which its path began
    std::array<uint32_t, kMaxFiles> epochOf {};
    std::array<int, kMaxFiles>      slotCur {};      // per slot: which of its files is selected
    std::array<int, kMaxFiles>      skipLeft {};     // per slot: resets still to be ignored (skipN)
    uint32_t rngState = 0x9e3779b9u;
    std::vector<float> scratch;

    bool   settingsInit = false;
    int    latchedStart = 0, appliedEnd = 0;
    int    seenStart = 0, seenEnd = 0;
    int64_t startStable = 0, endStable = 0;

    double pauseMul = 1.0;        // 1 = playing, 0 = paused (slews)
    double speedSm  = 1.0;        // smoothed speed multiplier (negative = backwards)
    bool   speedInit = false;
    int    curDir   = 1;
    std::array<float, kRenderChunk> effPos {}, effNeg {}, gainPos {}, gainNeg {};

    double gritHold = 0.0, gritAcc = 0.0, gritLP = 0.0;
    float  gritValue = 0.0f;

    std::atomic<bool>     resetFlag { false };
    std::atomic<int64_t>  clockSamples { 0 };
    std::atomic<int>      uiStation { -1 };
    std::atomic<int>      uiFile    { -1 };
    std::atomic<int>      uiCount   { 0 };
    std::atomic<uint32_t> underruns { 0 };
    std::atomic<double>   uiClockSeconds { 0.0 };
    std::atomic<double>   uiPlayFrac { -1.0 };
    std::atomic<double>   uiWinLo { 0.0 };
    std::atomic<double>   uiWinHi { 1.0 };
    std::atomic<bool>     uiWinRev { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RadioEngine)
};
} // namespace rm
