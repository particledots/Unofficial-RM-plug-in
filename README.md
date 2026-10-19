# Radio Music (unofficial)

An unofficial AU / VST3 sample-player plug-in for macOS, inspired by **Radio Music mk2**, the Eurorack
module designed by **Tom Whitwell of [Music Thing Modular](https://www.musicthing.co.uk/)** together with
**Chris Johnson of Plus Minus Audio**.

Radio Music behaves like a radio: each audio file in a folder is a "station", and files keep playing
in the background when you tune away, so tuning back finds them further along.

> This project is **not affiliated with, endorsed by or supported by** Music Thing Modular or Plus Minus Audio.
> Please support the real thing: <https://www.musicthing.co.uk/Radio-Music/>

## Status: milestone 3

Works now:
- Choose a folder of WAV / AIFF files. Each file is a station (sorted by file name).
- A **Station** parameter (automatable) selects the file, with a crossfade (adjustable, 1 ms to 60 s).
- A **radio clock**: every station has its own playhead derived from one clock, so a station you tuned
  away from has moved on when you tune back. Free-running by default, or **host sync** to follow the DAW position.
- **Start / End** loop window (START to the right of END plays in reverse), **loop mode** (off / loop / ping-pong),
  a **Reset** button (and automatable Reset parameter), **Pause** and **radio start** on/off.
- **Speed**: tape mode (-400% to +400%, through zero into reverse) and notes mode (-48 to +24 semitones, quantised to
  semitones by default). Speed changes only the tuned station; other stations keep advancing at normal speed.
  The '90s mode is not available yet (it plays at original speed).
- **Cubic interpolation** for clean pitch changes and sample-rate conversion, or **grit mode**
  (8-bit, about 20 kHz, zero-order hold, with a post-DAC filter).
- Files at any supported sample rate are played at the right speed in the session rate.
- Long files are streamed from disk, never loaded into memory.
- A tuning-dial user interface with hover help for every control.

Not yet (planned milestones): '90s mode, 16 banks / subfolders / `skipN` / `settings.txt`, soft and radio tuner modes,
MIDI control, remembering the folder across sandboxed sessions.

## How it was made (clean room)

Behaviour was implemented from the **published documentation** only:
- <https://www.musicthing.co.uk/Radio-Music/>
- <https://www.musicthing.co.uk/Radio_Music_Reference/>

No Radio Music firmware source code and no VCV Rack port source code was copied or consulted. Where the documentation
is silent, this plug-in makes its own choices (listed in the project notes) which may differ from the hardware.

## Licence

Copyright (C) 2026 particledots.

Released under the **GNU Affero General Public License v3.0** (see `LICENSE`). It is built with the
[JUCE](https://juce.com/) framework (version 9.0.4), which is dual-licensed under the AGPLv3 and a commercial JUCE
licence; this project uses JUCE under the AGPLv3. JUCE bundles the Steinberg VST3 SDK 3.8.0 (MIT) and Apple's
AudioUnitSDK (Apache-2.0) according to its own bill of materials (`JUCE.spdx.json`).

If you distribute a modified version of this plug-in, the AGPLv3 requires you to make its complete source available
under the same licence.

## Building

Builds happen on GitHub Actions (`.github/workflows/build.yml`) on every push to `main`, or by hand from the
**Actions** tab. The result is a downloadable `RadioMusicUnofficial-macOS` artifact: a universal (arm64 + x86_64)
AU and VST3 for macOS 12 or newer. See `INSTALL.txt` for installation.
