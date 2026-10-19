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
        { "90s mode (not yet separate): original speed", RadioEngine::Speed90s, 300.0f, 7.0f, true, 1.0 },
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

int main()
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

    auto two = root.getChildFile ("two"); two.createDirectory();
    writeRamp (two.getChildFile ("a.wav"), 48000.0, 10.0, 24, 1);
    writeRamp (two.getChildFile ("b.wav"), 48000.0, 10.0, 24, 1);

    auto sine = root.getChildFile ("sine"); sine.createDirectory();
    writeSine (sine.getChildFile ("sine.wav"), 44100.0, 10.0, 1000.0, 0.5f);

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
    testLongFile (big);

    root.deleteRecursively();
    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED: " + std::to_string (failures) + " check(s)") << "\n";
    return failures == 0 ? 0 : 1;
}
