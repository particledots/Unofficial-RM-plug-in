#include "RadioEngine.h"
#include <algorithm>
#include <cmath>

namespace rm
{
//==============================================================================
// Stream thread: opens files and keeps each voice's ring buffer topped up.
class RadioEngine::StreamThread : public juce::Thread
{
public:
    explicit StreamThread (RadioEngine& e) : juce::Thread ("RadioMusic stream"), engine (e) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            bool didWork = false;

            for (auto& v : engine.voices)
                didWork = engine.serviceVoice (v) || didWork;

            wait (didWork ? 1 : 2);
        }
    }

private:
    RadioEngine& engine;
};

//==============================================================================
RadioEngine::RadioEngine()
{
    Library::registerSupportedFormats (streamFormats);
    streamTmp.assign ((size_t) kChunkFrames, 0.0f);
    scratch.assign ((size_t) (kMaxStep * kMaxSpeed * kRenderChunk) + 16, 0.0f);

    for (auto& v : voices)
    {
        v.gen = 1;
        v.reqGen.store (1);
        v.handledGen = 1;
    }

    refClock.fill (0.0);
    epochOf.fill (0u);
    slotCur.fill (0);
    skipLeft.fill (0);

    streamThread = std::make_unique<StreamThread> (*this);
    streamThread->startThread();
}

RadioEngine::~RadioEngine()
{
    streamThread->stopThread (3000);
    streamThread.reset();
}

// Radio mode station strength: a fixed pseudo-random value per station slot between minStrength/256 and 1.
float RadioEngine::stationStrength (int globalSlot, int minStrength) noexcept
{
    uint32_t h = (uint32_t) globalSlot * 2654435761u + 0x9e3779b9u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    const float u = (float) (h & 0xffffu) / 65535.0f;
    const float m = juce::jlimit (0.0f, 1.0f, (float) minStrength / 256.0f);
    return m + (1.0f - m) * u;
}

void RadioEngine::prepare (double sessionSampleRate)
{
    sessionRate = sessionSampleRate > 0.0 ? sessionSampleRate : 44100.0;
    grainLen = std::max (64, 2 * (int) std::lround (kGrainSeconds * sessionRate * 0.5));
    grainWin.assign ((size_t) grainLen, 0.0f);
    for (int i = 0; i < grainLen; ++i)
        grainWin[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) grainLen);
    pitchSm = 1.0;
    clockSamples.store (0);
    underruns.store (0);
    knownTableId = tables[(size_t) activeTable.load()].id;
    settingsInit = false;
    pauseMul = 1.0;
    speedSm = 1.0;
    speedInit = false;
    curDir = 1;
    gritAcc = gritLP = 0.0;
    gritValue = 0.0f;
    xInit = false;
    rAmp = rNoise = rWhistle = rWhistleHz = rSsbHz = 0.0;
    whistlePhase = ssbPhase = 0.0;
    resetFlag.store (false);
    invalidateAll();
}

void RadioEngine::setLibrary (std::shared_ptr<const Library> library)
{
    const juce::ScopedLock sl (setLock);
    const uint32_t id = ++nextLibId;

    {
        const juce::ScopedLock l (libLock);
        streamLib   = library;
        streamLibId = id;
    }

    const int cur = activeTable.load();
    const int rd  = readingTable.load();
    int slot = 0;
    while (slot == cur || slot == rd)
        ++slot;                                   // 3 slots, at most 2 excluded

    auto& t = tables[(size_t) slot];
    t.id = id;
    t.bankFirst.fill (0);
    t.bankCount.fill (0);

    if (library != nullptr)
    {
        const int nFiles = (int) std::min<size_t> (library->stations.size(), (size_t) kMaxFiles);
        for (int i = 0; i < nFiles; ++i)
        {
            t.info[(size_t) i] = library->stations[(size_t) i].info;
            t.skip[(size_t) i] = (uint8_t) juce::jlimit (0, 255, library->stations[(size_t) i].skip);
        }

        const int nSlots = (int) std::min<size_t> (library->slots.size(), (size_t) kMaxFiles);
        for (int i = 0; i < nSlots; ++i)
        {
            const auto& ls = library->slots[(size_t) i];
            t.slots[(size_t) i] = { ls.firstFile, ls.numFiles, ls.mode };
        }

        for (const auto& b : library->banks)
        {
            if (b.number >= 0 && b.number < kNumBanks && b.firstSlot + b.numSlots <= nSlots)
            {
                t.bankFirst[(size_t) b.number] = b.firstSlot;
                t.bankCount[(size_t) b.number] = b.numSlots;
            }
        }
    }

    activeTable.store (slot);
}

//==============================================================================
// Audio thread

const RadioEngine::Table& RadioEngine::acquireTable() noexcept
{
    for (;;)
    {
        const int a = activeTable.load();
        readingTable.store (a);
        if (activeTable.load() == a)
            return tables[(size_t) a];
    }
}

// Frames of path time for a station at a given radio-clock sample.
double RadioEngine::tauFor (int station, int64_t clock, double fileRate) const noexcept
{
    return ((double) clock - refClock[(size_t) station]) * (fileRate / sessionRate);   // negative if played backwards past the start
}

void RadioEngine::setIdle (Voice& v) noexcept
{
    v.state       = Voice::State::Idle;
    v.sig         = Sig {};
    v.level       = 0.0f;
    v.levelTarget = 0.0f;
    v.gen        += 1;
    v.reqStation.store (-1, std::memory_order_relaxed);
    v.reqGen.store (v.gen, std::memory_order_release);
}

void RadioEngine::invalidateAll() noexcept
{
    for (auto& v : voices)
        setIdle (v);

    for (auto& ch : chans)
        ch = Channel {};
    badStation.fill (false);
    refClock.fill (0.0);
    epochOf.fill (0u);
    slotCur.fill (0);
    skipLeft.fill (0);
}

