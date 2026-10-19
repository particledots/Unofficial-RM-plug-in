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
    scratch.assign ((size_t) (kMaxStep * kRenderChunk) + 16, 0.0f);

    for (auto& v : voices)
    {
        v.gen = 1;
        v.reqGen.store (1);
        v.handledGen = 1;
    }

    refClock.fill (0.0);
    epochOf.fill (0u);

    streamThread = std::make_unique<StreamThread> (*this);
    streamThread->startThread();
}

RadioEngine::~RadioEngine()
{
    streamThread->stopThread (3000);
    streamThread.reset();
}

void RadioEngine::prepare (double sessionSampleRate)
{
    sessionRate = sessionSampleRate > 0.0 ? sessionSampleRate : 44100.0;
    clockSamples.store (0);
    underruns.store (0);
    knownTableId = tables[(size_t) activeTable.load()].id;
    settingsInit = false;
    speedMul = 1.0;
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
    t.id    = id;
    t.count = 0;

    if (library != nullptr)
    {
        t.count = (int) std::min<size_t> (library->stations.size(), (size_t) kMaxStationsPerBank);
        for (int i = 0; i < t.count; ++i)
            t.info[(size_t) i] = library->stations[(size_t) i].info;
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
    const double elapsed = (double) clock - refClock[(size_t) station];
    return elapsed > 0.0 ? elapsed * (fileRate / sessionRate) : 0.0;
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

    current = pending = -1;
    committedSig = pendingSig = Sig {};
    lastDesiredStation = -2;
    badStation.fill (false);
    refClock.fill (0.0);
    epochOf.fill (0u);
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
    v.cfg       = makeCfg (sig.startStep, sig.endStep, info.numFrames, sig.loopMode);
    v.tauStart  = (int64_t) std::floor (tauFor (sig.station, clock, info.sampleRate));

    v.reqStation.store (sig.station, std::memory_order_relaxed);
    v.reqLo.store (v.cfg.lo, std::memory_order_relaxed);
    v.reqHi.store (v.cfg.hi, std::memory_order_relaxed);
    v.reqMode.store (v.cfg.mode, std::memory_order_relaxed);
    v.reqRev.store (v.cfg.rev ? 1 : 0, std::memory_order_relaxed);
    v.reqTau.store (v.tauStart, std::memory_order_relaxed);
    v.reqTable.store (t.id, std::memory_order_relaxed);
    v.reqGen.store (v.gen, std::memory_order_release);
}

int RadioEngine::allocVoice() noexcept
{
    for (int i = 0; i < kNumVoices; ++i)
        if (voices[(size_t) i].state == Voice::State::Idle && i != current && i != pending)
            return i;

    int best = -1;
    float bestLevel = 2.0f;
    for (int i = 0; i < kNumVoices; ++i)
    {
        auto& v = voices[(size_t) i];
        if (i == current || i == pending)
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

void RadioEngine::handleTargetChange (Sig desired, const Table& t, int64_t clock, float xfInc, float fastInc) noexcept
{
    if (desired.station >= 0 && badStation[(size_t) desired.station])
        desired = committedSig;                          // never retry a file that failed to open

    if (desired == committedSig)
    {
        if (pending >= 0)                                // tuned back to what is already playing
        {
            setIdle (voices[(size_t) pending]);
            pending = -1;
            pendingSig = Sig {};
        }
        return;
    }

    if (desired.station >= 0 && desired == pendingSig)
        return;

    if (desired.station < 0)                             // folder is empty: fade out whatever is playing
    {
        if (pending >= 0)
        {
            setIdle (voices[(size_t) pending]);
            pending = -1;
            pendingSig = Sig {};
        }
        if (current >= 0)
        {
            voices[(size_t) current].levelTarget = 0.0f;
            voices[(size_t) current].levelInc    = xfInc;
        }
        current = -1;
        committedSig = Sig {};
        return;
    }

    // A new station uses the crossfade time; Reset, loop and window changes on the same station use a short fade.
    const float fade = (desired.station != committedSig.station) ? xfInc : std::max (xfInc, fastInc);

    if (pending >= 0)                                    // retarget the voice that is still loading
    {
        requestVoice (voices[(size_t) pending], desired, t, clock, fade);
        pendingSig = desired;
        return;
    }

    const int vi = allocVoice();
    if (vi < 0)
        return;

    auto& v = voices[(size_t) vi];
    v.level = 0.0f;
    v.levelTarget = 1.0f;
    requestVoice (v, desired, t, clock, fade);
    pending = vi;
    pendingSig = desired;
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

    if (v.failedGen.load (std::memory_order_acquire) == v.gen)
    {
        if (v.sig.station >= 0 && v.sig.station < kMaxStationsPerBank)
            badStation[(size_t) v.sig.station] = true;

        if (voiceIndex == pending)
        {
            pending = -1;
            pendingSig = Sig {};
        }
        if (voiceIndex == current)
        {
            current = -1;
            committedSig = Sig {};
        }
        setIdle (v);
        return;
    }

    if (v.readyGen.load (std::memory_order_acquire) != v.gen)
        return;

    // The voice began where the station was when it was requested. Skip ahead by however much
    // the radio clock has moved since, so it joins in exactly where the station "is".
    const int64_t tauNow = (int64_t) std::floor (tauFor (v.sig.station, clock, v.fileRate));
    const int64_t skip = std::max<int64_t> (0, tauNow - v.tauStart);

    if (skip + 2 > kFifoFrames - 2048)                   // too far behind to catch up from the buffer
    {
        requestVoice (v, v.sig, t, clock, v.fadeInc);
        return;
    }

    if ((int64_t) v.fifo.getNumReady() < skip + 2)
        return;                                          // wait for the stream thread to read further

    popFrames (v, nullptr, (int) skip);
    float two[2] {};
    popFrames (v, two, 2);
    v.s0 = two[0];
    v.s1 = two[1];
    v.phase = 0.0;
    v.consumed = skip;
    v.state = Voice::State::Playing;

    if (voiceIndex == pending)
    {
        v.level = 0.0f;
        v.levelTarget = 1.0f;
        v.levelInc = v.fadeInc;

        if (current >= 0)
        {
            auto& old = voices[(size_t) current];
            old.levelTarget = 0.0f;
            old.levelInc = v.fadeInc;
            if (old.state == Voice::State::Loading)
                setIdle (old);
        }

        current = pending;
        committedSig = pendingSig;
        pending = -1;
        pendingSig = Sig {};
    }
}

void RadioEngine::renderVoice (Voice& v, float* out, int n, const Table& t, int64_t clock,
                               const float* mul, double sumMul) noexcept
{
    const double adv = v.phase + v.step * sumMul;
    const int k = (int) adv;

    if ((int) scratch.size() < k + 2 || v.fifo.getNumReady() < k)
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
    s[0] = v.s0;
    s[1] = v.s1;
    popFrames (v, s + 2, k);

    const bool fading = std::fabs (v.level - v.levelTarget) > 0.0f;
    float level = v.level;
    const float target = v.levelTarget;
    const float inc = v.levelInc;
    const float dir = target > level ? 1.0f : -1.0f;

    double p = v.phase;
    for (int i = 0; i < n; ++i)
    {
        const int idx = (int) p;
        const float frac = (float) (p - (double) idx);
        const float sample = s[idx] + (s[idx + 1] - s[idx]) * frac;
        p += v.step * (double) mul[i];

        float g;
        if (fading)
        {
            level += dir * inc;
            if ((dir > 0.0f && level >= target) || (dir < 0.0f && level <= target))
                level = target;
            g = std::sin (level * juce::MathConstants<float>::halfPi);   // constant power
        }
        else
        {
            g = (level >= 1.0f) ? 1.0f : 0.0f;
        }

        out[i] += sample * g;
    }

    v.level = level;
    v.phase = adv - (double) k;
    v.s0 = s[k];
    v.s1 = s[k + 1];
    v.consumed += k;

    if (v.level <= 0.0f && v.levelTarget <= 0.0f)
        setIdle (v);
}

void RadioEngine::publishStatus (const Table& t, int64_t clock, int desiredStation) noexcept
{
    uiClockSeconds.store ((double) clock / sessionRate, std::memory_order_relaxed);
    uiStation.store (desiredStation);
    uiCount.store (t.count);

    // Loop window, as fractions of the file (for the display).
    const double nominal = 1048576.0;
    const PathCfg w = makeCfg (latchedStart, appliedEnd, (int64_t) nominal, LoopForward);
    uiWinLo.store ((double) w.lo / nominal);
    uiWinHi.store ((double) w.hi / nominal);
    uiWinRev.store (w.rev);

    double frac = -1.0;
    if (current >= 0)
    {
        const auto& v = voices[(size_t) current];
        if (v.state == Voice::State::Playing && v.numFrames > 0)
        {
            const int64_t tau = (int64_t) std::floor (tauFor (v.sig.station, clock, v.fileRate));
            frac = (double) pathAt (v.cfg, tau).pos / (double) v.numFrames;
        }
    }
    uiPlayFrac.store (frac, std::memory_order_relaxed);
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
    }

    const int count = t.count;
    int desiredStation = -1;
    if (count > 0)
        desiredStation = juce::jlimit (0, count - 1, (int) std::floor (juce::jlimit (0.0f, 1.0f, p.station) * (float) count));

    int64_t clock = clockSamples.load (std::memory_order_relaxed);

    // Radio clock source. Host-synced: the DAW's sample position decides; the plug-in is silent while the transport is stopped.
    if (p.hostSync)
    {
        if (! p.hostPlaying || p.hostSamples < 0)
        {
            publishStatus (t, clock, desiredStation);
            readingTable.store (-1);
            return;
        }

        if (p.hostSamples != clock)                       // the transport was moved: start every voice again at the new time
        {
            clock = p.hostSamples;
            for (auto& v : voices)
                setIdle (v);
            current = pending = -1;
            committedSig = pendingSig = Sig {};
        }
    }

    if (! settingsInit)
    {
        latchedStart = quantiseStep (p.startNorm, kStartDivider);
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
    const double mInc = 1.0 / (kPauseSlewSeconds * sr);
    const double mTarget = p.paused ? 0.0 : 1.0;

    int done = 0;

    while (done < numSamples)
    {
        const int n = std::min (kRenderChunk, numSamples - done);

        // END (and START, when it is set to act at once) must hold still briefly before it is used.
        const int endNow = quantiseStep (p.endNorm, 1);
        if (endNow != seenEnd) { seenEnd = endNow; endStable = 0; } else endStable += n;
        if (endStable >= settle)
            appliedEnd = seenEnd;

        const int startNow = quantiseStep (p.startNorm, kStartDivider);
        if (startNow != seenStart) { seenStart = startNow; startStable = 0; } else startStable += n;
        if (p.startImmediate && startStable >= settle)
            latchedStart = seenStart;

        // Reset: restart the tuned station from START. START is read at this moment.
        if (resetFlag.exchange (false))
        {
            latchedStart = startNow;
            if (desiredStation >= 0)
            {
                refClock[(size_t) desiredStation] = (double) clock;
                epochOf[(size_t) desiredStation] += 1;
            }
        }

        // With radio start off, tuning to a station restarts it from START.
        if (desiredStation != lastDesiredStation)
        {
            if (! p.radioStart && desiredStation >= 0)
            {
                latchedStart = startNow;
                refClock[(size_t) desiredStation] = (double) clock;
                epochOf[(size_t) desiredStation] += 1;
            }
            lastDesiredStation = desiredStation;
        }

        Sig desired;
        if (desiredStation >= 0)
        {
            desired.station   = desiredStation;
            desired.startStep = latchedStart;
            desired.endStep   = appliedEnd;
            desired.loopMode  = juce::jlimit (0, 2, p.loopMode);
            desired.epoch     = epochOf[(size_t) desiredStation];
        }

        handleTargetChange (desired, t, clock, xfInc, fastInc);

        for (int i = 0; i < kNumVoices; ++i)
            if (voices[(size_t) i].state == Voice::State::Loading)
                tryAlign (i, t, clock);

        // Pause: tape-style, the speed multiplier slews to zero and back.
        double m = speedMul, sumMul = 0.0;
        for (int i = 0; i < n; ++i)
        {
            if (m < mTarget)      m = std::min (mTarget, m + mInc);
            else if (m > mTarget) m = std::max (mTarget, m - mInc);
            mulBuf[(size_t) i] = (float) m;
            sumMul += (double) mulBuf[(size_t) i];
        }
        speedMul = m;

        for (int i = 0; i < kNumVoices; ++i)
        {
            auto& v = voices[(size_t) i];
            if (v.state == Voice::State::Playing)
            {
                if (v.step > kMaxStep || v.step <= 0.0)
                    setIdle (v);
                else
                    renderVoice (v, out + done, n, t, clock, mulBuf.data(), sumMul);
            }
        }

        // Time spent paused is time the tuned station fell behind the radio clock.
        if (committedSig.station >= 0 && sumMul < (double) n)
            refClock[(size_t) committedSig.station] += (double) n - sumMul;

        clock += n;
        done += n;
    }

    clockSamples.store (clock, std::memory_order_relaxed);
    publishStatus (t, clock, desiredStation);
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

                    const PathPoint pt = pathAt (c, tau);
                    v.spos   = pt.pos;
                    v.sback  = pt.back;
                    v.sended = pt.ended;

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
