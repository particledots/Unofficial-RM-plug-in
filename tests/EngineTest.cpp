// Offline-ish test of the Radio Music engine. Generates its own audio files, runs the engine
// paced in real time (the stream thread needs real time to read from disk) and checks:
//   * folder scanning rules,
//   * every station's playhead follows one global clock, including after tuning away and back,
//   * files at different sample rates play at the right speed in the session rate,
//   * the left channel of a stereo file is used,
//   * crossfades are smooth and about the requested length,
//   * a very long file is streamed, not loaded into RAM,
//   * (milestone 3) tape and notes speed (forwards and backwards), cubic interpolation, grit mode,
//     pause fading to silence, background stations staying at 1x,
//   * (milestone 2) loop window, reverse, ping-pong, loop off, Reset, Start latching, radio start off,
//     Pause, and host sync all follow the documented behaviour.
#include "Engine/RadioEngine.h"
#include "Engine/LibraryRules.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>

static int failures = 0;
#define CHECK(cond, msg)                                                                         \
    do { if (! (cond)) { ++failures; std::cout << "  FAIL: " << msg << "  [" #cond "]\n"; }       \
         else          { std::cout << "  ok:   " << msg << "\n"; } } while (0)

using namespace rm;

static double residentMB()
{
    std::ifstream f ("/proc/self/status");
    std::string line;
    while (std::getline (f, line))
        if (line.rfind ("VmRSS:", 0) == 0)
            return std::stod (line.substr (6)) / 1024.0;
    return -1.0;
}

// Writes a mono (or stereo) file whose sample value at frame i is amp * ((i % len) / len). Stereo files get the
// negated ramp on the right channel, so a test can tell which channel was played.
static void writeRamp (const juce::File& file, double rate, double seconds, int bits, int channels, float amp = 0.9f)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
    auto w = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (rate)
                                                                         .withNumChannels (channels)
                                                                         .withBitsPerSample (bits));

    const int64_t len = (int64_t) std::llround (seconds * rate);
    juce::AudioBuffer<float> buf (channels, 65536);
    for (int64_t pos = 0; pos < len; pos += 65536)
    {
        const int n = (int) std::min<int64_t> (65536, len - pos);
        for (int i = 0; i < n; ++i)
        {
            const float v = amp * (float) ((double) (pos + i) / (double) len);
            buf.setSample (0, i, v);
            if (channels > 1) buf.setSample (1, i, -v);
        }
        w->writeFromAudioSampleBuffer (buf, 0, n);
    }
}

static void writeDC (const juce::File& file, double rate, double seconds, float value)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
    auto w = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (rate)
                                                                         .withNumChannels (1)
                                                                         .withBitsPerSample (24));
    juce::AudioBuffer<float> buf (1, 4096);
    for (int i = 0; i < 4096; ++i) buf.setSample (0, i, value);
    const int64_t len = (int64_t) (seconds * rate);
    for (int64_t pos = 0; pos < len; pos += 4096)
        w->writeFromAudioSampleBuffer (buf, 0, (int) std::min<int64_t> (4096, len - pos));
}

static std::shared_ptr<Library> scanFolder (const juce::File& f)
{
    juce::AudioFormatManager fm;
    Library::registerSupportedFormats (fm);
    std::atomic<bool> cancel { false };
    return Library::scan (f, fm, cancel);
}

// Expected ramp value for a station at a given radio-clock sample.
static double expectedRamp (const StationInfo& s, int64_t clock, double sessionRate, float amp = 0.9f)
{
    const double pos = std::fmod ((double) clock * (s.sampleRate / sessionRate), (double) s.numFrames);
    return amp * pos / (double) s.numFrames;
}

// Circular difference between two ramp values, in frames of the file.
static double rampErrorFrames (const StationInfo& s, double a, double b, float amp = 0.9f)
{
    double d = std::fabs (a - b) / amp;
    d = std::min (d, 1.0 - d);
    return d * (double) s.numFrames;
}

struct Runner
{
    RadioEngine& engine;
    double sr;
    int block = 512;
    std::vector<float> out = std::vector<float> (4096);

    // Runs real-time paced. If check is set, it is called after every block with (firstClockSample, samples).
    template <typename F>
    void run (double seconds, RadioEngine::Params& p, F&& check)
    {
        const auto t0 = std::chrono::steady_clock::now();
        const int blocks = (int) (seconds * sr / block);
        for (int b = 0; b < blocks; ++b)
        {
            const int64_t clock = engine.getClockSamples();
            engine.process (out.data(), block, p);
            check (clock, out.data(), block);
            std::this_thread::sleep_until (t0 + std::chrono::duration<double> ((b + 1) * block / sr));
        }
    }
    void run (double seconds, RadioEngine::Params& p)
    {
        run (seconds, p, [] (int64_t, const float*, int) {});
    }
};

static float stationParam (int index, int count) { return ((float) index + 0.5f) / (float) count; }

static void testAtRate (double sr, const juce::File& dir)
{
    std::cout << "\n=== Session rate " << sr << " Hz ===\n";
    auto lib = scanFolder (dir);
    CHECK (lib->stations.size() == 4, "scan found 4 stations (hidden/underscore/non-audio files ignored)");
    if (lib->stations.size() != 4) return;
    CHECK (lib->stations[0].file.getFileName() == "01_a.wav" && lib->stations[3].file.getFileName() == "04_short.wav",
           "stations sorted by file name");

    RadioEngine engine;
    engine.prepare (sr);
    engine.setLibrary (lib);
    Runner r { engine, sr };

    const int N = 4;
    RadioEngine::Params p;
    p.crossfadeMs = 25.0f;

    auto checkStation = [&] (int s, const char* label)
    {
        double worst = 0.0; int blocks = 0;
        r.run (0.4, p, [&] (int64_t clock, const float* o, int n)
        {
            if (clock < 0) return;
            if (++blocks < 2) return;                 // settle
            for (int i = 0; i < n; i += 32)
            {
                const double e = expectedRamp (lib->stations[(size_t) s].info, clock + i, sr);
                worst = std::max (worst, rampErrorFrames (lib->stations[(size_t) s].info, o[i], e));
            }
        });
        std::cout << "    worst position error: " << worst << " file frames\n";
        CHECK (worst < 6.0, label);
    };

    // Station 0 (44.1 kHz): follow the clock from the start.
    p.station = stationParam (0, N);
    r.run (0.5, p);
    checkStation (0, "station 0 (44.1 kHz file) tracks the global clock");

    // Station 1 (48 kHz).
    p.station = stationParam (1, N);
    r.run (0.3, p);
    checkStation (1, "station 1 (48 kHz file) tracks the global clock after tuning to it");

    // Station 2 (96 kHz stereo): left channel only.
    p.station = stationParam (2, N);
    r.run (0.3, p);
    checkStation (2, "station 2 (96 kHz stereo file) plays the left channel at the right speed");

    // Station 3 (short, 22.05 kHz) wraps many times.
    p.station = stationParam (3, N);
    r.run (0.3, p);
    checkStation (3, "station 3 (0.5 s file) wraps and stays on the clock");

    // Tune away to 3 for a while, then back to 0: station 0 must have kept moving in the background.
    p.station = stationParam (0, N);
    r.run (0.3, p);
    checkStation (0, "back on station 0 after 1+ s away: it advanced in the background");

    CHECK (engine.getUnderrunCount() == 0, "no buffer underruns (count = " + std::to_string (engine.getUnderrunCount()) + ")");
}

static void testCrossfade (const juce::File& dir)
{
    std::cout << "\n=== Crossfade ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    CHECK (lib->stations.size() == 2, "two DC stations found");
    RadioEngine engine;
    engine.prepare (sr);
    engine.setLibrary (lib);
    Runner r { engine, sr, 128 };

    RadioEngine::Params p;
    p.crossfadeMs = 100.0f;
    p.station = 0.25f;                 // station 0 (value 0.5)
    r.run (0.5, p);

    std::vector<float> trace;
    p.station = 0.75f;                 // station 1 (value 0.25)
    r.run (0.6, p, [&] (int64_t, const float* o, int n) { trace.insert (trace.end(), o, o + n); });

    int first = -1, last = -1;
    float maxStep = 0.0f;
    for (size_t i = 0; i < trace.size(); ++i)
    {
        if (first < 0 && std::fabs (trace[i] - 0.5f) > 0.002f) first = (int) i;
        if (std::fabs (trace[i] - 0.25f) > 0.002f) last = (int) i;
        if (i > 0) maxStep = std::max (maxStep, std::fabs (trace[i] - trace[i - 1]));
    }
    const double lenMs = (last - first) * 1000.0 / sr;
    std::cout << "    crossfade measured " << lenMs << " ms (requested 100), largest sample step " << maxStep << "\n";
    CHECK (first >= 0 && last > first, "crossfade happened");
    CHECK (lenMs > 90.0 && lenMs < 110.0, "crossfade length is about 100 ms");
    CHECK (maxStep < 0.002f, "no clicks: sample-to-sample step stays tiny");
    CHECK (std::fabs (trace.back() - 0.25f) < 0.002f, "ends fully on the new station");
    // Constant power: with fully correlated DC sources the level mid-fade rises above the linear mean.
    float mid = trace[(size_t) ((first + last) / 2)];
    CHECK (mid > 0.375f - 0.02f, "constant-power shape (mid-fade level above/at linear midpoint)");
}

static void testEdgeCases (const juce::File& root)
{
    std::cout << "\n=== Empty / missing / swapped folders ===\n";
    const double sr = 44100.0;
    RadioEngine engine;
    engine.prepare (sr);
    Runner r { engine, sr };
    RadioEngine::Params p; p.station = 0.5f;

    float peak = 0.0f;
    r.run (0.2, p, [&] (int64_t, const float* o, int n) { for (int i = 0; i < n; ++i) peak = std::max (peak, std::fabs (o[i])); });
    CHECK (peak < 1.0e-9f, "no library installed: silence, no crash");

    engine.setLibrary (scanFolder (root.getChildFile ("does_not_exist")));
    r.run (0.2, p);
    CHECK (engine.getNumStations() == 0, "missing folder gives zero stations");

    auto emptyDir = root.getChildFile ("empty"); emptyDir.createDirectory();
    engine.setLibrary (scanFolder (emptyDir));
    r.run (0.2, p);
    CHECK (engine.getNumStations() == 0, "empty folder gives zero stations");

    auto dcDir = root.getChildFile ("dc");
    auto lib = scanFolder (dcDir);
    engine.setLibrary (lib);
    float last = 0.0f;
    r.run (0.5, p, [&] (int64_t, const float* o, int n) { last = o[n - 1]; });
    CHECK (std::fabs (last - 0.25f) < 0.002f, "installing a library while running starts playing it");
}