// A subfolder supplies a different file: the next one in order for a "...next" folder, otherwise a random one
// (never the same one twice in a row when there is a choice; not documented).
void RadioEngine::reselect (int g, const Table& t) noexcept
{
    const auto& sl = t.slots[(size_t) g];
    if (sl.numFiles < 2)
        return;

    int& cur = slotCur[(size_t) g];

    if (sl.mode == SlotNext)
    {
        cur = (cur + 1) % sl.numFiles;
    }
    else if (sl.mode == SlotRandom)
    {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        cur = (cur + 1 + (int) (rngState % (uint32_t) (sl.numFiles - 1))) % sl.numFiles;
    }
}

void RadioEngine::requestVoice (Voice& v, const Sig& sig, const Table& t, int64_t clock, float fadeInc) noexcept
{
    v.gen += 1;
    v.sig = sig;
    v.fadeInc = fadeInc;

    const auto& info = t.info[(size_t) sig.station];
    v.fileRate  = info.sampleRate;
    v.numFrames = info.numFrames;
    v.step      = info.sampleRate / sessionRate;
    v.phase     = 0.0;
    v.consumed  = 0;
    v.state     = Voice::State::Loading;
    v.grain     = grainNow;
    v.cfg       = makeCfg (sig.startStep, sig.endStep, info.numFrames, sig.loopMode);
    v.tauStart  = (int64_t) std::floor (tauFor (sig.station, clock, info.sampleRate));

    v.reqStation.store (sig.station, std::memory_order_relaxed);
    v.reqLo.store (v.cfg.lo, std::memory_order_relaxed);
    v.reqHi.store (v.cfg.hi, std::memory_order_relaxed);
    v.reqMode.store (v.cfg.mode, std::memory_order_relaxed);
    v.reqRev.store (v.cfg.rev ? 1 : 0, std::memory_order_relaxed);
    v.reqTau.store (v.tauStart, std::memory_order_relaxed);
    v.reqDir.store (sig.dir, std::memory_order_relaxed);
    v.reqTable.store (t.id, std::memory_order_relaxed);
    v.reqGen.store (v.gen, std::memory_order_release);
}

bool RadioEngine::isChannelVoice (int i) const noexcept
{
    for (const auto& ch : chans)
        if (i == ch.current || i == ch.pending)
            return true;
    return false;
}

int RadioEngine::allocVoice() noexcept
{
    for (int i = 0; i < kNumVoices; ++i)
        if (voices[(size_t) i].state == Voice::State::Idle && ! isChannelVoice (i))
            return i;

    int best = -1;
    float bestLevel = 2.0f;
    for (int i = 0; i < kNumVoices; ++i)
    {
        auto& v = voices[(size_t) i];
        if (isChannelVoice (i))
            continue;
        if (v.level < bestLevel)
        {
            bestLevel = v.level;
            best = i;
        }
    }

    if (best >= 0)
        setIdle (voices[(size_t) best]);

    return best;
}

void RadioEngine::handleTargetChange (int c, Sig desired, const Table& t, int64_t clock, float xfInc, float fastInc) noexcept
{
    auto& ch = chans[(size_t) c];

    if (desired.station >= 0 && badStation[(size_t) desired.station])
        desired = ch.committed;                          // never retry a file that failed to open

    if (desired == ch.committed)
    {
        if (ch.pending >= 0)                             // tuned back to what is already playing
        {
            setIdle (voices[(size_t) ch.pending]);
            ch.pending = -1;
            ch.pendingSig = Sig {};
        }
        return;
    }

    if (desired.station >= 0 && desired == ch.pendingSig)
        return;

    if (desired.station < 0)                             // nothing wanted on this channel: fade out whatever is playing
    {
        if (ch.pending >= 0)
        {
            setIdle (voices[(size_t) ch.pending]);
            ch.pending = -1;
            ch.pendingSig = Sig {};
        }
        if (ch.current >= 0)
        {
            voices[(size_t) ch.current].levelTarget = 0.0f;
            voices[(size_t) ch.current].levelInc    = xfInc;
        }
        ch.current = -1;
        ch.committed = Sig {};
        return;
    }

    // A new station uses the crossfade time; Reset, loop and window changes on the same station use a short fade.
    const float fade = (desired.station != ch.committed.station) ? xfInc : std::max (xfInc, fastInc);

    if (ch.pending >= 0)                                 // retarget the voice that is still loading
    {
        requestVoice (voices[(size_t) ch.pending], desired, t, clock, fade);
        ch.pendingSig = desired;
        return;
    }

    const int vi = allocVoice();
    if (vi < 0)
        return;

    auto& v = voices[(size_t) vi];
    v.chan = c;
    v.level = 0.0f;
    v.levelTarget = 1.0f;
    requestVoice (v, desired, t, clock, fade);
    ch.pending = vi;
    ch.pendingSig = desired;
}

bool RadioEngine::popFrames (Voice& v, float* dst, int num) noexcept
{
    if (num <= 0)
        return true;

    if (v.fifo.getNumReady() < num)
        return false;

    int s1, n1, s2, n2;
    v.fifo.prepareToRead (num, s1, n1, s2, n2);

    if (dst != nullptr)
    {
        if (n1 > 0) std::copy_n (v.data.data() + s1, n1, dst);
        if (n2 > 0) std::copy_n (v.data.data() + s2, n2, dst + n1);
    }

    v.fifo.finishedRead (n1 + n2);
    return true;
}

