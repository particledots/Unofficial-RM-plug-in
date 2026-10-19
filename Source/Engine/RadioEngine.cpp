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

void RadioEngine::prepare (double sessionSampleRate)
{
    sessionRate = sessionSampleRate > 0.0 ? sessionSampleRate : 44100.0;
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

    current = pending = -1;
    committedSig = pendingSig = Sig {};
    lastDesiredSlot = -2;
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
        if (v.sig.station >= 0 && v.sig.station < kMaxFiles)
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

    popFrames (v, nullptr, (int) skip);
    float three[3] {};
    popFrames (v, three, 3);
    v.h[0] = three[0];                                   // no earlier frame is known: repeat the first one
    v.h[1] = three[0];
    v.h[2] = three[1];
    v.h[3] = three[2];
    v.phase = skipReal - (double) skip;
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
                               const float* eff, double sumEff, const float* gain, bool cubic) noexcept
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
            g = std::sin (level * juce::MathConstants<float>::halfPi);   // constant power
        }
        else
        {
            g = (level >= 1.0f) ? 1.0f : 0.0f;
        }

        out[i] += sample * g * gain[i];
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

void RadioEngine::publishStatus (const Table&, int64_t clock, int desiredSlot, int desiredFile, int slotCount) noexcept
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

    int desiredSlot = -1;                  // position on the dial, 0 .. count - 1
    if (count > 0)
        desiredSlot = juce::jlimit (0, count - 1, (int) std::floor (juce::jlimit (0.0f, 1.0f, p.station) * (float) count));

    const int globalSlot = desiredSlot >= 0 ? firstSlot + desiredSlot : -1;
    auto fileOfSlot = [&] (int g) { return g >= 0 ? t.slots[(size_t) g].firstFile + slotCur[(size_t) g] : -1; };
    int desiredStation = fileOfSlot (globalSlot);   // index of the file that plays (all per-file arrays use this)

    int64_t clock = clockSamples.load (std::memory_order_relaxed);

    // Radio clock source. Host-synced: the DAW's sample position decides; the plug-in is silent while the transport is stopped.
    if (p.hostSync)
    {
        if (! p.hostPlaying || p.hostSamples < 0)
        {
            publishStatus (t, clock, desiredSlot, desiredStation, count);
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
    // 90s mode is not implemented yet and plays at original speed.
    double speedTarget = 1.0;
    if (p.speedMode == SpeedTape)
    {
        speedTarget = juce::jlimit (-kMaxSpeed, kMaxSpeed, (double) p.speedTape / 100.0);
    }
    else if (p.speedMode == SpeedNotes)
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
    const double pauseInc = 1.0 / ((p.speedMode == SpeedNotes ? kNotesPauseSeconds : kPauseSlewSeconds) * sr);
    const double pauseTarget = p.paused ? 0.0 : 1.0;

    // Grit mode: output rate and filter.
    gritHold = sr / kGritRate;
    const double gritCoef = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * kGritFilterHz / sr);

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

        // Reset: restart the tuned station from START. START is read at this moment.
        // A file named "...skipN" makes the next N resets be ignored while it plays; a subfolder moves on to its next file.
        if (resetFlag.exchange (false))
        {
            if (globalSlot < 0)
            {
                latchedStart = startNow;
            }
            else if (skipLeft[(size_t) globalSlot] > 0)
            {
                skipLeft[(size_t) globalSlot] -= 1;           // ignored
            }
            else
            {
                latchedStart = startNow;
                reselect (globalSlot, t);
                desiredStation = fileOfSlot (globalSlot);
                refClock[(size_t) desiredStation] = (double) clock;
                epochOf[(size_t) desiredStation] += 1;
                skipLeft[(size_t) globalSlot] = (int) t.skip[(size_t) desiredStation];
            }
        }

        // Tuning to a station: a subfolder may pick a new file (if enabled), skipN counts start, and with radio start off
        // the station restarts from START.
        if (globalSlot != lastDesiredSlot)
        {
            if (globalSlot >= 0)
            {
                bool restart = ! p.radioStart;

                if (p.reselectOnTune && t.slots[(size_t) globalSlot].numFiles > 1)
                {
                    reselect (globalSlot, t);
                    desiredStation = fileOfSlot (globalSlot);
                    restart = true;
                }

                if (restart)
                {
                    latchedStart = startNow;
                    refClock[(size_t) desiredStation] = (double) clock;
                    epochOf[(size_t) desiredStation] += 1;
                }

                skipLeft[(size_t) globalSlot] = (int) t.skip[(size_t) desiredStation];
            }
            lastDesiredSlot = globalSlot;
        }

        // Playing backwards needs a voice that reads the path backwards: switch once the speed has crossed zero.
        if (curDir > 0 && speedSm < -0.002)       curDir = -1;
        else if (curDir < 0 && speedSm > 0.002)   curDir = 1;

        Sig desired;
        if (desiredStation >= 0)
        {
            desired.station   = desiredStation;
            desired.startStep = latchedStart;
            desired.endStep   = appliedEnd;
            desired.loopMode  = juce::jlimit (0, 2, p.loopMode);
            desired.epoch     = epochOf[(size_t) desiredStation];
            desired.dir       = curDir;
        }

        handleTargetChange (desired, t, clock, xfInc, fastInc);

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

            const double e = speedSm * pauseMul;
            effPos[(size_t) i] = (float) std::max (0.0, e);
            effNeg[(size_t) i] = (float) std::max (0.0, -e);
            gainPos[(size_t) i] = std::min (1.0f, effPos[(size_t) i] * 20.0f);
            gainNeg[(size_t) i] = std::min (1.0f, effNeg[(size_t) i] * 20.0f);
            sumPos += (double) effPos[(size_t) i];
            sumNeg += (double) effNeg[(size_t) i];
            sumAll += e;
        }

        for (int i = 0; i < kNumVoices; ++i)
        {
            auto& v = voices[(size_t) i];
            if (v.state == Voice::State::Playing)
            {
                if (v.step * kMaxSpeed > kMaxStep * kMaxSpeed || v.step <= 0.0)
                    setIdle (v);
                else if (v.sig.dir > 0)
                    renderVoice (v, out + done, n, t, clock, effPos.data(), sumPos, gainPos.data(), cubic);
                else
                    renderVoice (v, out + done, n, t, clock, effNeg.data(), sumNeg, gainNeg.data(), cubic);
            }
        }

        // The tuned station moves by (speed x time); the difference to real time is what it has fallen behind the radio clock
        // (negative if it has moved ahead). Other stations are not affected.
        if (committedSig.station >= 0)
            refClock[(size_t) committedSig.station] += (double) n - sumAll;

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
    publishStatus (t, clock, desiredSlot, desiredStation, count);
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