static void testStress (const juce::File& ramps, const juce::File& dc)
{
    std::cout << "\n=== Stress: wild automation and folder swapping ===\n";
    const double sr = 48000.0;
    auto libA = scanFolder (ramps);
    auto libB = scanFolder (dc);
    RadioEngine engine;
    engine.prepare (sr);
    engine.setLibrary (libA);
    Runner r { engine, sr, 64 };
    juce::Random rnd (1234);
    RadioEngine::Params p;
    bool bad = false;
    int blockCount = 0;

    r.run (6.0, p, [&] (int64_t, const float* o, int n)
    {
        for (int i = 0; i < n; ++i)
            if (! std::isfinite (o[i]) || std::fabs (o[i]) > 3.0f)
            {
                if (! bad) std::cout << "    first bad sample: " << o[i] << " at block " << blockCount << " index " << i << "\n";
                bad = true;
            }
        ++blockCount;
        p.station = rnd.nextFloat();                          // a new random station nearly every block
        p.crossfadeMs = (blockCount % 50 == 0) ? 500.0f : 1.0f + rnd.nextFloat() * 40.0f;
        if (blockCount % 200 == 0)
            engine.setLibrary ((blockCount / 200) % 2 ? libB : libA);   // swap folders under load
    });
    CHECK (! bad, "output stays finite and bounded under random automation and folder swaps");
    std::cout << "    underruns during the stress run: " << engine.getUnderrunCount() << " (informational)\n";
}

static void testLongFile (const juce::File& dir)
{
    std::cout << "\n=== Very long file streams without loading into memory ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    CHECK (lib->stations.size() == 1, "long file found");
    if (lib->stations.empty()) return;
    const auto size = lib->stations[0].file.getSize();
    std::cout << "    file: " << lib->stations[0].info.numFrames / lib->stations[0].info.sampleRate / 60.0 << " minutes, "
              << size / (1024.0 * 1024.0) << " MB on disk\n";

    const double before = residentMB();
    RadioEngine engine;
    engine.prepare (sr);
    engine.setClockSamplesForTesting ((int64_t) (20.0 * 60.0 * sr));   // pretend the radio has been on for 20 minutes
    engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p; p.station = 0.5f;

    // The playhead should join the file about 20 minutes in, without reading the 20 minutes before it.
    double worst = 0.0; int blocks = 0;
    r.run (3.0, p, [&] (int64_t clock, const float* o, int n)
    {
        if (++blocks < 8) return;
        const double e = expectedRamp (lib->stations[0].info, clock + n / 2, sr);
        worst = std::max (worst, rampErrorFrames (lib->stations[0].info, o[n / 2], e));
    });
    const double after = residentMB();
    std::cout << "    resident memory " << before << " MB -> " << after << " MB, worst position error " << worst << " frames\n";
    if (before < 0.0)
        std::cout << "  skip: memory check (only available on Linux in this test)\n";
    else
        CHECK (after - before < 40.0, "memory growth is far smaller than the file (streamed, not loaded)");
    CHECK (worst < 40.0, "long file joins ~20 minutes in and stays on the clock (24-bit quantisation allows ~11 frames)");
}

//==============================================================================
// Milestone 2 tests

// Follows one station's path: the output must equal the ramp value at the file frame the path says we are on.
// tau = (clock - refClock) * fileRate / sessionRate. Returns the fraction of checked samples that were off by more than tolFrames.
struct PathTrack { int checked = 0; int bad = 0; double badFraction() const { return checked ? (double) bad / checked : 1.0; } };

template <typename ClockRef>
static PathTrack trackPath (Runner& r, RadioEngine::Params& p, const StationInfo& info, const PathCfg& cfg,
                            ClockRef refClock, double seconds, double skipSeconds, double tolFrames = 8.0)
{
    PathTrack t;
    const double sr = r.sr;
    int64_t firstClock = -1;
    r.run (seconds, p, [&] (int64_t clock, const float* o, int n)
    {
        if (firstClock < 0) firstClock = clock;
        if ((double) (clock - firstClock) < skipSeconds * sr) return;
        for (int i = 0; i < n; i += 16)
        {
            const double tau = ((double) (clock + i) - refClock()) * (info.sampleRate / sr);
            const auto pt = pathAt (cfg, (int64_t) std::floor (tau));
            const double e = pt.ended ? 0.0 : 0.9 * (double) pt.pos / (double) info.numFrames;   // loop off: silence once ended
            const double errFrames = std::fabs ((double) o[i] - e) / 0.9 * (double) info.numFrames;
            ++t.checked;
            if (errFrames > tolFrames) ++t.bad;
        }
    });
    return t;
}

static PathCfg cfgFromParams (const RadioEngine::Params& p, const StationInfo& info)
{
    return makeCfg (quantiseStep (p.startNorm, kStartDivider), quantiseStep (p.endNorm, 1), info.numFrames, p.loopMode);
}

static void testPathMath()
{
    std::cout << "\n=== Path arithmetic ===\n";
    const int64_t N = 100000;
    CHECK (quantiseStep (0.5f, 2) == 512 && quantiseStep (1.0f, 2) == 1024 && quantiseStep (0.0f, 2) == 0, "quantiseStep end points");
    CHECK (quantiseStep (0.5009f, 2) % 2 == 0, "START is rounded down to a multiple of startCVDivider (2)");
    CHECK (quantiseStep (0.5009f, 1) == 512, "divider 1 gives plain 1024 steps");

    auto whole1 = makeCfg (0, 1024, N, LoopForward);
    auto whole2 = makeCfg (0, 0, N, LoopForward);
    auto whole3 = makeCfg (400, 400, N, LoopForward);
    CHECK (whole1.lo == 0 && whole1.hi == N && ! whole1.rev, "START fully left and END at the right edge: whole sample");
    CHECK (whole2.lo == 0 && whole2.hi == N && ! whole2.rev, "START fully left and END at the left edge: whole sample");
    CHECK (whole3.lo == 0 && whole3.hi == N && ! whole3.rev, "START equal to END: whole sample");
    auto rev = makeCfg (800, 200, N, LoopForward);
    CHECK (rev.rev && rev.lo == N * 200 / 1024 && rev.hi == N * 800 / 1024, "START right of END: reversed window");

    const PathCfg f { 1000, 1100, false, LoopForward };
    CHECK (pathAt (f, 0).pos == 1000 && pathAt (f, 99).pos == 1099 && pathAt (f, 100).pos == 1000, "forward loop wraps to the start of the window");
    const PathCfg b { 1000, 1100, true, LoopForward };
    CHECK (pathAt (b, 0).pos == 1099 && pathAt (b, 99).pos == 1000 && pathAt (b, 100).pos == 1099, "reversed loop runs backwards and wraps");
    const PathCfg pp { 1000, 1100, false, LoopPingPong };
    CHECK (pathAt (pp, 99).pos == 1099 && ! pathAt (pp, 99).back && pathAt (pp, 100).pos == 1099 && pathAt (pp, 100).back
           && pathAt (pp, 199).pos == 1000 && pathAt (pp, 200).pos == 1000 && ! pathAt (pp, 200).back, "ping-pong alternates forwards and backwards");
    const PathCfg off { 1000, 1100, false, LoopOff };
    CHECK (! pathAt (off, 99).ended && pathAt (off, 100).ended && pathAt (off, 5000).ended, "loop off ends at the end of the window");
}

// Creates a fresh engine and runs a loop-window case.
static void testWindowCase (const juce::File& dir, const char* name, float start, float end, int mode, double seconds, double skip)
{
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p;
    p.station = 0.5f; p.startNorm = start; p.endNorm = end; p.loopMode = mode;
    const auto info = lib->stations[0].info;
    const auto cfg = cfgFromParams (p, info);
    const auto res = trackPath (r, p, info, cfg, [] { return 0.0; }, seconds, skip);
    std::cout << "    " << res.bad << " of " << res.checked << " checks off the expected path\n";
    CHECK (res.checked > 1000 && res.badFraction() < 0.01, name);
    CHECK (engine.getUnderrunCount() == 0, std::string (name) + ": no underruns");
}

static void testWindows (const juce::File& dir)
{
    std::cout << "\n=== Start/End window and loop modes ===\n";
    testWindowCase (dir, "forward loop inside Start..End plays the window and wraps", 0.25f, 0.30f, LoopForward, 3.0, 0.2);
    testWindowCase (dir, "START right of END plays the window in reverse", 0.5f, 0.25f, LoopForward, 3.0, 0.2);
    testWindowCase (dir, "ping-pong alternates forwards and backwards", 0.25f, 0.30f, LoopPingPong, 3.0, 0.2);
    testWindowCase (dir, "loop off stops at the end of the window (then silence)", 0.25f, 0.30f, LoopOff, 1.5, 0.2);
    testWindowCase (dir, "short window (about 20 ms) loops accurately from the cache", 0.2f, 0.202f, LoopForward, 1.5, 0.2);
    testWindowCase (dir, "whole sample when START is fully left and END at the right", 0.0f, 1.0f, LoopForward, 1.5, 0.2);
}

static void testLoopOffSilence (const juce::File& dir)
{
    std::cout << "\n=== Loop off: playback ceases ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p; p.station = 0.5f; p.startNorm = 0.25f; p.endNorm = 0.30f; p.loopMode = LoopOff;
    float peakLate = 0.0f; int blocks = 0;
    r.run (2.0, p, [&] (int64_t clock, const float* o, int n)
    {
        ++blocks;
        if ((double) clock > 0.9 * sr)                          // window is 0.5 s long
            for (int i = 0; i < n; ++i) peakLate = std::max (peakLate, std::fabs (o[i]));
    });
    CHECK (peakLate < 1.0e-6f, "after the window ends the output is silent");

    // A Reset restarts playback.
    engine.requestReset();
    float peakAfter = 0.0f;
    r.run (0.3, p, [&] (int64_t, const float* o, int n) { for (int i = 0; i < n; ++i) peakAfter = std::max (peakAfter, std::fabs (o[i])); });
    CHECK (peakAfter > 0.1f, "Reset restarts a station that has stopped");
}