void RadioEngine::tryAlign (int voiceIndex, const Table& t, int64_t clock) noexcept
{
    auto& v = voices[(size_t) voiceIndex];
    auto& ch = chans[(size_t) v.chan];

    if (v.failedGen.load (std::memory_order_acquire) == v.gen)
    {
        if (v.sig.station >= 0 && v.sig.station < kMaxFiles)
            badStation[(size_t) v.sig.station] = true;

        if (voiceIndex == ch.pending)
        {
            ch.pending = -1;
            ch.pendingSig = Sig {};
        }
        if (voiceIndex == ch.current)
        {
            ch.current = -1;
            ch.committed = Sig {};
        }
        setIdle (v);
        return;
    }

    if (v.readyGen.load (std::memory_order_acquire) != v.gen)
        return;

    // The voice began where the station was when it was requested. Skip ahead by however much
    // the radio clock has moved since, so it joins in exactly where the station "is".
    // Join where the station really is, to a fraction of a frame (the voice began at a whole frame, tauStart).
    const double tauNow = tauFor (v.sig.station, clock, v.fileRate);
    const double skipReal = std::max (0.0, v.sig.dir > 0 ? tauNow - (double) v.tauStart : (double) v.tauStart - tauNow);
    const int64_t skip = (int64_t) std::floor (skipReal);

    if (skip + 4 > kFifoFrames - 2048)                   // too far behind to catch up from the buffer
    {
        requestVoice (v, v.sig, t, clock, v.fadeInc);
        return;
    }

    if ((int64_t) v.fifo.getNumReady() < skip + 4)
        return;                                          // wait for the stream thread to read further

    if (v.grain)                                         // 90s mode: the cursor sits on the station's position
    {
        popFrames (v, nullptr, (int) skip);
        v.consumed  = skip;
        v.histStart = skip;
        v.cursor    = (double) skip + (skipReal - (double) skip);
        v.grainClock = 0;
        v.grains[0] = Voice::Grain {};
        v.grains[1] = Voice::Grain { true, v.cursor, grainLen / 2 };   // starts at full weight, the first hop adds the next grain
        v.state = Voice::State::Playing;
    }
    else
    {
        popFrames (v, nullptr, (int) skip);
        float three[3] {};
        popFrames (v, three, 3);
        v.h[0] = three[0];                               // no earlier frame is known: repeat the first one
        v.h[1] = three[0];
        v.h[2] = three[1];
        v.h[3] = three[2];
        v.phase = skipReal - (double) skip;
        v.consumed = skip;
        v.state = Voice::State::Playing;
    }

    if (voiceIndex == ch.pending)
    {
        v.level = 0.0f;
        v.levelTarget = 1.0f;
        v.levelInc = v.fadeInc;

        if (ch.current >= 0)
        {
            auto& old = voices[(size_t) ch.current];
            old.levelTarget = 0.0f;
            old.levelInc = v.fadeInc;
            if (old.state == Voice::State::Loading)
                setIdle (old);
        }

        ch.current = ch.pending;
        ch.committed = ch.pendingSig;
        ch.pending = -1;
        ch.pendingSig = Sig {};
    }
}

void RadioEngine::renderVoice (Voice& v, float* out, int n, const Table& t, int64_t clock,
                               const float* eff, double sumEff, const float* gain, bool cubic,
                               float chanGain0, float chanGain1, bool linearFade) noexcept
{
    const double adv = v.phase + v.step * sumEff;
    const int k = (int) adv;

    if ((int) scratch.size() < k + 4 || v.fifo.getNumReady() < k)
    {
        underruns.fetch_add (1);

        if (v.levelTarget > 0.0f && v.sig.station >= 0)
        {
            requestVoice (v, v.sig, t, clock, v.fadeInc);   // re-sync with the clock and carry on
        }
        else
        {
            setIdle (v);
        }
        return;
    }

    float* s = scratch.data();
    s[0] = v.h[0];
    s[1] = v.h[1];
    s[2] = v.h[2];
    s[3] = v.h[3];
    popFrames (v, s + 4, k);

    const bool fading = std::fabs (v.level - v.levelTarget) > 0.0f;
    float level = v.level;
    const float target = v.levelTarget;
    const float inc = v.levelInc;
    const float dir = target > level ? 1.0f : -1.0f;
    const float chanStep = (chanGain1 - chanGain0) / (float) n;
    float chanGain = chanGain0;

    double p = v.phase;
    for (int i = 0; i < n; ++i)
    {
        const int idx = (int) p;
        const float f = (float) (p - (double) idx);
        float sample;

        if (cubic)                                        // 4-point cubic (Catmull-Rom) between the current and the next frame
        {
            const float y0 = s[idx], y1 = s[idx + 1], y2 = s[idx + 2], y3 = s[idx + 3];
            const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
            const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c = -0.5f * y0 + 0.5f * y2;
            sample = ((a * f + b) * f + c) * f + y1;
        }
        else                                              // zero-order hold
        {
            sample = s[idx + 1];
        }

        p += v.step * (double) eff[i];

        float g;
        if (fading)
        {
            level += dir * inc;
            if ((dir > 0.0f && level >= target) || (dir < 0.0f && level <= target))
                level = target;
            g = linearFade ? level : std::sin (level * juce::MathConstants<float>::halfPi);   // linear or constant power
        }
        else
        {
            g = (level >= 1.0f) ? 1.0f : 0.0f;
        }

        chanGain += chanStep;
        out[i] += sample * g * gain[i] * chanGain;
    }

    v.level = level;
    v.phase = adv - (double) k;
    v.h[0] = s[k];
    v.h[1] = s[k + 1];
    v.h[2] = s[k + 2];
    v.h[3] = s[k + 3];
    v.consumed += k;

    if (v.level <= 0.0f && v.levelTarget <= 0.0f)
        setIdle (v);
}

