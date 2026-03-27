#pragma once

// Forward declarations (cfg_int/cfg_bool are defined via stdafx.h / foobar2000 SDK)
extern cfg_int  cfg_adlib_samplerate;   // index into sample rate table; default = 7 (49716 Hz)
extern cfg_int  cfg_adlib_core;         // 0=Harekiet's  1=Ken Silverman's  2=Jarek Burczynski's  3=Nuked OPL3
extern cfg_int  cfg_adlib_equalizer;    // 0=ESS FM  1=None  (only relevant when surround is on)
extern cfg_bool cfg_adlib_surround;     // true=enable CSurroundopl stereo harmonic effect

constexpr int  kAdLibDefaultSampleRateIdx = 7;     // 49716 Hz
constexpr int  kAdLibDefaultCoreIdx       = 0;     // Harekiet's
constexpr int  kAdLibDefaultEqualizerIdx  = 0;     // ESS FM
constexpr bool kAdLibDefaultSurround      = true;  // surround on by default

// Returns the configured sample rate in Hz.
// Safe to call from any thread after foobar2000 has initialised cfg vars.
unsigned GetAdLibSampleRate();