static void testReset (const juce::File& dir)
{
    std::cout << "\n=== Reset, START latching ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p; p.station = 0.5f; p.startNorm = 0.25f; p.endNorm = 1.0f;
    const auto info = lib->stations[0].info;
    double ref = 0.0;

    auto cfg = cfgFromParams (p, info);
    auto res = trackPath (r, p, info, cfg, [&] { return ref; }, 1.0, 0.2);
    CHECK (res.badFraction() < 0.01, "before any Reset the station follows the clock from START 0.25");

    p.startNorm = 0.5f;                                       // moved, but no Reset yet
    res = trackPath (r, p, info, cfg, [&] { return ref; }, 0.6, 0.2);
    CHECK (res.badFraction() < 0.01, "moving START without Reset changes nothing (START only updates on reset)");

    ref = (double) engine.getClockSamples();                  // the Reset happens at the start of the next block
    engine.requestReset();
    auto cfg2 = cfgFromParams (p, info);
    res = trackPath (r, p, info, cfg2, [&] { return ref; }, 1.0, 0.3);
    std::cout << "    " << res.bad << " of " << res.checked << " checks off the expected path\n";
    CHECK (res.badFraction() < 0.01, "Reset restarts the station at the new START position and it plays on from there");

    // Immediate START: changes take effect without a Reset (after a short settle).
    p.startImmediate = true; p.startNorm = 0.75f;
    auto cfg3 = cfgFromParams (p, info);
    res = trackPath (r, p, info, cfg3, [&] { return ref; }, 1.0, 0.4);
    CHECK (res.badFraction() < 0.01, "with 'immediate' on, START moves take effect straight away");
    CHECK (engine.getUnderrunCount() == 0, "no underruns");
}

static void testResetOnlyTunedStation (const juce::File& dir)
{
    std::cout << "\n=== Reset only restarts the tuned station ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    CHECK (lib->stations.size() == 2, "two stations");
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p; p.station = 0.25f;
    const auto info = lib->stations[0].info;
    const auto cfg = cfgFromParams (p, info);
    double ref0 = 0.0;
    r.run (1.0, p);
    ref0 = (double) engine.getClockSamples();
    engine.requestReset();
    auto res = trackPath (r, p, info, cfg, [&] { return ref0; }, 0.8, 0.3);
    CHECK (res.badFraction() < 0.01, "station 0 restarted from its beginning");

    p.station = 0.75f;                                        // station 1 was not reset: it is where the clock says
    r.run (0.3, p);
    res = trackPath (r, p, lib->stations[1].info, cfg, [] { return 0.0; }, 0.5, 0.1);
    CHECK (res.badFraction() < 0.01, "station 1 still follows the global clock (it was not reset)");
}

static void testRadioStartOff (const juce::File& dir)
{
    std::cout << "\n=== radioStart off: tuning restarts the station ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p; p.station = 0.25f; p.radioStart = false;
    const auto info = lib->stations[0].info;
    const auto cfg = cfgFromParams (p, info);
    r.run (1.0, p);

    p.station = 0.75f;
    double ref = (double) engine.getClockSamples();
    auto res = trackPath (r, p, lib->stations[1].info, cfg, [&] { return ref; }, 0.8, 0.3);
    CHECK (res.badFraction() < 0.01, "station 1 starts from the beginning when tuned");

    p.station = 0.25f;
    ref = (double) engine.getClockSamples();
    res = trackPath (r, p, info, cfg, [&] { return ref; }, 0.8, 0.3);
    CHECK (res.badFraction() < 0.01, "station 0 starts again from the beginning when tuned back (it did not keep running)");
}

static void testPause (const juce::File& dir)
{
    std::cout << "\n=== Pause (tape-style) ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p; p.station = 0.5f;
    const auto info = lib->stations[0].info;
    r.run (1.0, p);

    const int64_t pauseAt = engine.getClockSamples();
    p.paused = true;
    r.run (0.6, p);                                          // slews to a stop in 0.3 s

    float lo = 10.0f, hi = -10.0f;
    r.run (0.5, p, [&] (int64_t, const float* o, int n) { for (int i = 0; i < n; ++i) { lo = std::min (lo, o[i]); hi = std::max (hi, o[i]); } });
    std::cout << "    held value spread while paused: " << (hi - lo) << "\n";
    CHECK (hi - lo < 1.0e-5f, "paused: the sample is held (speed has slewed to zero)");

    r.run (0.4, p);
    const int64_t resumeAt = engine.getClockSamples();
    p.paused = false;
    float last = 0.0f; int64_t lastClock = 0;
    r.run (1.0, p, [&] (int64_t clock, const float* o, int n) { last = o[n - 1]; lastClock = clock + n - 1; });

    const double pos = (double) last / 0.9 * (double) info.numFrames;
    const double lag = (double) lastClock - pos;
    const double expected = (double) (resumeAt - pauseAt);
    std::cout << "    lag behind the radio clock " << lag / sr << " s, expected about " << expected / sr << " s\n";
    CHECK (std::fabs (lag - expected) < 0.03 * sr, "after resuming, the station is behind the clock by the time it was paused");

    // Other stations are not delayed by the pause: nothing to check with one file, so check the rate instead.
    float a = 0.0f, b = 0.0f; int cnt = 0;
    r.run (0.5, p, [&] (int64_t, const float* o, int n) { if (cnt == 0) a = o[0]; b = o[n - 1]; ++cnt; });
    const double adv = (double) (b - a) / 0.9 * (double) info.numFrames;
    const double elapsed = (double) cnt * 512.0 - 1.0;
    CHECK (std::fabs (adv - elapsed) < 0.01 * elapsed, "after resuming it plays at normal speed");
}

static void testHostSync (const juce::File& dir)
{
    std::cout << "\n=== Host sync ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    const auto info = lib->stations[0].info;
    RadioEngine::Params p; p.station = 0.5f; p.hostSync = true; p.hostPlaying = true; p.hostSamples = 100000;
    const auto cfg = cfgFromParams (p, info);

    auto follow = [&] (double seconds, double skip)
    {
        PathTrack t;
        int64_t first = -1;
        r.run (seconds, p, [&] (int64_t, const float* o, int n)
        {
            if (first < 0) first = p.hostSamples;
            const int64_t blockStart = p.hostSamples;           // what the host reported for this block
            if ((double) (blockStart - first) >= skip * sr)
                for (int i = 0; i < n; i += 16)
                {
                    const auto pt = pathAt (cfg, blockStart + i);
                    const double e = 0.9 * (double) pt.pos / (double) info.numFrames;
                    ++t.checked;
                    if (std::fabs ((double) o[i] - e) / 0.9 * (double) info.numFrames > 8.0) ++t.bad;
                }
            p.hostSamples += n;                                  // the host moves on by one block
        });
        return t;
    };

    auto t = follow (1.0, 0.2);
    CHECK (t.badFraction() < 0.01, "audio is derived from the host position");
    CHECK (std::llabs (engine.getClockSamples() - p.hostSamples) <= 512, "radio clock equals the host position");

    p.hostSamples = 240000;                                      // the host relocates the transport
    t = follow (1.0, 0.2);
    CHECK (t.badFraction() < 0.01, "after the host jumps, audio follows the new position");

    p.hostPlaying = false;
    const int64_t held = engine.getClockSamples();
    float peak = 0.0f;
    r.run (0.3, p, [&] (int64_t, const float* o, int n) { for (int i = 0; i < n; ++i) peak = std::max (peak, std::fabs (o[i])); });
    CHECK (peak < 1.0e-9f && engine.getClockSamples() == held, "transport stopped: silent, clock held");

    p.hostPlaying = true;
    t = follow (0.8, 0.0);
    CHECK (t.badFraction() < 0.05, "transport started again at the same place: carries on");
}

static void testStressM2 (const juce::File& ramps, const juce::File& dc)
{
    std::cout << "\n=== Stress: every parameter changing, plus folder swaps ===\n";
    const double sr = 48000.0;
    auto libA = scanFolder (ramps);
    auto libB = scanFolder (dc);
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (libA);
    Runner r { engine, sr, 64 };
    juce::Random rnd (99);
    RadioEngine::Params p;
    bool bad = false;
    int blockCount = 0;
    int64_t host = 0;

    r.run (8.0, p, [&] (int64_t, const float* o, int n)
    {
        for (int i = 0; i < n; ++i)
            if (! std::isfinite (o[i]) || std::fabs (o[i]) > 3.0f)
            {
                if (! bad) std::cout << "    first bad sample: " << o[i] << " at block " << blockCount << " index " << i << "\n";
                bad = true;
            }
        ++blockCount;
        if (blockCount % 3 == 0)  p.station = rnd.nextFloat();
        if (blockCount % 5 == 0)  p.startNorm = rnd.nextFloat();
        if (blockCount % 7 == 0)  p.endNorm = rnd.nextFloat();
        if (blockCount % 11 == 0) p.loopMode = rnd.nextInt (3);
        if (blockCount % 13 == 0) p.paused = rnd.nextBool();
        if (blockCount % 17 == 0) engine.requestReset();
        if (blockCount % 19 == 0) p.startImmediate = rnd.nextBool();
        if (blockCount % 23 == 0) p.radioStart = rnd.nextBool();
        if (blockCount % 29 == 0) p.hostSync = rnd.nextBool();
        if (blockCount % 31 == 0) p.speedMode = rnd.nextInt (3);
        if (blockCount % 9 == 0)  p.speedTape = -400.0f + rnd.nextFloat() * 800.0f;
        if (blockCount % 10 == 0) p.speedNotes = -48.0f + rnd.nextFloat() * 72.0f;
        if (blockCount % 43 == 0) p.quantisePitch = rnd.nextBool();
        if (blockCount % 47 == 0) p.grit = rnd.nextBool();
        p.hostPlaying = (blockCount % 37) != 0;
        p.crossfadeMs = (blockCount % 50 == 0) ? 500.0f : 1.0f + rnd.nextFloat() * 40.0f;
        if (blockCount % 41 == 0) host = (int64_t) (rnd.nextFloat() * 20.0f * sr); else host += 64;
        p.hostSamples = host;
        if (blockCount % 200 == 0)
            engine.setLibrary ((blockCount / 200) % 2 ? libB : libA);
    });
    CHECK (! bad, "output stays finite and bounded while every parameter is automated");
    std::cout << "    underruns during the stress run: " << engine.getUnderrunCount() << " (informational)\n";
}