// 90s mode: lo-fi granular time stretch. The cursor moves through the file at the advance rate (eff, in session samples of
// path time); two Hann-windowed grains, half a grain apart, read the file at the pitch rate starting from the cursor. At
// pitch 1x and advance 1x the grains line up exactly and the result is the plain file. With the advance frozen, the grains
// keep replaying the same stretch of the file.
void RadioEngine::renderVoiceGrain (Voice& v, float* out, int n, const Table& t, int64_t clock,
                                    const float* eff, double sumEff, const float* pitch, bool cubic,
                                    float chanGain0, float chanGain1, bool linearFade) noexcept
{
    const double newCursor = v.cursor + v.step * sumEff;
    const int64_t pops = (int64_t) std::floor (newCursor) - v.consumed;

    if (pops < 0 || pops > (int64_t) scratch.size() || v.fifo.getNumReady() < (int) pops)
    {
        underruns.fetch_add (1);

        if (v.levelTarget > 0.0f && v.sig.station >= 0)
            requestVoice (v, v.sig, t, clock, v.fadeInc);
        else
            setIdle (v);
        return;
    }

    // Where the frames that are still in the FIFO start (peeked, not consumed).
    const int ready = v.fifo.getNumReady();
    int s0 = 0;
    if (ready > 0)
    {
        int s1, n1, s2, n2;
        v.fifo.prepareToRead (ready, s1, n1, s2, n2);
        s0 = s1;
    }

    const int64_t consumed = v.consumed;
    const int64_t histStart = v.histStart;
    const float* data = v.data.data();
    const float* hist = v.hist.data();

    auto fetch = [&] (int64_t a) -> float
    {
        if (a < histStart)
            a = histStart;
        if (a >= consumed)
        {
            const int64_t o = a - consumed;
            return o < (int64_t) ready ? data[(size_t) ((s0 + o) & (kFifoFrames - 1))] : 0.0f;
        }
        if (a >= consumed - (int64_t) kHistFrames)
            return hist[(size_t) (a & (kHistFrames - 1))];
        return 0.0f;
    };

    const bool fading = std::fabs (v.level - v.levelTarget) > 0.0f;
    float level = v.level;
    const float target = v.levelTarget;
    const float inc = v.levelInc;
    const float dir = target > level ? 1.0f : -1.0f;
    const float chanStep = (chanGain1 - chanGain0) / (float) n;
    float chanGain = chanGain0;

    const int hop = grainLen / 2;
    double cursor = v.cursor;

    for (int i = 0; i < n; ++i)
    {
        if (v.grainClock % hop == 0)                      // a new grain every half grain, starting at the cursor
            v.grains[(v.grainClock / hop) & 1] = Voice::Grain { true, cursor, 0 };
        v.grainClock = (v.grainClock + 1) % (hop * 2);

        const double pitchStep = v.step * (double) pitch[i];
        float sample = 0.0f;

        for (auto& g : v.grains)
        {
            if (! g.on)
                continue;

            const int64_t ip = (int64_t) std::floor (g.pos);
            const float f = (float) (g.pos - (double) ip);
            float y;

            if (cubic)
            {
                const float y0 = fetch (ip - 1), y1 = fetch (ip), y2 = fetch (ip + 1), y3 = fetch (ip + 2);
                const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
                const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                const float c = -0.5f * y0 + 0.5f * y2;
                y = ((a * f + b) * f + c) * f + y1;
            }
            else
            {
                y = fetch (ip);                                   // zero-order hold
            }

            sample += y * grainWin[(size_t) g.k];
            g.pos += pitchStep;
            if (++g.k >= grainLen)
                g.on = false;
        }

        cursor += v.step * (double) eff[i];

        float gn;
        if (fading)
        {
            level += dir * inc;
            if ((dir > 0.0f && level >= target) || (dir < 0.0f && level <= target))
                level = target;
            gn = linearFade ? level : std::sin (level * juce::MathConstants<float>::halfPi);
        }
        else
        {
            gn = (level >= 1.0f) ? 1.0f : 0.0f;
        }

        chanGain += chanStep;
        out[i] += sample * gn * chanGain;
    }

    // Keep the frames that were just passed so grains can still read them, then release them from the FIFO.
    if (pops > 0)
    {
        popFrames (v, scratch.data(), (int) pops);
        for (int64_t j = 0; j < pops; ++j)
            v.hist[(size_t) ((consumed + j) & (kHistFrames - 1))] = scratch[(size_t) j];
        v.consumed = consumed + pops;
    }

    v.cursor = cursor;
    v.level = level;

    if (v.level <= 0.0f && v.levelTarget <= 0.0f)
        setIdle (v);
}

void RadioEngine::publishStatus (const Table&, int64_t clock, int desiredSlot, int desiredFile, int slotCount, int domChan) noexcept
{
    uiClockSeconds.store ((double) clock / sessionRate, std::memory_order_relaxed);
    uiStation.store (desiredSlot);
    uiFile.store (desiredFile);
    uiCount.store (slotCount);

    // Loop window, as fractions of the file (for the display).
    const double nominal = 1048576.0;
    const PathCfg w = makeCfg (latchedStart, appliedEnd, (int64_t) nominal, LoopForward);
    uiWinLo.store ((double) w.lo / nominal);
    uiWinHi.store ((double) w.hi / nominal);
    uiWinRev.store (w.rev);

    double frac = -1.0;
    if (chans[(size_t) domChan].current >= 0)
    {
        const auto& v = voices[(size_t) chans[(size_t) domChan].current];
        if (v.state == Voice::State::Playing && v.numFrames > 0)
        {
            const int64_t tau = (int64_t) std::floor (tauFor (v.sig.station, clock, v.fileRate));
            frac = (double) pathAt (v.cfg, tau).pos / (double) v.numFrames;
        }
    }
    uiPlayFrac.store (frac, std::memory_order_relaxed);
}

