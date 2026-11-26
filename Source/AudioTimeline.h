#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <vector>

/**
 * Thread-safe audio timeline storage for capturing DAW vocal input,
 * loading vocal audio files for standalone/offline auditioning,
 * generating waveform min/max peaks, and supplying audio slices to
 * the transcriber and preverb renderer.
 */
class AudioTimeline {
public:
    struct PeakPair {
        float minVal = 0.0f;
        float maxVal = 0.0f;
    };

    AudioTimeline();

    /** Pre-allocates internal storage for real-time capture (non-audio thread). */
    void prepare(double newSampleRate);

    /** Captures incoming dry audio from processBlock without allocating. */
    void captureBlock(const juce::AudioBuffer<float>& buffer,
                      int64_t timelineStartSample,
                      bool hasAudibleSignal);

    /** Reads captured/loaded audio into buffer for standalone preview playback. */
    bool readPlaybackBlock(int64_t timelineStartSample,
                           juce::AudioBuffer<float>& destBuffer,
                           bool loopPlayback) const;

    /** Loads an audio file (WAV/AIFF/FLAC/OGG/MP3) into the timeline at sample 0. */
    bool loadFromFile(const juce::File& file, juce::String& outError);

    /** Clears captured/loaded audio and resets capture origin. */
    void clear();

    void setLiveCaptureEnabled(bool enabled) noexcept { liveCaptureEnabled.store(enabled, std::memory_order_relaxed); }
    bool isLiveCaptureEnabled() const noexcept { return liveCaptureEnabled.load(std::memory_order_relaxed); }

    bool hasAudio() const noexcept;
    bool isFileLoaded() const noexcept { return fileLoaded.load(std::memory_order_relaxed); }
    double getSampleRate() const noexcept { return sampleRate.load(std::memory_order_relaxed); }
    int64_t getOriginSample() const noexcept { return originSample.load(std::memory_order_relaxed); }
    int64_t getValidStartSample() const noexcept;
    int64_t getValidEndSample() const noexcept;
    int64_t getValidLengthSamples() const noexcept;
    uint64_t getVersion() const noexcept { return version.load(std::memory_order_relaxed); }
    juce::String getSourceLabel() const;

    /** Computes downsampled waveform peaks across [viewStartSample, viewEndSample). */
    void computePeaks(int numBuckets,
                      int64_t viewStartSample,
                      int64_t viewEndSample,
                      std::vector<PeakPair>& outPeaks) const;

    /** Copies mono audio in [startSample, startSample + numSamples) at current sampleRate. */
    std::vector<float> copyMonoRange(int64_t startSample, int64_t numSamples) const;

    /** Copies the full valid mono audio resampled to 16 kHz for speech recognition. */
    bool extractMono16k(std::vector<float>& outSamples16k,
                        int64_t& outTimelineStartSample,
                        double& outSourceSampleRate) const;

    /** Copies the full valid mono audio at source sample rate for acoustic segmentation. */
    bool extractFullValidMono(std::vector<float>& outSamples,
                              int64_t& outTimelineStartSample,
                              double& outSourceSampleRate) const;

private:
    static constexpr double kDefaultCaptureSeconds = 120.0;

    mutable juce::SpinLock lock;
    juce::AudioBuffer<float> storage; // 2 channels
    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<int64_t> originSample { 0 };
    std::atomic<int64_t> validStartOffset { 0 };
    std::atomic<int64_t> validEndOffset { 0 };
    std::atomic<bool> hasOrigin { false };
    std::atomic<bool> fileLoaded { false };
    std::atomic<bool> liveCaptureEnabled { true };
    std::atomic<uint64_t> version { 0 };
    juce::String sourceLabel { "DAW Live Capture" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioTimeline)
};