//==============================================================================
// Milestone 3 tests

static void writeSine (const juce::File& file, double rate, double seconds, double freq, float amp)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
    auto w = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (rate)
                                                                         .withNumChannels (1)
                                                                         .withBitsPerSample (24));
    const int64_t len = (int64_t) (seconds * rate);
    juce::AudioBuffer<float> buf (1, 4096);
    for (int64_t pos = 0; pos < len; pos += 4096)
    {
        const int n = (int) std::min<int64_t> (4096, len - pos);
        for (int i = 0; i < n; ++i)
            buf.setSample (0, i, amp * (float) std::sin (2.0 * 3.14159265358979 * freq * (double) (pos + i) / rate));
        w->writeFromAudioSampleBuffer (buf, 0, n);
    }
}

// Median of the ramp's slope, in file frames per output sample, over many blocks (blocks that contain a wrap are ignored).
static double medianSlope (Runner& r, RadioEngine::Params& p, double N, double seconds, double skipSeconds)
{
    std::vector<double> slopes;
    int64_t first = -1;
    r.run (seconds, p, [&] (int64_t clock, const float* o, int n)
    {
        if (first < 0) first = clock;
        if ((double) (clock - first) < skipSeconds * r.sr) return;
        const double slope = ((double) o[n - 1] - (double) o[0]) / (double) (n - 1) / 0.9 * N;
        if (std::fabs (slope) < 10.0) slopes.push_back (slope);
    });
    if (slopes.empty()) return 1.0e9;
    std::sort (slopes.begin(), slopes.end());
    return slopes[slopes.size() / 2];
}

static void testSpeeds (const juce::File& dir)
{
    std::cout << "\n=== Speed: tape and notes ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    const double N = (double) lib->stations[0].info.numFrames;

    struct Case { const char* name; int mode; float tape; float notes; bool quant; double expected; };
    const Case cases[] = {
        { "tape +200%",                  RadioEngine::SpeedTape,  200.0f,  0.0f, true,  2.0 },
        { "tape +50%",                   RadioEngine::SpeedTape,   50.0f,  0.0f, true,  0.5 },
        { "tape +400%",                  RadioEngine::SpeedTape,  400.0f,  0.0f, true,  4.0 },
        { "tape -100% (backwards)",      RadioEngine::SpeedTape, -100.0f,  0.0f, true, -1.0 },
        { "tape -250% (backwards)",      RadioEngine::SpeedTape, -250.0f,  0.0f, true, -2.5 },
        { "notes +12 semitones",         RadioEngine::SpeedNotes, 100.0f, 12.0f, true,  2.0 },
        { "notes -12 semitones",         RadioEngine::SpeedNotes, 100.0f, -12.0f, true, 0.5 },
        { "notes +7 semitones",          RadioEngine::SpeedNotes, 100.0f,  7.0f, true,  std::exp2 (7.0 / 12.0) },
        { "notes +5.4, quantised: 5",    RadioEngine::SpeedNotes, 100.0f,  5.4f, true,  std::exp2 (5.0 / 12.0) },
        { "notes +5.4, not quantised",   RadioEngine::SpeedNotes, 100.0f,  5.4f, false, std::exp2 (5.4 / 12.0) },
    };

    for (auto& c : cases)
    {
        RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
        Runner r { engine, sr };
        RadioEngine::Params p; p.station = 0.5f; p.speedMode = c.mode; p.speedTape = c.tape; p.speedNotes = c.notes; p.quantisePitch = c.quant;
        const double slope = medianSlope (r, p, N, 1.2, 0.4);
        std::cout << "    " << c.name << ": measured " << slope << " frames per sample, expected " << c.expected << "\n";
        CHECK (std::fabs (slope - c.expected) < 0.01 * std::max (1.0, std::fabs (c.expected)), std::string (c.name) + " plays at the right speed");
        CHECK (engine.getUnderrunCount() == 0, std::string (c.name) + ": no underruns");
    }
}

static void testReversal (const juce::File& dir)
{
    std::cout << "\n=== Speed passing through zero into reverse ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    const double N = (double) lib->stations[0].info.numFrames;
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p; p.station = 0.5f;

    r.run (1.0, p);
    float v1 = 0.0f;
    r.run (0.1, p, [&] (int64_t, const float* o, int n) { v1 = o[n - 1]; });
    const double pos1 = (double) v1 / 0.9 * N;

    p.speedTape = -100.0f;
    r.run (0.6, p);
    float v2 = 0.0f; bool bad = false;
    r.run (0.2, p, [&] (int64_t, const float* o, int n) { v2 = o[n - 1]; for (int i = 0; i < n; ++i) if (! std::isfinite (o[i]) || std::fabs (o[i]) > 1.5f) bad = true; });
    const double pos2 = (double) v2 / 0.9 * N;
    const double movedSeconds = (pos2 - pos1) / sr;
    std::cout << "    moved " << movedSeconds << " s in 0.8 s after reversing (expected about -0.8 + a small forward glide)\n";
    CHECK (movedSeconds < -0.55 && movedSeconds > -0.95, "after reversing, the station runs backwards from where it was (no jump)");
    CHECK (! bad, "output stays finite and bounded through the reversal");
    CHECK (std::fabs (medianSlope (r, p, N, 0.5, 0.0) + 1.0) < 0.01, "reverse plays at -1x");

    p.speedTape = 100.0f;
    r.run (0.5, p);
    CHECK (std::fabs (medianSlope (r, p, N, 0.5, 0.0) - 1.0) < 0.01, "and forwards again at +1x");
    CHECK (engine.getUnderrunCount() == 0, "no underruns");
}

static void testReverseInWindow (const juce::File& dir)
{
    std::cout << "\n=== Reverse speed inside a loop window ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    const auto info = lib->stations[0].info;
    const double N = (double) info.numFrames;

    for (int mode : { LoopForward, LoopPingPong })
    {
        RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
        Runner r { engine, sr };
        RadioEngine::Params p; p.station = 0.5f; p.startNorm = 0.25f; p.endNorm = 0.30f; p.loopMode = mode; p.speedTape = -100.0f;
        const auto cfg = cfgFromParams (p, info);
        double lo = 1.0e9, hi = -1.0e9; int blocks = 0;
        r.run (2.5, p, [&] (int64_t, const float* o, int n)
        {
            if (++blocks < 40) return;
            for (int i = 0; i < n; ++i) { const double pos = (double) o[i] / 0.9 * N; lo = std::min (lo, pos); hi = std::max (hi, pos); }
        });
        std::cout << "    window " << cfg.lo << ".." << cfg.hi << ", played " << lo << ".." << hi << "\n";
        CHECK (lo > (double) cfg.lo - 64.0 && hi < (double) cfg.hi + 64.0, (mode == LoopForward ? "backwards loop stays inside the window" : "backwards ping-pong stays inside the window"));
        CHECK (hi - lo > 0.9 * (double) cfg.length(), "and covers the whole window (it wraps / bounces)");
    }
}

static void testBackgroundStaysAtOneX (const juce::File& dir)
{
    std::cout << "\n=== Background stations stay at 1x while the tuned one is sped up ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    const double N = (double) lib->stations[0].info.numFrames;
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p; p.station = 0.25f; p.speedTape = 200.0f; p.crossfadeMs = 5.0f;

    float vLeave = 0.0f; int64_t cLeave = 0;
    r.run (1.5, p, [&] (int64_t clock, const float* o, int n) { vLeave = o[n - 1]; cLeave = clock + n - 1; });
    const double posLeave = (double) vLeave / 0.9 * N;

    p.station = 0.75f;
    r.run (1.0, p);

    p.station = 0.25f; p.speedTape = 100.0f;
    float vBack = 0.0f; int64_t cBack = 0;
    r.run (0.6, p, [&] (int64_t clock, const float* o, int n) { vBack = o[n - 1]; cBack = clock + n - 1; });
    const double posBack = (double) vBack / 0.9 * N;
    const double expected = posLeave + (double) (cBack - cLeave);
    std::cout << "    left at " << posLeave / sr << " s, back at " << posBack / sr << " s, expected about " << expected / sr << " s\n";
    CHECK (std::fabs (posBack - expected) < 0.05 * sr, "station 0 kept advancing at 1x while it was not tuned");
}

static void testCubicAndGrit (const juce::File& dir)
{
    std::cout << "\n=== Cubic interpolation and grit mode ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    const auto info = lib->stations[0].info;                 // 1 kHz sine, 44.1 kHz file, 0.5 amplitude

    auto rmsError = [&] (bool grit, double& rmsSignal)
    {
        RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
        Runner r { engine, sr };
        RadioEngine::Params p; p.station = 0.5f; p.grit = grit;
        double se = 0.0, ss = 0.0; int count = 0; int64_t first = -1;
        r.run (1.0, p, [&] (int64_t clock, const float* o, int n)
        {
            if (first < 0) first = clock;
            if ((double) (clock - first) < 0.4 * sr) return;
            for (int i = 0; i < n; ++i)
            {
                const double pos = (double) (clock + i) * (info.sampleRate / sr);
                const double e = 0.5 * std::sin (2.0 * 3.14159265358979 * 1000.0 * pos / info.sampleRate);
                se += ((double) o[i] - e) * ((double) o[i] - e);
                ss += (double) o[i] * (double) o[i];
                ++count;
            }
        });
        rmsSignal = std::sqrt (ss / count);
        return std::sqrt (se / count);
    };

    double sigCubic = 0.0, sigGrit = 0.0;
    const double errCubic = rmsError (false, sigCubic);
    const double errGrit  = rmsError (true, sigGrit);
    std::cout << "    cubic: error RMS " << errCubic << ", signal RMS " << sigCubic << "; grit: error RMS " << errGrit << ", signal RMS " << sigGrit << "\n";
    CHECK (errCubic < 0.0005, "cubic interpolation follows a 1 kHz sine (44.1 kHz file in a 48 kHz session) within -60 dB");
    CHECK (errGrit > 4.0 * errCubic, "grit mode is audibly different from cubic (more error: hold, 8-bit, filter)");
    CHECK (sigGrit > 0.25 && sigGrit < 0.40, "grit mode keeps the level (signal RMS about 0.35)");
}