// Radio mode: the station is scaled by how well it is received, then hiss and a whistle are added. Smoothed per sample.
void RadioEngine::applyRadio (float* out, int n, double ampT, double noiseT, double whistleT, double whistleHzT, double ssbHzT) noexcept
{
    const double sr = sessionRate;
    const double a = 1.0 - std::exp (-1.0 / (0.005 * sr));
    const double twoPi = 2.0 * juce::MathConstants<double>::pi;

    for (int i = 0; i < n; ++i)
    {
        rAmp       += (ampT - rAmp) * a;
        rNoise     += (noiseT - rNoise) * a;
        rWhistle   += (whistleT - rWhistle) * a;
        rWhistleHz += (whistleHzT - rWhistleHz) * a;
        rSsbHz     += (ssbHzT - rSsbHz) * a;

        float x = out[i];

        if (rSsbHz > 0.001)                              // ring modulation that fades out as the station is tuned exactly
        {
            const double depth = std::min (1.0, rSsbHz / 20.0);
            ssbPhase += twoPi * rSsbHz / sr;
            if (ssbPhase > twoPi) ssbPhase -= twoPi;
            x *= (float) ((1.0 - depth) + depth * std::cos (ssbPhase));
        }

        noiseState ^= noiseState << 13;
        noiseState ^= noiseState >> 17;
        noiseState ^= noiseState << 5;
        const float noise = (float) (int32_t) noiseState / 2147483648.0f;

        whistlePhase += twoPi * rWhistleHz / sr;
        if (whistlePhase > twoPi) whistlePhase -= twoPi;

        out[i] = (float) ((double) x * rAmp + (double) noise * rNoise + std::sin (whistlePhase) * rWhistle);
    }
}

