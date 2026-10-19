// Radio Music (unofficial) - playback path of one station (milestone 2).
// Clean-room implementation from the documented behaviour of Music Thing
// Modular's Radio Music mk2 (Tom Whitwell with Chris Johnson).
//
// A station plays along a "path" through its file. The path is defined by a loop window
// (Start / End), the loop mode, and the time since the path began (tau, in file frames).
// Everything here is pure arithmetic, shared by the audio thread, the stream thread, the
// user interface and the tests, so they cannot disagree about where a station "is".
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rm
{
constexpr int kStartSteps       = 1024;   // documented: START is stored as 1024 steps across the file
constexpr int kStartDivider     = 2;      // documented default of startCVDivider (settings.txt arrives in a later milestone)
constexpr int kMinWindowFrames  = 32;     // shortest loop window we allow

enum LoopMode { LoopOff = 0, LoopForward = 1, LoopPingPong = 2 };

// A loop window [lo, hi) in file frames, whether it is played backwards, and the loop mode.
struct PathCfg
{
    int64_t lo   = 0;
    int64_t hi   = 0;
    bool    rev  = false;
    int     mode = LoopForward;

    int64_t length() const noexcept { return hi - lo; }
};

struct PathPoint
{
    int64_t pos   = 0;       // file frame
    bool    back  = false;   // currently moving backwards
    bool    ended = false;   // loop mode Off and the end of the window has been reached
};

// Converts a 0..1 knob position to a step 0..1024, rounded down to a multiple of divider.
inline int quantiseStep (float norm, int divider) noexcept
{
    const float n = std::min (1.0f, std::max (0.0f, norm));
    int s = (int) std::floor (n * (float) kStartSteps + 0.0001f);
    s = std::min (s, kStartSteps);
    if (divider > 1)
        s = (s / divider) * divider;
    return s;
}

// Documented rules: if START is fully left and END is at either edge, or START and END match,
// the whole sample plays. If START is to the right of END, playback is reversed.
inline PathCfg makeCfg (int startStep, int endStep, int64_t numFrames, int mode) noexcept
{
    PathCfg c;
    c.mode = mode;

    const bool whole = (startStep == endStep) || (startStep == 0 && (endStep == 0 || endStep == kStartSteps));

    if (whole)
    {
        c.lo = 0;
        c.hi = numFrames;
        c.rev = false;
    }
    else if (startStep < endStep)
    {
        c.lo = numFrames * startStep / kStartSteps;
        c.hi = numFrames * endStep / kStartSteps;
        c.rev = false;
    }
    else
    {
        c.lo = numFrames * endStep / kStartSteps;
        c.hi = numFrames * startStep / kStartSteps;
        c.rev = true;
    }

    const int64_t minLen = std::min<int64_t> (kMinWindowFrames, numFrames);
    if (c.hi - c.lo < minLen)
    {
        c.hi = std::min<int64_t> (numFrames, c.lo + minLen);
        c.lo = std::max<int64_t> (0, c.hi - minLen);
    }

    return c;
}

inline int64_t floorMod (int64_t a, int64_t m) noexcept
{
    const int64_t r = a % m;
    return r < 0 ? r + m : r;
}

// Where the path is after tau frames. tau may be negative (the station was played backwards past its start):
// looping paths simply wrap round, a path with loop mode Off stays at the start of the window.
inline PathPoint pathAt (const PathCfg& c, int64_t tau) noexcept
{
    const int64_t L = std::max<int64_t> (1, c.hi - c.lo);

    PathPoint p;

    if (c.mode == LoopOff)
    {
        if (tau < 0)
            tau = 0;
        if (tau >= L)
        {
            p.ended = true;
            tau = L - 1;
        }
        p.back = c.rev;
        p.pos  = c.rev ? c.hi - 1 - tau : c.lo + tau;
    }
    else if (c.mode == LoopPingPong)
    {
        const int64_t u = floorMod (tau, 2 * L);
        const bool second = u >= L;
        const int64_t w = second ? u - L : u;
        p.back = (second != c.rev);
        p.pos  = p.back ? c.hi - 1 - w : c.lo + w;
    }
    else
    {
        const int64_t w = floorMod (tau, L);
        p.back = c.rev;
        p.pos  = c.rev ? c.hi - 1 - w : c.lo + w;
    }

    return p;
}
} // namespace rm