static void testPauseSilence (const juce::File& dir)
{
    std::cout << "\n=== Pause fades to silence (no steady DC) ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);                             // DC 0.5 and DC 0.25
    for (int mode : { RadioEngine::SpeedTape, RadioEngine::SpeedNotes })
    {
        RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
        Runner r { engine, sr };
        RadioEngine::Params p; p.station = 0.25f; p.speedMode = mode;
        r.run (0.5, p);
        float level = 0.0f;
        r.run (0.1, p, [&] (int64_t, const float* o, int n) { level = o[n - 1]; });
        CHECK (std::fabs (level - 0.5f) < 0.01f, "playing: DC file reads 0.5");

        p.paused = true;
        r.run (mode == RadioEngine::SpeedTape ? 0.6 : 0.1, p);
        float peak = 0.0f;
        r.run (0.3, p, [&] (int64_t, const float* o, int n) { for (int i = 0; i < n; ++i) peak = std::max (peak, std::fabs (o[i])); });
        std::cout << "    paused level " << peak << "\n";
        CHECK (peak < 1.0e-3f, (mode == RadioEngine::SpeedTape ? "tape pause: silent once stopped (no DC)" : "notes pause: silent almost at once (no DC)"));

        p.paused = false;
        r.run (0.7, p, [&] (int64_t, const float* o, int n) { level = o[n - 1]; });
        CHECK (std::fabs (level - 0.5f) < 0.01f, "released: sound returns");
    }
}

static void testGritDC (const juce::File& dir)
{
    std::cout << "\n=== Grit mode keeps a steady level ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dir);
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p; p.station = 0.25f; p.grit = true;
    r.run (0.5, p);
    float lo = 10.0f, hi = -10.0f;
    r.run (0.3, p, [&] (int64_t, const float* o, int n) { for (int i = 0; i < n; ++i) { lo = std::min (lo, o[i]); hi = std::max (hi, o[i]); } });
    std::cout << "    DC 0.5 through grit: " << lo << " .. " << hi << "\n";
    CHECK (lo > 0.49f && hi < 0.51f, "DC 0.5 comes out within 8-bit accuracy");
}


//==============================================================================
// Milestone 4: banks, subfolders, .raw, skipN, settings.txt

static void writeRawDC (const juce::File& file, double seconds, float value)
{
    juce::MemoryBlock mb;
    const int64_t n = (int64_t) (seconds * 44100.0);
    mb.setSize ((size_t) n * 2);
    const int16_t v = (int16_t) std::lround (value * 32768.0f);
    for (int64_t i = 0; i < n; ++i)
    {
        mb[(size_t) i * 2]     = (char) (v & 0xff);          // little-endian
        mb[(size_t) i * 2 + 1] = (char) ((v >> 8) & 0xff);
    }
    file.replaceWithData (mb.getData(), mb.getSize());
}

static void testRules()
{
    std::cout << "\n=== M4 naming and settings rules ===\n";
    CHECK (parseBankNumber ("0") == 0 && parseBankNumber ("05 shortwave") == 5 && parseBankNumber ("15 - Koto plucks") == 15,
           "bank numbers: 0, 05 shortwave, 15 - Koto plucks");
    CHECK (parseBankNumber ("16 too high") == -1 && parseBankNumber ("shortwave") == -1 && parseBankNumber ("2024 field") == -1,
           "not banks: 16, no number, 2024");

    CHECK (parseSkipCount ("loop skip2") == 2 && parseSkipCount ("skip1") == 1 && parseSkipCount ("Fooskip12") == 12 && parseSkipCount ("x SKIP255") == 255,
           "skipN found (1, 2, 12, 255; letter case ignored)");
    CHECK (parseSkipCount ("skip0") == 0 && parseSkipCount ("skip256") == 0 && parseSkipCount ("skip") == 0 && parseSkipCount ("kip3") == 0 && parseSkipCount ("a skip300") == 0,
           "skipN rejected (0, 256, no number, wrong word, 300)");

    CHECK (endsWithNext ("seq next") && endsWithNext ("Next") && ! endsWithNext ("next up") && ! endsWithNext ("random"),
           "subfolder 'next' rule");

    const auto s = parseSettings ("  Cross Fade Time = 100   # ten\n"
                                  "LOOPMODE=2\n"
                                  "# whole line comment\n"
                                  "pitchKnobMin = -12\n"
                                  "bad line\n"
                                  "novalue =\n"
                                  "frac = 1.5\n"
                                  "tunerMode\t=\t1\r\n");
    CHECK (settingOr (s, "crossfadetime", -1) == 100, "settings: whitespace inside names ignored, comment stripped");
    CHECK (settingOr (s, "loopMode", -1) == 2, "settings: names are not case-sensitive");
    CHECK (settingOr (s, "pitchKnobMin", 0) == -12, "settings: negative values");
    CHECK (settingOr (s, "tunerMode", -1) == 1, "settings: tabs and CRLF");
    CHECK (! hasSetting (s, "novalue") && ! hasSetting (s, "frac") && s.size() == 4, "settings: malformed lines are skipped");

    const auto m = mergeSettings (parseSettings ("a = 1\nb = 2"), parseSettings ("b = 5\nc = 3"));
    CHECK (settingOr (m, "a", -1) == 1 && settingOr (m, "b", -1) == 5 && settingOr (m, "c", -1) == 3, "settings: bank file overrides, the rest is inherited");

    CHECK (quantiseStep (0.3f, 8) == 304 && quantiseStep (0.5f, 1) == 512 && quantiseStep (1.0f, 2) == 1024, "START quantised by the divider");
}

// Plays the tuned station for a while and returns the mean of the last block.
static float levelAfter (Runner& r, RadioEngine::Params& p, double seconds)
{
    float last = 0.0f;
    r.run (seconds, p, [&] (int64_t, const float* o, int n)
    {
        double sum = 0.0;
        for (int i = 0; i < n; ++i) sum += o[i];
        last = (float) (sum / n);
    });
    return last;
}