void RadioEngine::process (float* out, int numSamples, const Params& p) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    std::fill (out, out + numSamples, 0.0f);

    const Table& t = acquireTable();

    if (t.id != knownTableId)
    {
        invalidateAll();
        knownTableId = t.id;

        // A random subfolder starts on a random file; a "next" subfolder starts on its first.
        for (int g = 0; g < kMaxFiles; ++g)
        {
            const auto& sl = t.slots[(size_t) g];
            if (sl.mode == SlotRandom && sl.numFiles > 1)
            {
                rngState ^= rngState << 13;
                rngState ^= rngState >> 17;
                rngState ^= rngState << 5;
                slotCur[(size_t) g] = (int) (rngState % (uint32_t) sl.numFiles);
            }
        }
    }

    // The dial picks a slot in the chosen bank; a slot is one file, or a subfolder that supplies one file at a time.
    const int bank       = juce::jlimit (0, kNumBanks - 1, p.bank);
    const int count      = t.bankCount[(size_t) bank];
    const int firstSlot  = t.bankFirst[(size_t) bank];
    const int divider    = juce::jlimit (1, kStartSteps, p.startDivider);

    const bool soft      = p.tunerMode == TunerSoft;
    const bool radio     = p.tunerMode == TunerRadio;
    const bool smoothX   = soft || radio;
    const bool linearFade = p.fadeMode == FadeLinear;

    auto slotAt = [count] (double x)      // dial position (0 .. count - 1) of the station whose segment contains x, -1 if none
    {
        return count > 0 ? juce::jlimit (0, count - 1, (int) std::floor (juce::jlimit (0.0, 1.0, x) * (double) count)) : -1;
    };
    auto fileOfSlot = [&] (int g) { return g >= 0 ? t.slots[(size_t) g].firstFile + slotCur[(size_t) g] : -1; };

    int64_t clock = clockSamples.load (std::memory_order_relaxed);

    // Radio clock source. Host-synced: the DAW's sample position decides; the plug-in is silent while the transport is stopped.
    if (p.hostSync)
    {
        if (! p.hostPlaying || p.hostSamples < 0)
        {
            const int ds = slotAt (p.station);
            publishStatus (t, clock, ds, fileOfSlot (ds >= 0 ? firstSlot + ds : -1), count, soft && ds >= 0 ? (ds & 1) : 0);
            readingTable.store (-1);
            return;
        }

        if (p.hostSamples != clock)                       // the transport was moved: start every voice again at the new time
        {
            clock = p.hostSamples;
            for (auto& v : voices)
                setIdle (v);
            for (auto& ch : chans)
            {
                ch.current = ch.pending = -1;
                ch.committed = ch.pendingSig = Sig {};
            }
        }
    }

    if (! settingsInit)
    {
        latchedStart = quantiseStep (p.startNorm, divider);
        appliedEnd   = quantiseStep (p.endNorm, 1);
        seenStart = latchedStart;
        seenEnd   = appliedEnd;
        startStable = endStable = 0;
        settingsInit = true;
    }

    const double sr = sessionRate;
    const float xfInc   = (float) (1.0 / std::max (1.0, (double) p.crossfadeMs * 0.001 * sr));
    const float fastInc = (float) (1.0 / (kFastFadeSeconds * sr));
    const int64_t settle = (int64_t) (kSettleSeconds * sr);
    const bool cubic = ! p.grit;

    // Speed. Tape: percent of original speed (negative = backwards). Notes: semitones (whole semitones if quantised).
    // 90s: pitch and advance rate are separate (and the mode is not available with the Soft and Radio tuners: Tape is used then).
    const int speedMode = (p.speedMode == Speed90s && p.tunerMode != TunerSharp) ? (int) SpeedTape : p.speedMode;
    grainNow = speedMode == Speed90s;

    double speedTarget = 1.0;
    const double pitchTarget = juce::jlimit (-kMaxSpeed, kMaxSpeed, (double) p.speedTape / 100.0);
    if (speedMode == SpeedTape)
    {
        speedTarget = pitchTarget;
    }
    else if (grainNow)
    {
        speedTarget = juce::jlimit (0.0, kMaxSpeed, (double) p.advance90 / 100.0);
    }
    else if (speedMode == SpeedNotes)
    {
        double st = juce::jlimit (-48.0, 24.0, (double) p.speedNotes);
        if (p.quantisePitch)
            st = std::round (st);
        speedTarget = std::exp2 (st / 12.0);
    }

    if (! speedInit)
    {
        speedSm = speedTarget;
        speedInit = true;
    }

    const double speedAlpha = 1.0 - std::exp (-1.0 / (kSpeedSmoothSeconds * sr));
    const double pauseInc = 1.0 / ((speedMode == SpeedNotes ? kNotesPauseSeconds : kPauseSlewSeconds) * sr);
    const double pauseTarget = p.paused ? 0.0 : 1.0;

    // Grit mode: output rate and filter.
    gritHold = sr / kGritRate;
    const double gritCoef = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * kGritFilterHz / sr);

    int domSlot = -1, domFile = -1, domChan = 0;
    int done = 0;

    while (done < numSamples)
    {
        const int n = std::min (kRenderChunk, numSamples - done);

        // END (and START, when it is set to act at once) must hold still briefly before it is used.
        const int endNow = quantiseStep (p.endNorm, 1);
        if (endNow != seenEnd) { seenEnd = endNow; endStable = 0; } else endStable += n;
        if (endStable >= settle)
            appliedEnd = seenEnd;

        const int startNow = quantiseStep (p.startNorm, divider);
        if (startNow != seenStart) { seenStart = startNow; startStable = 0; } else startStable += n;
        if (p.startImmediate && startStable >= settle)
            latchedStart = seenStart;

        // Station position. Sharp follows the knob directly; Soft and Radio glide to it so the blend has no steps.
        if (! smoothX || ! xInit)
        {
            xSm = (double) p.station;
            xInit = true;
        }
        else
        {
            xSm += ((double) p.station - xSm) * (1.0 - std::exp (-(double) n / (kPositionSmoothSeconds * sr)));
        }
        const double x = juce::jlimit (0.0, 1.0, xSm);

        // Which stations are wanted, on which channel, and how loud. Sharp and Radio: the station whose segment holds the
        // knob, on channel 0. Soft: the two stations either side of the knob, blended (stations sit in the middle of their
        // segment; beyond the first and last station only that station plays).
        struct Want { int slot = -1; int g = -1; float gain = 0.0f; };
        Want want[2];
        domSlot = slotAt (x);

        if (count > 0)
        {
            if (soft)
            {
                const double pos = juce::jlimit (0.0, (double) (count - 1), x * (double) count - 0.5);
                int lo = juce::jlimit (0, count - 1, (int) std::floor (pos));
                double f = pos - (double) lo;
                if (lo >= count - 1) { lo = count - 1; f = 0.0; }

                const float fl = (float) f;
                const float gLo = linearFade ? 1.0f - fl : std::cos (fl * juce::MathConstants<float>::halfPi);
                const float gHi = linearFade ? fl        : std::sin (fl * juce::MathConstants<float>::halfPi);

                want[lo & 1] = { lo, firstSlot + lo, gLo };
                if (lo + 1 < count)
                    want[(lo + 1) & 1] = { lo + 1, firstSlot + lo + 1, gHi };
            }
            else
            {
                want[0] = { domSlot, firstSlot + domSlot, 1.0f };
            }
        }

        domChan = soft && domSlot >= 0 ? (domSlot & 1) : 0;

        // Reset: restart the tuned station(s) from START. START is read at this moment. In Soft mode every station that is
        // audible restarts. A file named "...skipN" makes the next N resets be ignored while it plays; a subfolder moves
        // on to its next file.
        if (resetFlag.exchange (false))
        {
            bool any = false;
            for (const auto& w : want)
            {
                if (w.g < 0 || ! (w.gain > 0.0f))
                    continue;

                any = true;
                if (skipLeft[(size_t) w.g] > 0)
                {
                    skipLeft[(size_t) w.g] -= 1;              // ignored
                    diagIgnored.fetch_add (1, std::memory_order_relaxed);
                }
                else
                {
                    latchedStart = startNow;
                    const int before = fileOfSlot (w.g);
                    reselect (w.g, t);
                    const int after = fileOfSlot (w.g);
                    diagApplied.fetch_add (1, std::memory_order_relaxed);
                    if (after != before)
                        diagFileChanges.fetch_add (1, std::memory_order_relaxed);
                    refClock[(size_t) after] = (double) clock;
                    epochOf[(size_t) after] += 1;
                    skipLeft[(size_t) w.g] = (int) t.skip[(size_t) after];
                }
            }

            if (! any)
                latchedStart = startNow;
        }

        // Tuning to a station: a subfolder may pick a new file (if enabled), skipN counts start, and with radio start off
        // the station restarts from START.
        int file[2] = { -1, -1 };
        for (int c = 0; c < 2; ++c)
        {
            auto& ch = chans[(size_t) c];
            const int g = want[c].g;

            if (g != ch.lastSlot)
            {
                if (g >= 0)
                {
                    bool restart = ! p.radioStart;

                    if (p.reselectOnTune && t.slots[(size_t) g].numFiles > 1)
                    {
                        reselect (g, t);
                        restart = true;
                    }

                    if (restart)
                    {
                        const int f = fileOfSlot (g);
                        latchedStart = startNow;
                        refClock[(size_t) f] = (double) clock;
                        epochOf[(size_t) f] += 1;
                    }

                    skipLeft[(size_t) g] = (int) t.skip[(size_t) fileOfSlot (g)];
                }
                ch.lastSlot = g;
            }

            file[c] = fileOfSlot (g);
        }

        domFile = file[domChan];

        // Playing backwards needs a voice that reads the path backwards: switch once the speed has crossed zero.
        if (grainNow)                              curDir = 1;       // 90s mode never plays the file backwards (the pitch does)
        else if (curDir > 0 && speedSm < -0.002)   curDir = -1;
        else if (curDir < 0 && speedSm > 0.002)   curDir = 1;

        for (int c = 0; c < 2; ++c)
        {
            Sig desired;
            if (file[c] >= 0)
            {
                desired.station   = file[c];
                desired.startStep = latchedStart;
                desired.endStep   = appliedEnd;
                desired.loopMode  = juce::jlimit (0, 2, p.loopMode);
                desired.epoch     = epochOf[(size_t) file[c]];
                desired.dir       = curDir;
            }

            handleTargetChange (c, desired, t, clock, soft ? fastInc : xfInc, fastInc);   // Soft mode does not use the crossfade time
        }

        for (int i = 0; i < kNumVoices; ++i)
            if (voices[(size_t) i].state == Voice::State::Loading)
                tryAlign (i, t, clock);

        // Per-sample speed: smoothed speed knob times the pause ramp. A voice only plays in its own direction
        // and fades to silence as the speed approaches zero (no steady DC when paused or stopped).
        double sumPos = 0.0, sumNeg = 0.0, sumAll = 0.0;
        for (int i = 0; i < n; ++i)
        {
            if (pauseMul < pauseTarget)      pauseMul = std::min (pauseTarget, pauseMul + pauseInc);
            else if (pauseMul > pauseTarget) pauseMul = std::max (pauseTarget, pauseMul - pauseInc);

            speedSm += (speedTarget - speedSm) * speedAlpha;
            pitchSm += (pitchTarget - pitchSm) * speedAlpha;
            pitchArr[(size_t) i] = (float) pitchSm;

            const double e = speedSm * pauseMul;
            effPos[(size_t) i] = (float) std::max (0.0, e);
            effNeg[(size_t) i] = (float) std::max (0.0, -e);
            gainPos[(size_t) i] = grainNow ? 1.0f : std::min (1.0f, effPos[(size_t) i] * 20.0f);   // 90s keeps sounding when the advance stops
            gainNeg[(size_t) i] = std::min (1.0f, effNeg[(size_t) i] * 20.0f);
            sumPos += (double) effPos[(size_t) i];
            sumNeg += (double) effNeg[(size_t) i];
            sumAll += e;
        }

        // Channel gains. Soft: each channel carries its blend gain, but only once the channel really holds the wanted file
        // (so a neighbour that is still loading is silent instead of playing the wrong file). Other modes: full.
        float chanTarget[2] = { 1.0f, 1.0f };
        if (soft)
            for (int c = 0; c < 2; ++c)
                chanTarget[c] = (file[c] >= 0 && chans[(size_t) c].committed.station == file[c]) ? want[c].gain : 0.0f;

        for (int i = 0; i < kNumVoices; ++i)
        {
            auto& v = voices[(size_t) i];
            if (v.state == Voice::State::Playing)
            {
                const float g0 = chans[(size_t) v.chan].gain, g1 = chanTarget[v.chan];

                if (v.step * kMaxSpeed > kMaxStep * kMaxSpeed || v.step <= 0.0)
                    setIdle (v);
                else if (v.sig.dir > 0 && v.grain != grainNow)      // 90s mode switched on or off: join again in the other engine
                    requestVoice (v, v.sig, t, clock, v.fadeInc);
                else if (v.sig.dir > 0 && v.grain)
                    renderVoiceGrain (v, out + done, n, t, clock, effPos.data(), sumPos, pitchArr.data(), cubic, g0, g1, linearFade);
                else if (v.sig.dir > 0)
                    renderVoice (v, out + done, n, t, clock, effPos.data(), sumPos, gainPos.data(), cubic, g0, g1, linearFade);
                else
                    renderVoice (v, out + done, n, t, clock, effNeg.data(), sumNeg, gainNeg.data(), cubic, g0, g1, linearFade);
            }
        }

        for (int c = 0; c < 2; ++c)
            chans[(size_t) c].gain = chanTarget[c];

        // The tuned station moves by (speed x time); the difference to real time is what it has fallen behind the radio clock
        // (negative if it has moved ahead). Other stations are not affected. In Soft mode both blended stations are tuned.
        for (int c = 0; c < 2; ++c)
        {
            const auto& ch = chans[(size_t) c];
            if (ch.committed.station >= 0 && chanTarget[c] > 0.0f)
                refClock[(size_t) ch.committed.station] += (double) n - sumAll;
        }

        // Radio mode: reception strength, hiss and whistle depend on how far the knob is from the middle of the station.
        if (radio)
        {
            double ampT = 0.0, noiseT = 0.0, whistleT = 0.0, whistleHzT = 0.0, ssbHzT = 0.0;

            if (want[0].g >= 0)
            {
                const double pos = x * (double) count;
                const double rn = std::min (1.0, 2.0 * std::fabs (pos - ((double) want[0].slot + 0.5)));   // 0 = exactly tuned, 1 = halfway to the next station
                const double s = juce::jlimit (0.0, 1.0, 1.0 - rn / kRadioReception);                       // 1 at the station, 0 outside reception
                const double shape = s * s * (3.0 - 2.0 * s);

                ampT = (double) stationStrength (want[0].g, p.minStrength) * shape;
                noiseT = (double) juce::jlimit (0.0f, 100.0f, p.noiseVol) / 100.0 * kNoiseLevel * (1.0 - ampT);
                whistleHzT = kWhistleMaxHz * std::min (1.0, rn / kRadioReception);
                whistleT = (double) juce::jlimit (0.0f, 100.0f, p.whistleVol) / 100.0 * kWhistleLevel * s * std::min (1.0, whistleHzT / 80.0);
                ssbHzT = p.ssb ? rn * kSsbMaxHz : 0.0;
            }

            applyRadio (out + done, n, ampT, noiseT, whistleT, whistleHzT, ssbHzT);
        }

        clock += n;
        done += n;
    }

    // Grit mode: 8-bit, about 20 kHz, then an emulated post-DAC filter.
    if (p.grit)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            gritAcc += 1.0;
            if (gritAcc >= gritHold)
            {
                gritAcc -= gritHold;
                gritValue = std::round (juce::jlimit (-1.0f, 1.0f, out[i]) * 127.0f) / 127.0f;
            }
            gritLP += ((double) gritValue - gritLP) * gritCoef;
            out[i] = (float) gritLP;
        }
    }

    clockSamples.store (clock, std::memory_order_relaxed);
    publishStatus (t, clock, domSlot, domFile, count, domChan);
    readingTable.store (-1);
}

