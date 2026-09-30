# dgmod

Oversampling and DSD modulation for everything Windows plays.

Normally Windows mixes all audio at one fixed rate and leaves the rest to the DAC's built-in filter. dgmod takes over
that step: applications play into a virtual audio device, dgmod captures the mix, upsamples it with a long,
high-precision filter and sends it to the DAC in exclusive mode — as high-rate PCM, or as DSD made by its own
delta-sigma modulator.

## Is it for your DAC?

dgmod only makes a difference when the DAC passes what it receives more or less straight to the conversion stage:

- **R2R (ladder) and NOS DACs.** They have little or no oversampling of their own, so the filter that shapes the sound
  is whatever the source provides. Feeding them high-rate PCM from dgmod's filter replaces a short or missing
  reconstruction filter with a long, precise one — this is where oversampling is most audible.
- **DACs with a DSD direct / bypass path.** DSD goes to the analog stage without being converted or remodulated, so
  dgmod's own modulator (and the filter in front of it) replaces the DAC's. Use *DSD (native)* through the ASIO
  driver, or *DSD (DoP)*.

On **ordinary delta-sigma DACs** (most ESS, AKM and Cirrus Logic based devices) expect a small effect at best. They
oversample every input again with their own filter and remodulate it — often DSD as well — so most of dgmod's
processing is redone by the chip. The true-peak limiter and plugins still work, but the filter and modulator
settings will hardly change the sound.

## Features

- **Output**: PCM up to 768 kHz (16/24/32-bit with TPDF dither), DSD64–256 over DoP, or native DSD64–1024 through
  the DAC's ASIO driver.
- **Filters**: quality presets up to 150 dB stop-band attenuation; linear, minimum or intermediate phase; sharp,
  Gaussian (ringing about ten times shorter), slow or NOS roll-off; optional apodizing; Kaiser or equiripple
  (Parks-McClellan) design.
- **True-peak limiter** that catches inter-sample overs of loud masters instead of letting the DAC clip them.
- **VST 3 and VST 2 plug-ins**: a chain of up to 16 effects (headphone correction, crossfeed, EQ, ...) on the
  source-rate signal, after the tone stages and before oversampling.
- **No clicks**: the clock drift between the virtual device and the DAC is tracked continuously, and underruns, gaps
  and restarts are faded instead of cut.

## Requirements

- Windows 11, x64.
- A virtual playback device, e.g. [VB-Audio Voicemeeter or VB-CABLE](https://vb-audio.com/).
- A DAC that supports exclusive mode — ideally an R2R / NOS DAC or one with a DSD direct path (see above). For
  native DSD: the manufacturer's ASIO driver with DSD support.

## Getting started

1. Enable the virtual device in *Sound settings → All sound devices*.
2. Start `dgmod.exe`. On the **Bridge** page choose the virtual device as *Source* and your DAC as *Output*, then
   press **Start**.
3. Turn on *Start with Windows* if you want it permanently.

> **Turn the volume down before the first DSD start.** A DAC that does not recognise DoP plays it as loud noise.

Exclusive mode skips any effects installed on the DAC. If you use Equalizer APO or similar, install it on the virtual
device instead — it will then run before oversampling.

## How it works

```
apps
  ↓
virtual device
  ↓  WASAPI loopback
buffer ──── level ────► drift controller
  ↓                           │
VST 3 / VST 2 plug-ins        │
  ↓                           │
resampler ◄──── ±ppm ─────────┘
  ↓
limiter
  ↓
PCM writer / delta-sigma modulator
  ↓  WASAPI exclusive or ASIO
DAC
```

When oversampling by two or more, the resampler works in two stages: the filter you choose runs at twice the source
rate with exact coefficients, then a short interpolator reaches the output rate and is trimmed by a few ppm to follow
the DAC's clock, so the buffer neither runs dry nor overflows. This costs a fraction of a single long filter at the
output rate. For DSD, a 7th-order 1-bit modulator reaches about 150 dB SNR in the audio band at DSD128 and 160 dB at
DSD256/512, where it also keeps its noise out of the band up to 34 kHz (DSD256) or 50 kHz (DSD512). Peaks beyond its
stable range are held by clipping its error feedback instead of resetting the loop (no click, only briefly more noise).

## Building

Visual Studio 2022 or newer with MSVC and the Windows SDK 10.0.26100, CMake 3.28+, Ninja.

```bat
build.cmd release
```

This produces `dgmod.exe` (settings) and `dgmod-bridge.exe` (background process) in `build\x64-release`.
`build.cmd debug` and `build.cmd arm64` are available as well.

## Command line

| Command | |
|---|---|
| `dgmod --status` | Bridge status and playback devices |
| `dgmod-bridge --stop` | Stop the bridge and restore the default device |
| `dgmod-bridge --asio-probe` | List ASIO drivers and their native DSD rates |
| `dgmod-bridge --asio-test "<driver>" [512]` | Check native DSD timing on a driver (bridge stopped) |
| `dgmod-bridge --scan-plugin "<path>"` | List the effects of a plug-in file |
| `dgmod-bridge --plugin-test "<path>" [--editor 5]` | Load one plug-in as the bridge does, process a test signal, check its state round trip and optionally show its editor |

Settings are stored in `HKCU\Software\dgmod` (the plug-in chain in `HKCU\Software\dgmod\Plugins`), the log in `%LOCALAPPDATA%\dgmod\bridge.log` (also shown on the
**Log** page).

## Limitations

- Latency is roughly 30–90 ms: fine for music and video, not for live monitoring or competitive games.
- Plug-ins run inside the bridge: a plug-in that corrupts memory without raising an exception, or crashes on one of
  its own threads, can still stop the bridge. 32-bit plug-ins are not supported.
- DSD modulation costs CPU: a few percent of one core at DSD128, about half a core at DSD1024.