static void testBanks (const juce::File& root)
{
    std::cout << "\n=== M4 banks, subfolders, .raw, skipN, settings.txt ===\n";
    const double sr = 48000.0;

    root.getChildFile ("settings.txt").replaceWithText ("crossfadeTime = 100\nloopMode = 2  # ping-pong\nstartCVDivider = 8\n");

    auto b0 = root.getChildFile ("0 first"); b0.createDirectory();
    writeDC (b0.getChildFile ("a.wav"), 48000.0, 3.0, 0.5f);

    auto b5 = root.getChildFile ("05 shortwave"); b5.createDirectory();
    b5.getChildFile ("settings.txt").replaceWithText ("loopMode = 0\n");
    writeDC (b5.getChildFile ("01 plain.wav"), 48000.0, 3.0, 0.1f);

    auto seq = b5.getChildFile ("02 seq next"); seq.createDirectory();
    writeDC (seq.getChildFile ("a.wav"), 48000.0, 3.0, 0.2f);
    writeDC (seq.getChildFile ("b.wav"), 44100.0, 3.0, 0.3f);
    writeDC (seq.getChildFile ("c.wav"), 48000.0, 3.0, 0.4f);

    auto rnd = b5.getChildFile ("03 rand"); rnd.createDirectory();
    writeDC (rnd.getChildFile ("a.wav"), 48000.0, 3.0, 0.15f);
    writeDC (rnd.getChildFile ("b.wav"), 48000.0, 3.0, 0.25f);
    writeDC (rnd.getChildFile ("c.wav"), 48000.0, 3.0, 0.35f);

    auto skp = b5.getChildFile ("04 skipper next"); skp.createDirectory();
    writeDC (skp.getChildFile ("a skip2.wav"), 48000.0, 3.0, 0.45f);
    writeDC (skp.getChildFile ("b.wav"), 48000.0, 3.0, 0.55f);

    writeRawDC (b5.getChildFile ("05 raw.raw"), 3.0, 0.25f);
    writeRamp (b5.getChildFile ("._junk.wav"), 44100.0, 1.0, 16, 1);

    auto b16 = root.getChildFile ("16 too high"); b16.createDirectory();
    writeDC (b16.getChildFile ("e.wav"), 48000.0, 3.0, 0.9f);
    writeDC (root.getChildFile ("stray.wav"), 48000.0, 3.0, 0.9f);

    auto lib = scanFolder (root);
    CHECK (lib->hasNumberedBanks && lib->banks.size() == 2, "two banks found (0 and 05); 16 and the root file are ignored");
    if (lib->banks.size() != 2) return;
    CHECK (lib->banks[0].number == 0 && lib->banks[1].number == 5, "banks are in number order");
    CHECK (lib->banks[1].numSlots == 5, "bank 5 has 5 slots (file, next folder, random folder, skip folder, raw file)");
    if (lib->banks[1].numSlots != 5) return;

    const auto& slots = lib->slots;
    const int s5 = lib->banks[1].firstSlot;
    CHECK (slots[(size_t) s5].mode == SlotFile && slots[(size_t) s5 + 1].mode == SlotNext && slots[(size_t) s5 + 2].mode == SlotRandom
           && slots[(size_t) s5 + 3].mode == SlotNext && slots[(size_t) s5 + 4].mode == SlotFile,
           "slots in name order with the right modes");
    CHECK (slots[(size_t) s5 + 1].numFiles == 3 && slots[(size_t) s5 + 2].numFiles == 3, "subfolders hold their 3 files");

    const auto& raw = lib->stations[(size_t) slots[(size_t) s5 + 4].firstFile];
    CHECK (raw.file.getFileName() == "05 raw.raw" && std::fabs (raw.info.sampleRate - 44100.0) < 1e-6 && raw.info.numFrames == (int64_t) (3.0 * 44100.0),
           ".raw file read as 44.1 kHz mono, 3 s");
    CHECK (lib->stations[(size_t) slots[(size_t) s5 + 3].firstFile].skip == 2, "skip2 recognised in the file name");

    CHECK (settingOr (lib->banks[1].settings, "loopMode", -1) == 0 && settingOr (lib->banks[1].settings, "crossfadeTime", -1) == 100
           && settingOr (lib->banks[0].settings, "loopMode", -1) == 2,
           "settings.txt: bank 5 overrides loopMode and inherits crossfadeTime; bank 0 uses the top-level file");

    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p;
    p.crossfadeMs = 25.0f;

    // bank 0
    p.bank = 0; p.station = 0.5f;
    CHECK (std::fabs (levelAfter (r, p, 0.6) - 0.5f) < 0.01f, "bank 0 plays its file (0.5)");
    CHECK (engine.getNumStations() == 1, "bank 0 has 1 slot");

    // bank 5
    p.bank = 5; p.station = stationParam (0, 5);
    CHECK (std::fabs (levelAfter (r, p, 0.6) - 0.1f) < 0.01f, "bank 5 slot 0 plays its plain file (0.1)");
    CHECK (engine.getNumStations() == 5, "bank 5 has 5 slots");

    // raw
    p.station = stationParam (4, 5);
    CHECK (std::fabs (levelAfter (r, p, 0.6) - 0.25f) < 0.01f, ".raw file plays at the right level (0.25)");

    // a bank that does not exist
    p.bank = 7;
    levelAfter (r, p, 0.3);
    CHECK (std::fabs (levelAfter (r, p, 0.4)) < 0.001f && engine.getNumStations() == 0, "a missing bank is silent");

    // next folder: first file, then each Reset moves on, wrapping round
    p.bank = 5; p.station = stationParam (1, 5);
    CHECK (std::fabs (levelAfter (r, p, 0.7) - 0.2f) < 0.01f, "next folder starts on its first file (0.2)");
    const float expectNext[] = { 0.3f, 0.4f, 0.2f };
    for (int i = 0; i < 3; ++i)
    {
        engine.requestReset();
        const float got = levelAfter (r, p, 0.7);
        CHECK (std::fabs (got - expectNext[i]) < 0.01f, "Reset steps a next folder to its next file");
    }

    // random folder: every Reset gives a different file than the one before, all from the folder
    p.station = stationParam (2, 5);
    float prev = levelAfter (r, p, 0.7);
    bool allDiffer = true, allInSet = true;
    auto inSet = [] (float v) { return std::fabs (v - 0.15f) < 0.01f || std::fabs (v - 0.25f) < 0.01f || std::fabs (v - 0.35f) < 0.01f; };
    allInSet = inSet (prev);
    for (int i = 0; i < 5; ++i)
    {
        engine.requestReset();
        const float got = levelAfter (r, p, 0.7);
        allDiffer = allDiffer && std::fabs (got - prev) > 0.05f;
        allInSet = allInSet && inSet (got);
        prev = got;
    }
    CHECK (allInSet, "random folder only plays its own files");
    CHECK (allDiffer, "random folder never repeats the same file on consecutive Resets");

    // skipN: "a skip2" ignores the next 2 resets, the 3rd moves on to the next file
    p.station = stationParam (3, 5);
    CHECK (std::fabs (levelAfter (r, p, 0.7) - 0.45f) < 0.01f, "skip folder starts on the skip2 file (0.45)");
    engine.requestReset(); CHECK (std::fabs (levelAfter (r, p, 0.7) - 0.45f) < 0.01f, "first Reset is ignored");
    engine.requestReset(); CHECK (std::fabs (levelAfter (r, p, 0.7) - 0.45f) < 0.01f, "second Reset is ignored");
    engine.requestReset(); CHECK (std::fabs (levelAfter (r, p, 0.7) - 0.55f) < 0.01f, "third Reset moves on (0.55)");

    // tuning away and back reloads the skip count for the file that is selected (it has no skip: Reset acts at once)
    // Go back to the skip file by cycling the folder: reset wraps to a skip2.wav
    engine.requestReset(); CHECK (std::fabs (levelAfter (r, p, 0.7) - 0.45f) < 0.01f, "next Reset wraps back to the skip2 file");
    engine.requestReset(); CHECK (std::fabs (levelAfter (r, p, 0.7) - 0.45f) < 0.01f, "...and its resets are ignored again (1)");
    p.station = stationParam (0, 5); levelAfter (r, p, 0.5);        // tune away
    p.station = stationParam (3, 5); levelAfter (r, p, 0.5);        // and back: the count starts again at 2
    engine.requestReset(); CHECK (std::fabs (levelAfter (r, p, 0.7) - 0.45f) < 0.01f, "retuning reloads the skip count (1 of 2 ignored)");
    engine.requestReset(); CHECK (std::fabs (levelAfter (r, p, 0.7) - 0.45f) < 0.01f, "retuning reloads the skip count (2 of 2 ignored)");
    engine.requestReset(); CHECK (std::fabs (levelAfter (r, p, 0.7) - 0.55f) < 0.01f, "then the next Reset moves on");

    // reselect on tune: a next folder steps when it is tuned to
    p.reselectOnTune = true;
    p.station = stationParam (1, 5);
    const float a = levelAfter (r, p, 0.7);
    p.station = stationParam (0, 5); levelAfter (r, p, 0.5);
    p.station = stationParam (1, 5);
    const float b = levelAfter (r, p, 0.7);
    CHECK (std::fabs (a - b) > 0.05f, "with reselect on tune, tuning back to a subfolder picks its next file");
}


// Closer to a real folder: stereo 44.1 kHz files, loop mode off, a session at 48 kHz.
static void testNextFolderRealistic (const juce::File& root)
{
    std::cout << "\n=== M4 realistic next folder ===\n";
    auto bank = root.getChildFile ("0"); bank.createDirectory();
    writeRamp (bank.getChildFile ("02 - one.wav"), 44100.0, 20.0, 16, 2);
    writeRamp (bank.getChildFile ("03 - two.wav"), 44100.0, 20.0, 16, 2);
    writeRamp (bank.getChildFile ("a skip2.wav"),  44100.0, 20.0, 16, 2);
    auto nxt = bank.getChildFile ("loops next"); nxt.createDirectory();
    writeRamp (nxt.getChildFile ("04 - x.wav"), 44100.0, 20.0, 16, 2);
    writeRamp (nxt.getChildFile ("05 - y.wav"), 44100.0, 21.0, 16, 2);
    writeRamp (nxt.getChildFile ("06 - z.wav"), 44100.0, 22.0, 16, 2);

    auto lib = scanFolder (root);
    CHECK (lib->banks.size() == 1 && lib->banks[0].numSlots == 4, "bank 0 has 4 slots");
    if (lib->banks.empty()) return;

    for (int mode : { LoopOff, LoopForward })
    {
        RadioEngine engine; engine.prepare (48000.0); engine.setLibrary (lib);
        Runner r { engine, 48000.0, 1024 };
        RadioEngine::Params p;
        p.loopMode = mode; p.station = stationParam (3, 4); p.bank = 0;
        r.run (1.0, p);
        const int f0 = engine.getDisplayedFile();
        CHECK (engine.getDisplayedStation() == 3, "tuned to the next folder (slot 4)");
        int prev = f0;
        for (int i = 0; i < 4; ++i)
        {
            engine.requestReset();
            r.run (0.6, p);
            const int f = engine.getDisplayedFile();
            std::cout << "    loop mode " << mode << ", Reset " << (i + 1) << ": file " << prev << " -> " << f << "\n";
            CHECK (f != prev, "Reset moves a next folder to a different file");
            prev = f;
        }
    }
}


//==============================================================================
// Milestone 5: Soft and Radio tuner modes, fade modes

struct Stat { double mean = 0.0, lo = 1e9, hi = -1e9, rms = 0.0, maxStep = 0.0; };

static Stat measure (Runner& r, RadioEngine::Params& p, double settle, double seconds)
{
    r.run (settle, p);
    Stat st;
    std::vector<float> all;
    r.run (seconds, p, [&] (int64_t, const float* o, int n) { all.insert (all.end(), o, o + n); });
    double sum = 0.0;
    for (size_t i = 0; i < all.size(); ++i)
    {
        sum += all[i];
        st.lo = std::min (st.lo, (double) all[i]);
        st.hi = std::max (st.hi, (double) all[i]);
        if (i > 0) st.maxStep = std::max (st.maxStep, (double) std::fabs (all[i] - all[i - 1]));
    }
    st.mean = sum / (double) all.size();
    double sq = 0.0;
    for (auto v : all) sq += (v - st.mean) * (v - st.mean);
    st.rms = std::sqrt (sq / (double) all.size());
    return st;
}

