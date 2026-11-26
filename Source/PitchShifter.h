#pragma once
#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <vector>

/**
 * Zero-allocation real-time dual-grain Hann-windowed stereo pitch shifter.
 * Uses two 180-degree phase-offset read heads over a circular buffer so the
 * Hann crossfade sums to unity with minimal comb filtering across -12..+12 st.
 */
class StereoPitchShifter {
public:
    void prepare(double newSampleRate) {
        sampleRate = juce::jmax(8000.0, newSampleRate);
        windowSamples = juce::jlimit(256, 4096, (int)std::llround(sampleRate * 0.045));
        bufferSize = 16384;
        for (auto& ch : buffers)
            ch.assign((size_t)bufferSize, 0.0f);
        reset();
    }

    void reset() {
        for (auto& ch : buffers)
            std::fill(ch.begin(), ch.end(), 0.0f);
        writePos = 0;
        phaseA = 0.0;
        phaseB = 0.5;
    }

    void processSample(float inL, float inR, int semitones, float& outL, float& outR) {
        if (semitones == 0 || buffers[0].empty()) {
            pushOnly(inL, inR);
            outL = inL;
            outR = inR;
            return;
        }

        buffers[0][(size_t)writePos] = inL;
        buffers[1][(size_t)writePos] = inR;

        const double ratio = std::pow(2.0, (double)semitones / 12.0);
        const double phaseInc = (1.0 - ratio) / (double)windowSamples;

        auto wrapPhase = [](double p) {
            p -= std::floor(p);
            if (p < 0.0) p += 1.0;
            return p;
        };

        phaseA = wrapPhase(phaseA + phaseInc);
        phaseB = wrapPhase(phaseB + phaseInc);

        const double delayA = phaseA * (double)windowSamples;
        const double delayB = phaseB * (double)windowSamples;

        constexpr double kPi = 3.14159265358979323846;
        const float winA = (float)std::pow(std::sin(kPi * phaseA), 2.0);
        const float winB = (float)std::pow(std::sin(kPi * phaseB), 2.0);

        outL = readInterp(0, delayA) * winA + readInterp(0, delayB) * winB;
        outR = readInterp(1, delayA) * winA + readInterp(1, delayB) * winB;

        writePos = (writePos + 1) % bufferSize;
    }

    void pushOnly(float inL, float inR) {
        if (buffers[0].empty()) return;
        buffers[0][(size_t)writePos] = inL;
        buffers[1][(size_t)writePos] = inR;
        writePos = (writePos + 1) % bufferSize;
    }

private:
    float readInterp(int ch, double delaySamples) const {
        double idx = (double)writePos - delaySamples;
        while (idx < 0.0) idx += (double)bufferSize;
        while (idx >= (double)bufferSize) idx -= (double)bufferSize;

        const int i0 = (int)idx;
        const int i1 = (i0 + 1) % bufferSize;
        const float frac = (float)(idx - (double)i0);
        const auto& buf = buffers[(size_t)ch];
        return buf[(size_t)i0] + frac * (buf[(size_t)i1] - buf[(size_t)i0]);
    }

    double sampleRate = 44100.0;
    int windowSamples = 2048;
    int bufferSize = 16384;
    std::array<std::vector<float>, 2> buffers;
    int writePos = 0;
    double phaseA = 0.0;
    double phaseB = 0.5;
};