//==============================================================================
// Stream thread

// Copies count frames of the file starting at start into dst (zeros if the read fails).
void RadioEngine::fetchFrames (Voice& v, float* dst, int64_t start, int count)
{
    if (count <= 0)
        return;

    if (v.cacheValid)
    {
        const int64_t off = start - v.scfg.lo;
        if (off >= 0 && off + count <= (int64_t) v.cache.size())
        {
            std::copy_n (v.cache.data() + off, count, dst);
            return;
        }
        std::fill_n (dst, count, 0.0f);
        return;
    }

    float* d[1] = { dst };
    if (v.reader == nullptr || ! v.reader->read (d, 1, start, count))
        std::fill_n (dst, count, 0.0f);
}

// Produces the next num frames along the station's path into streamTmp.
void RadioEngine::generate (Voice& v, int num)
{
    float* tmp = streamTmp.data();
    int produced = 0;

    while (produced < num)
    {
        if (v.sended)
        {
            std::fill (tmp + produced, tmp + num, 0.0f);
            return;
        }

        const int want = num - produced;

        if (! v.sback)
        {
            const int run = (int) std::min<int64_t> (want, v.scfg.hi - v.spos);
            fetchFrames (v, tmp + produced, v.spos, run);
            v.spos += run;
            produced += run;

            if (v.spos >= v.scfg.hi)
            {
                if (v.scfg.mode == LoopOff)           { v.sended = true; }
                else if (v.scfg.mode == LoopForward)  { v.spos = v.scfg.lo; }
                else                                  { v.sback = true; v.spos = v.scfg.hi - 1; }
            }
        }
        else
        {
            const int run = (int) std::min<int64_t> (want, v.spos - v.scfg.lo + 1);
            fetchFrames (v, tmp + produced, v.spos - run + 1, run);
            std::reverse (tmp + produced, tmp + produced + run);
            v.spos -= run;
            produced += run;

            if (v.spos < v.scfg.lo)
            {
                if (v.scfg.mode == LoopOff)           { v.sended = true; }
                else if (v.scfg.mode == LoopForward)  { v.spos = v.scfg.hi - 1; }   // a reversed window repeats
                else                                  { v.sback = false; v.spos = v.scfg.lo; }
            }
        }
    }
}