static void testSoft (const juce::File& dc, const juce::File& dc3)
{
    std::cout << "\n=== M5 Soft tuner mode ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dc);                          // station 0 = DC 0.5, station 1 = DC 0.25
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p;
    p.tunerMode = RadioEngine::TunerSoft;
    p.crossfadeMs = 5000.0f;                             // Soft must not use the crossfade time

    struct Case { float x; double expect; const char* name; };
    const double cp = 0.5 * std::sqrt (0.5) + 0.25 * std::sqrt (0.5);
    const Case cases[] = { { 0.0f, 0.5, "far left: first station alone" }, { 0.25f, 0.5, "middle of station 1: it plays alone" },
                           { 0.5f, cp, "halfway between: constant-power blend" }, { 0.75f, 0.25, "middle of station 2: it plays alone" },
                           { 1.0f, 0.25, "far right: last station alone (no wrap)" } };
    for (const auto& c : cases)
    {
        p.station = c.x;
        const auto st = measure (r, p, 0.5, 0.1);
        std::cout << "    x=" << c.x << " level " << st.mean << " (expected " << c.expect << ")\n";
        CHECK (std::fabs (st.mean - c.expect) < 0.01, c.name);
    }

    p.fadeMode = RadioEngine::FadeLinear;
    p.station = 0.5f;
    {
        const auto st = measure (r, p, 0.3, 0.1);
        CHECK (std::fabs (st.mean - 0.375) < 0.01, "linear fade mode: halfway between = plain average (0.375)");
    }
    p.fadeMode = RadioEngine::FadeConstantPower;

    // A slow sweep across three stations (so the two channels swap roles twice) must have no dropouts or clicks.
    auto lib3 = scanFolder (dc3);                        // 0.6, 0.3, 0.15
    CHECK (lib3->stations.size() == 3, "three DC stations found");
    RadioEngine e3; e3.prepare (sr); e3.setLibrary (lib3);
    Runner r3 { e3, sr, 128 };
    RadioEngine::Params q;
    q.tunerMode = RadioEngine::TunerSoft;
    q.station = 0.0f;
    r3.run (0.5, q);

    const double sweepSeconds = 3.0;
    double lo = 10.0, hi = -10.0, maxStep = 0.0, prev = -100.0;
    double xs = 0.0;
    r3.run (sweepSeconds, q, [&] (int64_t, const float* o, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            lo = std::min (lo, (double) o[i]); hi = std::max (hi, (double) o[i]);
            if (prev > -50.0) maxStep = std::max (maxStep, std::fabs ((double) o[i] - prev));
            prev = o[i];
        }
        xs += (double) n / (sr * sweepSeconds);
        q.station = (float) std::min (1.0, xs);
    });
    std::cout << "    sweep: min " << lo << " max " << hi << " largest step " << maxStep << "\n";
    CHECK (maxStep < 0.003, "slow sweep across 3 stations: no clicks or dropouts");
    CHECK (lo > 0.14 && hi < 0.7, "sweep stays between the station levels (never silent, never above the loudest blend)");

    // The Reset button restarts both audible stations.
    q.station = 1.0f / 3.0f;                             // halfway between station 1 and station 2 on the Soft scale
    const auto before = measure (r3, q, 0.4, 0.05);
    e3.requestReset();
    const auto after = measure (r3, q, 0.1, 0.1);
    std::cout << "    blend before reset " << before.mean << " after " << after.mean << "\n";
    CHECK (std::fabs (before.mean - after.mean) < 0.01, "Reset keeps the blend steady (DC files)");
    CHECK (e3.getResetsApplied() == 2, "Reset in the middle of a blend is applied to both stations");
}

static void testRadio (const juce::File& dc)
{
    std::cout << "\n=== M5 Radio tuner mode ===\n";
    const double sr = 48000.0;
    auto lib = scanFolder (dc);                          // station 0 = DC 0.5, station 1 = DC 0.25
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr };
    RadioEngine::Params p;
    p.tunerMode = RadioEngine::TunerRadio;
    p.minStrength = 256;                                 // every station at full strength
    p.noiseVol = 100.0f; p.whistleVol = 0.0f;

    // reception shape: rn = distance from the station centre as a fraction of half a segment; reception ends at rn = 0.6
    auto amp = [] (double rn) { const double s = std::max (0.0, 1.0 - rn / 0.6); return s * s * (3.0 - 2.0 * s); };

    p.station = 0.25f;                                   // exactly on station 1
    {
        const auto st = measure (r, p, 0.5, 0.2);
        std::cout << "    on station: mean " << st.mean << " rms " << st.rms << "\n";
        CHECK (std::fabs (st.mean - 0.5) < 0.01 && st.rms < 0.01, "exactly tuned at full strength: clean station, no hiss");
    }

    p.station = 0.5f;                                    // halfway between the two stations
    {
        const auto st = measure (r, p, 0.3, 0.3);
        std::cout << "    between stations: mean " << st.mean << " rms " << st.rms << "\n";
        CHECK (st.rms > 0.14 && st.rms < 0.21, "between stations: hiss only (uniform noise, peak 0.3 at noise 100)");
        CHECK (std::fabs (st.mean) < 0.02, "between stations: no station leaks through");
    }

    p.noiseVol = 50.0f;
    {
        const auto st = measure (r, p, 0.3, 0.3);
        CHECK (st.rms > 0.07 && st.rms < 0.105, "noise 50 is half as loud as noise 100");
    }

    p.noiseVol = 0.0f;
    {
        const auto st = measure (r, p, 0.3, 0.2);
        CHECK (std::max (std::fabs (st.lo), std::fabs (st.hi)) < 1.0e-3, "noise 0 and whistle 0: silence between stations");
    }

    p.noiseVol = 100.0f;
    p.station = 0.3f;                                    // a little off station 1 (rn = 0.2)
    {
        const double a = amp (0.2);
        const auto st = measure (r, p, 0.5, 0.3);
        std::cout << "    slightly off: mean " << st.mean << " (expected " << 0.5 * a << "), rms " << st.rms << "\n";
        CHECK (std::fabs (st.mean - 0.5 * a) < 0.012, "slightly off station: the station is quieter by the reception curve");
        CHECK (st.rms > 0.02, "slightly off station: hiss is mixed in");
    }

    // Whistle: a sine, silent when exactly tuned, louder off to the side.
    p.noiseVol = 0.0f; p.whistleVol = 100.0f;
    p.station = 0.25f;
    {
        const auto st = measure (r, p, 0.5, 0.3);
        CHECK ((st.hi - st.lo) < 0.01, "whistle fades away at exact tune (no beat, no DC jump)");
    }
    p.station = 0.3f;
    {
        const auto st = measure (r, p, 0.5, 0.3);
        std::cout << "    whistle swing " << (st.hi - st.lo) * 0.5 << "\n";
        CHECK ((st.hi - st.lo) * 0.5 > 0.05 && (st.hi - st.lo) * 0.5 < 0.11, "whistle audible beside the station (about 0.08)");
    }

    // SSB effect (ring-modulation style): untouched at exact tune, strongly modulated off tune.
    p.whistleVol = 0.0f; p.ssb = true;
    p.station = 0.25f;
    {
        const auto st = measure (r, p, 0.5, 0.2);
        CHECK (std::fabs (st.mean - 0.5) < 0.01 && (st.hi - st.lo) < 0.01, "SSB on, exactly tuned: station unchanged");
    }
    p.station = 0.3f;
    {
        const auto st = measure (r, p, 0.5, 0.4);
        std::cout << "    SSB off-tune swing " << st.hi - st.lo << "\n";
        CHECK ((st.hi - st.lo) > 0.2, "SSB on, off tune: the station is ring-modulated");
    }
    p.ssb = false;

    // Station strengths.
    {
        float lo = 2.0f, hi = -1.0f; double sum = 0.0;
        for (int g = 0; g < 400; ++g) { const float s = RadioEngine::stationStrength (g, 0); lo = std::min (lo, s); hi = std::max (hi, s); sum += s; }
        std::cout << "    strength at min 0: " << lo << " .. " << hi << " mean " << sum / 400.0 << "\n";
        CHECK (lo >= 0.0f && hi <= 1.0f && lo < 0.1f && hi > 0.9f && std::fabs (sum / 400.0 - 0.5) < 0.1, "minRadioStationStrength 0: strengths spread from inaudible to full");
        float lo2 = 2.0f, hi2 = -1.0f, lo3 = 2.0f;
        for (int g = 0; g < 400; ++g)
        {
            const float s = RadioEngine::stationStrength (g, 128); lo2 = std::min (lo2, s); hi2 = std::max (hi2, s);
            lo3 = std::min (lo3, RadioEngine::stationStrength (g, 256));
        }
        CHECK (lo2 >= 0.5f && hi2 <= 1.0f, "minRadioStationStrength 128: nothing weaker than half");
        CHECK (lo3 > 0.999f, "minRadioStationStrength 256: all stations at full strength");
    }

    p.noiseVol = 0.0f; p.minStrength = 0;
    for (int slot = 0; slot < 2; ++slot)
    {
        p.station = (float) (slot * 0.5 + 0.25);
        const double S = RadioEngine::stationStrength (slot, 0);
        const double value = slot == 0 ? 0.5 : 0.25;
        const auto st = measure (r, p, 0.5, 0.2);
        std::cout << "    station " << slot << " strength " << S << " level " << st.mean << " (expected " << S * value << ")\n";
        CHECK (std::fabs (st.mean - S * value) < 0.01, "a weak station is quieter by its strength");
    }

    // Walking across the boundary between two stations: no clicks.
    p.minStrength = 256; p.noiseVol = 0.0f; p.whistleVol = 0.0f;
    p.station = 0.25f;
    r.run (0.5, p);
    double maxStep = 0.0, prev = -100.0, xs = 0.25;
    r.run (2.0, p, [&] (int64_t, const float* o, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (prev > -50.0) maxStep = std::max (maxStep, std::fabs ((double) o[i] - prev));
            prev = o[i];
        }
        xs = std::min (0.75, xs + 0.5 * (double) n / (sr * 2.0));
        p.station = (float) xs;
    });
    std::cout << "    walk across: largest step " << maxStep << "\n";
    CHECK (maxStep < 0.003, "walking from one station to the next: no clicks");
}


//==============================================================================
// Milestone 6: 90s mode (granular time stretch, pitch and advance rate separate)

static double zeroCrossHz (const std::vector<float>& v, double sr)
{
    int crossings = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] <= 0.0f && v[i] > 0.0f)
            ++crossings;
    return (double) crossings * sr / (double) v.size();
}

static std::vector<float> record (Runner& r, RadioEngine::Params& p, double settle, double seconds)
{
    r.run (settle, p);
    std::vector<float> all;
    r.run (seconds, p, [&] (int64_t, const float* o, int n) { all.insert (all.end(), o, o + n); });
    return all;
}

static double rmsOf (const std::vector<float>& v)
{
    double sq = 0.0;
    for (auto x : v) sq += (double) x * x;
    return std::sqrt (sq / (double) v.size());
}

