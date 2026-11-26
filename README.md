# HoneyVoice VST3

Lyric-by-lyric vocal creative FX plugin inspired by iZotope Nectar Moments, built with JUCE 8/9 and CMake.

## Features
- **Real Audio Timeline & Live Waveform**: Captures live vocal audio from your DAW timeline (`AudioTimeline`) or loads/drops `.wav`, `.aiff`, `.flac`, `.ogg`, and `.mp3` files directly onto the waveform with standalone Play/Stop/Loop auditioning and draggable word timing edges.
- **Real Speech-to-Text & Vocal Alignment (`VocalTranscriber`)**:
  1. Embedded `whisper.cpp` (optional via `-DHONEYVOICE_WITH_WHISPER=ON`) or external `whisper-cli` with `.bin` GGML models (`%APPDATA%/HoneyVoice/models`).
  2. Native Windows Speech Recognition (`SAPI 5.4`) offline dictation with word-level timestamps.
  3. Acoustic RMS/onset vocal segmenter + manual lyric-to-audio aligner fallback for singing vocals.
- **7 Per-Word Creative FX**:
  - **Doubler**: Stereo LFO-modulated `Lagrange3rd` detuned delay
  - **Delay**: Ping-pong / stereo delay with parameter latching and smooth time interpolation
  - **Reverb**: Stereo hall/room reverb with latched tail decay
  - **Preverb**: Reverse-reverb swell pre-rendered prior to word onset
  - **Distortion**: Soft asymmetric saturation with drive, tone filter, and level compensation
  - **Harmony**: Dual-voice stereo pitch-shifted harmonies
  - **Pitch**: Real-time Hann-windowed dual-grain pitch shifter (`-12` to `+12` st)
- **Full DAW State Persistence**: Saves and restores all transcribed words, timestamps, selections, and per-word FX settings.

## Local Build & Tests
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DHONEYVOICE_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Quick Build via GitHub Actions
1. Push to `main` (preserving `.github/workflows/build.yml`).
2. Open the **Actions** tab on GitHub and wait for **Build HoneyVoice VST3** to complete.
3. Download **`HoneyVoice-VST3-windows-latest`** or **`HoneyVoice-VST3-macos-latest`** under **Artifacts**.
4. Copy `HoneyVoice.vst3` to your system VST3 folder:
   - **Windows:** `C:\Program Files\Common Files\VST3`
   - **macOS:** `/Library/Audio/Plug-Ins/VST3`