bool RadioEngine::serviceVoice (Voice& v)
{
    bool work = false;
    const uint32_t gen = v.reqGen.load (std::memory_order_acquire);

    if (gen != v.handledGen)
    {
        const int      station = v.reqStation.load (std::memory_order_relaxed);
        const int64_t  lo      = v.reqLo.load (std::memory_order_relaxed);
        const int64_t  hi      = v.reqHi.load (std::memory_order_relaxed);
        const int      mode    = v.reqMode.load (std::memory_order_relaxed);
        const bool     rev     = v.reqRev.load (std::memory_order_relaxed) != 0;
        const int64_t  tau     = v.reqTau.load (std::memory_order_relaxed);
        const int      dir     = v.reqDir.load (std::memory_order_relaxed);
        const uint32_t tableId = v.reqTable.load (std::memory_order_relaxed);

        if (v.reqGen.load (std::memory_order_acquire) != gen)
            return true;                                 // request changed while reading it; look again

        v.handledGen = gen;
        v.reader.reset();
        v.streaming = false;
        v.cacheValid = false;
        v.fifo.reset();                                  // safe: the audio thread ignores this voice until readyGen == gen

        if (station >= 0)
        {
            std::shared_ptr<const Library> lib;
            uint32_t libId = 0;
            {
                const juce::ScopedLock l (libLock);
                lib = streamLib;
                libId = streamLibId;
            }

            if (lib != nullptr && libId == tableId && station < (int) lib->stations.size())
            {
                v.reader.reset (streamFormats.createReaderFor (lib->stations[(size_t) station].file));

                if (v.reader != nullptr && v.reader->lengthInSamples >= 2)
                {
                    v.fileLen = (int64_t) v.reader->lengthInSamples;

                    PathCfg c;
                    c.lo   = juce::jlimit<int64_t> (0, v.fileLen - 1, lo);
                    c.hi   = juce::jlimit<int64_t> (c.lo + 1, v.fileLen, hi);
                    c.rev  = rev;
                    c.mode = mode;
                    v.scfg = c;

                    // dir < 0 reads the same path backwards: same position, opposite direction of travel.
                    const PathPoint pt = pathAt (c, tau);
                    v.spos   = pt.pos;
                    v.sback  = dir > 0 ? pt.back : ! pt.back;
                    v.sended = pt.ended || (dir < 0 && mode == LoopOff && tau <= 0);

                    // Short loop windows are held in a small cache so tight loops do not hammer the disk.
                    if (c.length() <= kCacheFrames)
                    {
                        v.cache.resize ((size_t) c.length());
                        float* d[1] = { v.cache.data() };
                        v.cacheValid = v.reader->read (d, 1, c.lo, (int) c.length());
                    }

                    v.streaming = true;
                }
            }

            if (! v.streaming)
            {
                v.reader.reset();
                v.failedGen.store (gen, std::memory_order_release);
            }
        }

        work = true;
    }

    if (v.streaming && v.handledGen == v.reqGen.load (std::memory_order_relaxed))
    {
        for (int guard = 0; guard < 4; ++guard)
        {
            if (v.fifo.getFreeSpace() < kChunkFrames)
                break;

            generate (v, kChunkFrames);

            int s1, n1, s2, n2;
            v.fifo.prepareToWrite (kChunkFrames, s1, n1, s2, n2);
            if (n1 > 0) std::copy_n (streamTmp.data(), n1, v.data.data() + s1);
            if (n2 > 0) std::copy_n (streamTmp.data() + n1, n2, v.data.data() + s2);
            v.fifo.finishedWrite (n1 + n2);

            work = true;
        }

        if (v.readyGen.load (std::memory_order_relaxed) != v.handledGen
            && v.fifo.getNumReady() >= kPrefillFrames)
            v.readyGen.store (v.handledGen, std::memory_order_release);
    }

    return work;
}
} // namespace rm