static void test90s (const juce::File& ramp, const juce::File& sine)
{
    std::cout << "\n=== M6 '90s mode ===\n";
    const double sr = 44100.0;

    // 1. pitch 100%, advance 100%: identical to the plain file
    {
        auto lib = scanFolder (ramp);
        RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
        Runner r { engine, sr, 256 };
        RadioEngine::Params p; p.station = 0.5f; p.speedMode = RadioEngine::Speed90s;
        r.run (0.5, p);
        double worst = 0.0; int blocks = 0;
        const auto info = lib->stations[0].info;
        r.run (1.0, p, [&] (int64_t clock, const float* o, int n)
        {
            for (int i = 0; i < n; i += 16)
                worst = std::max (worst, std::fabs ((double) o[i] - expectedRamp (info, clock + i, sr)));
            ++blocks;
        });
        std::cout << "    worst difference from the plain file " << worst << "\n";
        CHECK (worst < 2.0e-4, "pitch 100% / advance 100%: the file plays exactly as normal");
    }

    auto lib = scanFolder (sine);
    RadioEngine engine; engine.prepare (sr); engine.setLibrary (lib);
    Runner r { engine, sr, 256 };
    RadioEngine::Params p; p.station = 0.5f; p.speedMode = RadioEngine::Speed90s;

    // 2. pitch changes the tone, not the advance
    struct PitchCase { float pitch; double hz; };
    for (const auto& c : { PitchCase { 200.0f, 2000.0 }, PitchCase { 50.0f, 500.0 }, PitchCase { -100.0f, 1000.0 }, PitchCase { 100.0f, 1000.0 } })
    {
        p.speedTape = c.pitch;
        const auto v = record (r, p, 0.4, 0.5);
        const double hz = zeroCrossHz (v, sr);
        std::cout << "    pitch " << c.pitch << "% -> " << hz << " Hz (expected " << c.hz << ")\n";
        CHECK (std::fabs (hz - c.hz) < c.hz * 0.08, "pitch knob sets the pitch");
        CHECK (rmsOf (v) > 0.2, "...and the sound stays at full level");
    }

    // 3. frozen advance keeps sounding
    p.speedTape = 100.0f; p.advance90 = 0.0f;
    {
        const auto v = record (r, p, 0.6, 0.5);
        std::cout << "    advance 0%: rms " << rmsOf (v) << "\n";
        CHECK (rmsOf (v) > 0.1, "advance 0%: the sample keeps playing (frozen position, no silence)");
    }

    // 4. advance rate moves the file forward at that rate (ramp file, pitch unaffected)
    {
        auto rl = scanFolder (ramp);
        RadioEngine e2; e2.prepare (sr); e2.setLibrary (rl);
        Runner r2 { e2, sr, 256 };
        RadioEngine::Params q; q.station = 0.5f; q.speedMode = RadioEngine::Speed90s; q.advance90 = 50.0f;
        r2.run (0.5, q);
        auto a = record (r2, q, 0.0, 0.2);
        auto b = record (r2, q, 1.5, 0.2);
        double ma = 0.0, mb = 0.0;
        for (auto x : a) ma += x;
        for (auto x : b) mb += x;
        ma /= (double) a.size(); mb /= (double) b.size();
        const double slope = (mb - ma) / 1.5;
        const auto info = rl->stations[0].info;
        const double normalSlope = 0.9 * (info.sampleRate / sr) * sr / (double) info.numFrames;   // per second at 1x
        std::cout << "    advance 50%: slope " << slope << " per second (normal " << normalSlope << ")\n";
        CHECK (slope > normalSlope * 0.35 && slope < normalSlope * 0.65, "advance 50%: the file moves forward at about half speed");
    }

    // 5. Pause freezes the advance but the sound continues
    p.advance90 = 100.0f; p.paused = true;
    {
        const auto v = record (r, p, 0.8, 0.4);
        CHECK (rmsOf (v) > 0.1, "pause in '90s mode: advance freezes, sound continues");
    }
    p.paused = false;
    {
        const auto v = record (r, p, 0.8, 0.4);
        CHECK (rmsOf (v) > 0.2 && std::fabs (zeroCrossHz (v, sr) - 1000.0) < 80.0, "released: normal playback again");
    }

    // 6. Soft and Radio tuners do not offer 90s: Tape is used (advance is ignored)
    {
        p.tunerMode = RadioEngine::TunerSoft; p.advance90 = 0.0f; p.speedTape = 200.0f;
        const auto v = record (r, p, 0.6, 0.4);
        const double hz = zeroCrossHz (v, sr);
        std::cout << "    Soft tuner with '90s selected: " << hz << " Hz\n";
        CHECK (std::fabs (hz - 2000.0) < 160.0 && rmsOf (v) > 0.2, "'90s with the Soft tuner falls back to Tape speed");
        p.tunerMode = RadioEngine::TunerSharp;
    }

    // 7. switching between modes while playing
    {
        p.speedTape = 100.0f; p.advance90 = 100.0f;
        r.run (0.3, p);
        p.speedMode = RadioEngine::SpeedTape;
        auto v = record (r, p, 0.3, 0.3);
        CHECK (rmsOf (v) > 0.2, "'90s -> Tape while playing: sound continues");
        p.speedMode = RadioEngine::Speed90s;
        v = record (r, p, 0.3, 0.3);
        CHECK (rmsOf (v) > 0.2, "Tape -> '90s while playing: sound continues");
    }

    // 8. stress: wild pitch and advance moves never produce NaN or runaway levels
    {
        bool bad = false; double peak = 0.0;
        uint32_t seed = 12345u;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (double) (seed >> 8) / 16777216.0; };
        int blocks = 0;
        r.run (3.0, p, [&] (int64_t, const float* o, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                if (! std::isfinite (o[i])) bad = true;
                peak = std::max (peak, (double) std::fabs (o[i]));
            }
            if (++blocks % 8 == 0)
            {
                p.speedTape = (float) (rnd() * 800.0 - 400.0);
                p.advance90 = (float) (rnd() * 400.0);
                p.station = (float) rnd();
            }
        });
        std::cout << "    stress peak " << peak << "\n";
        CHECK (! bad && peak < 2.5, "random pitch / advance / station changes: finite, bounded output");
    }
}

int main (int argc, char** argv)
{
    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rm_engine_test");
    root.deleteRecursively();
    root.createDirectory();

    auto ramps = root.getChildFile ("ramps"); ramps.createDirectory();
    writeRamp (ramps.getChildFile ("01_a.wav"),     44100.0, 10.0, 24, 1);
    writeRamp (ramps.getChildFile ("02_b.wav"),     48000.0,  7.3, 24, 1);
    writeRamp (ramps.getChildFile ("03_c.wav"),     96000.0,  5.0, 24, 2);
    writeRamp (ramps.getChildFile ("04_short.wav"), 22050.0,  0.5, 16, 1);
    writeRamp (ramps.getChildFile ("._junk.wav"),   44100.0,  1.0, 16, 1);
    writeRamp (ramps.getChildFile ("_hidden.wav"),  44100.0,  1.0, 16, 1);
    ramps.getChildFile ("readme.txt").replaceWithText ("not audio");

    auto dc = root.getChildFile ("dc"); dc.createDirectory();
    writeDC (dc.getChildFile ("a.wav"), 48000.0, 3.0, 0.5f);
    writeDC (dc.getChildFile ("b.wav"), 48000.0, 3.0, 0.25f);

    auto big = root.getChildFile ("long"); big.createDirectory();
    writeRamp (big.getChildFile ("long.wav"), 44100.0, 30.0 * 60.0, 24, 1);

    auto win = root.getChildFile ("win"); win.createDirectory();
    writeRamp (win.getChildFile ("win.wav"), 48000.0, 10.0, 24, 1);

    auto dc3 = root.getChildFile ("dc3"); dc3.createDirectory();
    writeDC (dc3.getChildFile ("a.wav"), 48000.0, 3.0, 0.6f);
    writeDC (dc3.getChildFile ("b.wav"), 48000.0, 3.0, 0.3f);
    writeDC (dc3.getChildFile ("c.wav"), 48000.0, 3.0, 0.15f);

    auto two = root.getChildFile ("two"); two.createDirectory();
    writeRamp (two.getChildFile ("a.wav"), 48000.0, 10.0, 24, 1);
    writeRamp (two.getChildFile ("b.wav"), 48000.0, 10.0, 24, 1);

    root.getChildFile ("banks").createDirectory();
    root.getChildFile ("real").createDirectory();

    auto sine = root.getChildFile ("sine"); sine.createDirectory();
    writeSine (sine.getChildFile ("sine.wav"), 44100.0, 10.0, 1000.0, 0.5f);

    if (argc > 1 && std::string (argv[1]) == "m6")        // quick run of the milestone 6 tests only
    {
        test90s (win, sine);
        root.deleteRecursively();
        std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED: " + std::to_string (failures) + " check(s)") << "\n";
        return failures == 0 ? 0 : 1;
    }

    if (argc > 1 && std::string (argv[1]) == "m5")        // quick run of the milestone 5 tests only
    {
        testSoft (dc, dc3);
        testRadio (dc);
        root.deleteRecursively();
        std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED: " + std::to_string (failures) + " check(s)") << "\n";
        return failures == 0 ? 0 : 1;
    }

    if (argc > 1 && std::string (argv[1]) == "m4")        // quick run of the milestone 4 tests only
    {
        testRules();
        testBanks (root.getChildFile ("banks"));
        testNextFolderRealistic (root.getChildFile ("real"));
        root.deleteRecursively();
        std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED: " + std::to_string (failures) + " check(s)") << "\n";
        return failures == 0 ? 0 : 1;
    }

    testPathMath();
    testAtRate (48000.0, ramps);
    testAtRate (44100.0, ramps);
    testCrossfade (dc);
    testEdgeCases (root);
    testStress (ramps, dc);
    testWindows (win);
    testLoopOffSilence (win);
    testReset (win);
    testResetOnlyTunedStation (two);
    testRadioStartOff (two);
    testPause (win);
    testHostSync (win);
    testSpeeds (win);
    testReversal (win);
    testReverseInWindow (win);
    testBackgroundStaysAtOneX (two);
    testCubicAndGrit (sine);
    testPauseSilence (dc);
    testGritDC (dc);
    testStressM2 (ramps, dc);
    testRules();
    testBanks (root.getChildFile ("banks"));
    testSoft (dc, dc3);
    testRadio (dc);
    test90s (win, sine);
    testLongFile (big);

    root.deleteRecursively();
    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED: " + std::to_string (failures) + " check(s)") << "\n";
    return failures == 0 ? 0 : 1;
}
