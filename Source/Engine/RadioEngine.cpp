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

double RadioEngine::expectedFrame (double fileRate, int64_t numFrames, int64_t clock, double sessionRate) noexcept
{
    const double frames = (double) clock * (fileRate / sessionRate);
    return std::fmod (frames, (double) numFrames);
}

void RadioEngine::setIdle (Voice& v) noexcept
{
    v.state       = Voice::State::Idle;
    v.station     = -1;
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
    committedStation = pendingStation = -1;
    badStation.fill (false);
}

void RadioEngine::requestVoice (Voice& v, int station, const Table& t, int64_t clockStart) noexcept
{
    v.gen += 1;
    v.station = station;

    const auto& info = t.info[(size_t) station];
    v.fileRate  = info.sampleRate;
    v.numFrames = info.numFrames;
    v.step      = info.sampleRate / sessionRate;
    v.phase     = 0.0;
    v.consumed  = 0;
    v.state     = Voice::State::Loading;

    const double expected = expectedFrame (info.sampleRate, info.numFrames, clockStart, sessionRate);
    v.seekStart = (int64_t) std::floor (expected);
    if (v.seekStart >= info.numFrames)
        v.seekStart = 0;

    v.reqStation.store (station, std::memory_order_relaxed);
    v.reqStart.store (v.seekStart, std::memory_order_relaxed);
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

void RadioEngine::handleTargetChange (int desired, const Table& t, int64_t clockStart, float xfInc) noexcept
{
    juce::ignoreUnused (xfInc);

    if (desired >= 0 && badStation[(size_t) desired])
        desired = committedStation;                      // never retry a file that failed to open

    if (desired == committedStation)
    {
        if (pending >= 0)                                // tuned back to what is already playing
        {
            setIdle (voices[(size_t) pending]);
            pending = -1;
            pendingStation = -1;
        }
        return;
    }

    if (desired == pendingStation)
        return;

    if (desired < 0)                                     // folder is empty: fade out whatever is playing
    {
        if (pending >= 0)
        {
            setIdle (voices[(size_t) pending]);
            pending = -1;
            pendingStation = -1;
        }
        if (current >= 0)
        {
            voices[(size_t) current].levelTarget = 0.0f;
            voices[(size_t) current].levelInc    = xfInc;
        }
        current = -1;
        committedStation = -1;
        return;
    }

    if (pending >= 0)                                    // retarget the voice that is still loading
    {
        requestVoice (voices[(size_t) pending], desired, t, clockStart);
        pendingStation = desired;
        return;
    }

    const int vi = allocVoice();
    if (vi < 0)
        return;

    auto& v = voices[(size_t) vi];
    v.level = 0.0f;
    v.levelTarget = 1.0f;
    requestVoice (v, desired, t, clockStart);
    pending = vi;
    pendingStation = desired;
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

void RadioEngine::tryAlign (int voiceIndex, const Table& t, int64_t clockStart, float xfInc) noexcept
{
    auto& v = voices[(size_t) voiceIndex];

    if (v.failedGen.load (std::memory_order_acquire) == v.gen)
    {
        if (v.station >= 0 && v.station < kMaxStationsPerBank)
            badStation[(size_t) v.station] = true;

        if (voiceIndex == pending)
        {
            pending = -1;
            pendingStation = -1;
        }
        if (voiceIndex == current)
        {
            current = -1;
            committedStation = -1;
        }
        setIdle (v);
        return;
    }

    if (v.readyGen.load (std::memory_order_acquire) != v.gen)
        return;

    // The voice was seeked to where the radio clock was when it was requested. Skip ahead by
    // however much the clock has moved since, so it joins in exactly where the station "is".
    const double expected = expectedFrame (v.fileRate, v.numFrames, clockStart, sessionRate);
    double d = std::fmod (expected - (double) v.seekStart, (double) v.numFrames);
    if (d < 0.0)
        d += (double) v.numFrames;
    if ((double) v.numFrames - d < 64.0)
        d = 0.0;                                         // the voice is a hair ahead of the clock

    const int64_t skip = (int64_t) d;

    if (skip + 2 > kFifoFrames - 2048)                   // too far behind to catch up from the buffer
    {
        requestVoice (v, v.station, t, clockStart);
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
        v.levelInc = xfInc;

        if (current >= 0)
        {
            auto& old = voices[(size_t) current];
            old.levelTarget = 0.0f;
            old.levelInc = xfInc;
            if (old.state == Voice::State::Loading)
                setIdle (old);
        }

        current = pending;
        committedStation = pendingStation;
        pending = -1;
        pendingStation = -1;
    }
}

void RadioEngine::renderVoice (Voice& v, float* out, int n, const Table& t, int64_t clockStart) noexcept
{
    const double adv = v.phase + v.step * (double) n;
    const int k = (int) adv;

    if ((int) scratch.size() < k + 2 || v.fifo.getNumReady() < k)
    {
        underruns.fetch_add (1);

        if (v.levelTarget > 0.0f && v.station >= 0)
        {
            requestVoice (v, v.station, t, clockStart);     // re-sync with the clock and carry on
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

    for (int i = 0; i < n; ++i)
    {
        const double p = v.phase + v.step * (double) i;
        const int idx = (int) p;
        const float frac = (float) (p - (double) idx);
        const float sample = s[idx] + (s[idx + 1] - s[idx]) * frac;

        float g = level;
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
    int desired = -1;
    if (count > 0)
        desired = juce::jlimit (0, count - 1, (int) std::floor (juce::jlimit (0.0f, 1.0f, p.station) * (float) count));

    const double xfSamples = std::max (1.0, (double) p.crossfadeMs * 0.001 * sessionRate);
    const float xfInc = (float) (1.0 / xfSamples);

    int done = 0;
    int64_t clock = clockSamples.load (std::memory_order_relaxed);

    while (done < numSamples)
    {
        const int n = std::min (kRenderChunk, numSamples - done);

        handleTargetChange (desired, t, clock, xfInc);

        for (int i = 0; i < kNumVoices; ++i)
            if (voices[(size_t) i].state == Voice::State::Loading)
                tryAlign (i, t, clock, xfInc);

        for (int i = 0; i < kNumVoices; ++i)
        {
            auto& v = voices[(size_t) i];
            if (v.state == Voice::State::Playing)
            {
                if (v.step > kMaxStep || v.step <= 0.0)
                    setIdle (v);
                else
                    renderVoice (v, out + done, n, t, clock);
            }
        }

        clock += n;
        done += n;
    }

    clockSamples.store (clock, std::memory_order_relaxed);
    uiClockSeconds.store ((double) clock / sessionRate, std::memory_order_relaxed);
    uiStation.store (desired);
    uiCount.store (count);
    readingTable.store (-1);
}

//==============================================================================
// Stream thread

bool RadioEngine::serviceVoice (Voice& v)
{
    bool work = false;
    const uint32_t gen = v.reqGen.load (std::memory_order_acquire);

    if (gen != v.handledGen)
    {
        const int     station = v.reqStation.load (std::memory_order_relaxed);
        const int64_t start   = v.reqStart.load (std::memory_order_relaxed);
        const uint32_t tableId = v.reqTable.load (std::memory_order_relaxed);

        if (v.reqGen.load (std::memory_order_acquire) != gen)
            return true;                                 // request changed while reading it; look again

        v.handledGen = gen;
        v.reader.reset();
        v.streaming = false;
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
                    v.fileLen   = (int64_t) v.reader->lengthInSamples;
                    v.readPos   = ((start % v.fileLen) + v.fileLen) % v.fileLen;
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
            const int64_t toEnd = v.fileLen - v.readPos;
            const int num = (int) std::min<int64_t> (kChunkFrames, toEnd);

            if (v.fifo.getFreeSpace() < num)
                break;

            float* dest[1] = { streamTmp.data() };
            if (! v.reader->read (dest, 1, v.readPos, num))
                std::fill_n (streamTmp.data(), num, 0.0f);

            v.readPos += num;
            if (v.readPos >= v.fileLen)
                v.readPos = 0;                           // loop: the stream wraps like the file does

            int s1, n1, s2, n2;
            v.fifo.prepareToWrite (num, s1, n1, s2, n2);
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
