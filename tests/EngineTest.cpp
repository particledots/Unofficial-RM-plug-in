// Offline-ish test of the Radio Music engine. Generates its own audio files, runs the engine
// paced in real time (the stream thread needs real time to read from disk) and checks:
//   * folder scanning rules,
//   * every station's playhead follows one global clock, including after tuning away and back,
//   * files at different sample rates play at the right speed in the session rate,
//   * the left channel of a stereo file is used,
//   * crossfades are smooth and about the requested length,
//   * a very long file is streamed, not loaded into RAM.
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
    void run (double seconds, RadioEngine::Params p, F&& check)
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
    void run (double seconds, RadioEngine::Params p)
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
            if (! std::isfinite (o[i]) || std::fabs (o[i]) > 2.0f) bad = true;
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

    testAtRate (48000.0, ramps);
    testAtRate (44100.0, ramps);
    testCrossfade (dc);
    testEdgeCases (root);
    testStress (ramps, dc);
    testLongFile (big);

    root.deleteRecursively();
    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED: " + std::to_string (failures) + " check(s)") << "\n";
    return failures == 0 ? 0 : 1;
}
