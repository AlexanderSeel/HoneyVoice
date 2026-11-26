#pragma once
#include <JuceHeader.h>
#include <cstdint>

struct WordEffectSettings {
    // Doubler
    bool doublerActive = false;
    float doublerAmount = 0.70f;
    float doublerDelayMs = 10.0f;
    float doublerDetuneCents = 16.0f;

    // Delay
    bool delayActive = false;
    float delayAmount = 0.55f;
    float delayTimeMs = 345.0f;
    float delayFeedback = 0.60f;
    bool pingPong = true;

    // Reverb
    bool reverbActive = false;
    float reverbAmount = 0.40f;
    float reverbDecaySec = 3.8f;
    float reverbWidth = 0.60f;

    // Preverb (reverse-reverb swell leading into word onset)
    bool preverbActive = false;
    float preverbAmount = 0.55f;
    float preverbLengthMs = 450.0f;
    float preverbDecaySec = 2.2f;

    // Distortion
    bool distortionActive = false;
    float distortionAmount = 0.75f;
    float drive = 0.50f;
    float distortionTone = 0.65f;

    // Harmony (up to 2 pitch-shifted stereo harmony voices)
    bool harmonyActive = false;
    float harmonyAmount = 0.55f;
    int harmonyInterval1 = 4;  // +4 semitones (major 3rd)
    int harmonyInterval2 = 7;  // +7 semitones (fifth), 0 = off
    float harmonyWidth = 0.75f;

    // Pitch
    bool pitchActive = false;
    float pitchAmount = 1.0f;
    int semitoneShift = -12;

    bool hasAnyEffect() const noexcept {
        return doublerActive || delayActive || reverbActive || preverbActive
            || distortionActive || harmonyActive || pitchActive;
    }
};

struct LyricWord {
    juce::String word;
    int64_t startSample = 0;
    int64_t endSample = 0;
    bool isSelected = false;
    bool isSearchMatch = false;
    WordEffectSettings fx;
};
