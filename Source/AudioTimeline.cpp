#include "AudioTimeline.h"
#include <algorithm>
#include <cmath>

AudioTimeline::AudioTimeline() {
    prepare(44100.0);
}

void AudioTimeline::prepare(double newSampleRate) {
    const double safeRate = juce::jmax(8000.0, newSampleRate);
    const int64_t targetCapacity = (int64_t)std::llround(safeRate * kDefaultCaptureSeconds);

    const juce::SpinLock::ScopedLockType guard(lock);
    const double prevRate = sampleRate.load(std::memory_order_relaxed);
    if (storage.getNumSamples() < targetCapacity || std::abs(prevRate - safeRate) > 1.0) {
        if (!fileLoaded.load(std::memory_order_relaxed)) {
            storage.setSize(2, (int)targetCapacity, false, true, true);
            originSample.store(0, std::memory_order_relaxed);
            validStartOffset.store(0, std::memory_order_relaxed);
            validEndOffset.store(0, std::memory_order_relaxed);
            hasOrigin.store(false, std::memory_order_relaxed);
            sampleRate.store(safeRate, std::memory_order_relaxed);
            version.fetch_add(1, std::memory_order_relaxed);
        } else if (std::abs(prevRate - safeRate) > 1.0 && validEndOffset.load(std::memory_order_relaxed) > 0) {
            // Resample loaded file to match host sample rate
            const int oldLen = (int)validEndOffset.load(std::memory_order_relaxed);
            const double ratio = prevRate / safeRate;
            const int newLen = juce::jmax(1, (int)std::ceil((double)oldLen / ratio));
            juce::AudioBuffer<float> resampled(2, newLen);
            for (int ch = 0; ch < 2; ++ch) {
                juce::LagrangeInterpolator interp;
                interp.reset();
                interp.process(ratio, storage.getReadPointer(ch), resampled.getWritePointer(ch), newLen);
            }
            storage = std::move(resampled);
            validStartOffset.store(0, std::memory_order_relaxed);
            validEndOffset.store(newLen, std::memory_order_relaxed);
            sampleRate.store(safeRate, std::memory_order_relaxed);
            version.fetch_add(1, std::memory_order_relaxed);
        } else {
            sampleRate.store(safeRate, std::memory_order_relaxed);
        }
    }
}

void AudioTimeline::captureBlock(const juce::AudioBuffer<float>& buffer,
                                 int64_t timelineStartSample,
                                 bool hasAudibleSignal) {
    if (!liveCaptureEnabled.load(std::memory_order_relaxed) || fileLoaded.load(std::memory_order_relaxed))
        return;

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0 || timelineStartSample < 0)
        return;

    const juce::SpinLock::ScopedTryLockType guard(lock);
    if (!guard.isLocked())
        return;

    const int capacity = storage.getNumSamples();
    if (capacity <= 0)
        return;

    const double sr = sampleRate.load(std::memory_order_relaxed);

    if (!hasOrigin.load(std::memory_order_relaxed)) {
        if (!hasAudibleSignal)
            return;

        // Anchor to 0 if near the start of the song so rewinding to bar 1 works seamlessly,
        // otherwise leave 4s pre-roll before the first vocal entry.
        const int64_t nearZeroThreshold = (int64_t)std::llround(sr * 30.0);
        const int64_t preRoll = (int64_t)std::llround(sr * 4.0);
        const int64_t newOrigin = (timelineStartSample < nearZeroThreshold)
                                    ? 0
                                    : juce::jmax<int64_t>(0, timelineStartSample - preRoll);
        originSample.store(newOrigin, std::memory_order_relaxed);
        validStartOffset.store(timelineStartSample - newOrigin, std::memory_order_relaxed);
        validEndOffset.store(timelineStartSample - newOrigin, std::memory_order_relaxed);
        hasOrigin.store(true, std::memory_order_relaxed);
        sourceLabel = "DAW Live Capture";
    }

    int64_t orig = originSample.load(std::memory_order_relaxed);
    int64_t relStart = timelineStartSample - orig;
    int64_t relEnd = relStart + numSamples;

    // If the playhead jumped completely outside the current capture window, re-anchor on audible audio
    if (relStart < 0 || relEnd > capacity) {
        if (!hasAudibleSignal)
            return;

        storage.clear();
        const int64_t nearZeroThreshold = (int64_t)std::llround(sr * 30.0);
        const int64_t preRoll = (int64_t)std::llround(sr * 4.0);
        orig = (timelineStartSample < nearZeroThreshold)
                 ? 0
                 : juce::jmax<int64_t>(0, timelineStartSample - preRoll);
        originSample.store(orig, std::memory_order_relaxed);
        relStart = timelineStartSample - orig;
        relEnd = relStart + numSamples;
        if (relStart < 0 || relEnd > capacity)
            return;
        validStartOffset.store(relStart, std::memory_order_relaxed);
        validEndOffset.store(relStart, std::memory_order_relaxed);
    }

    const int writeOffset = (int)relStart;
    const float* srcL = buffer.getReadPointer(0);
    const float* srcR = buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : srcL;

    std::copy_n(srcL, numSamples, storage.getWritePointer(0, writeOffset));
    std::copy_n(srcR, numSamples, storage.getWritePointer(1, writeOffset));

    const int64_t curStart = validStartOffset.load(std::memory_order_relaxed);
    const int64_t curEnd = validEndOffset.load(std::memory_order_relaxed);
    if (curEnd <= curStart) {
        validStartOffset.store(relStart, std::memory_order_relaxed);
        validEndOffset.store(relEnd, std::memory_order_relaxed);
    } else {
        validStartOffset.store(juce::jmin(curStart, relStart), std::memory_order_relaxed);
        validEndOffset.store(juce::jmax(curEnd, relEnd), std::memory_order_relaxed);
    }
    version.fetch_add(1, std::memory_order_relaxed);
}

bool AudioTimeline::readPlaybackBlock(int64_t timelineStartSample,
                                      juce::AudioBuffer<float>& destBuffer,
                                      bool loopPlayback) const {
    const int numSamples = destBuffer.getNumSamples();
    if (numSamples <= 0)
        return false;

    const juce::SpinLock::ScopedTryLockType guard(lock);
    if (!guard.isLocked())
        return false;

    if (!hasOrigin.load(std::memory_order_relaxed))
        return false;

    const int64_t orig = originSample.load(std::memory_order_relaxed);
    const int64_t vStart = validStartOffset.load(std::memory_order_relaxed);
    const int64_t vEnd = validEndOffset.load(std::memory_order_relaxed);
    const int64_t vLen = vEnd - vStart;
    if (vLen <= 0)
        return false;

    const int outCh = destBuffer.getNumChannels();
    for (int i = 0; i < numSamples; ++i) {
        int64_t rel = (timelineStartSample + i) - orig;
        if (loopPlayback) {
            if (rel < vStart || rel >= vEnd) {
                int64_t wrapped = (rel - vStart) % vLen;
                if (wrapped < 0) wrapped += vLen;
                rel = vStart + wrapped;
            }
        }
        if (rel >= vStart && rel < vEnd && rel < storage.getNumSamples()) {
            const float l = storage.getSample(0, (int)rel);
            const float r = storage.getSample(1, (int)rel);
            if (outCh >= 1) destBuffer.setSample(0, i, l);
            if (outCh >= 2) destBuffer.setSample(1, i, r);
        }
    }
    return true;
}

bool AudioTimeline::loadFromFile(const juce::File& file, juce::String& outError) {
    if (!file.existsAsFile()) {
        outError = "File does not exist.";
        return false;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (!reader) {
        outError = "Unsupported audio format: " + file.getFileExtension();
        return false;
    }

    const int64_t maxFileSamples = (int64_t)std::llround(reader->sampleRate * 600.0);
    const int sourceSamples = (int)juce::jmin<int64_t>(reader->lengthInSamples, maxFileSamples);
    if (sourceSamples <= 0) {
        outError = "Audio file is empty.";
        return false;
    }

    juce::AudioBuffer<float> raw(2, sourceSamples);
    raw.clear();
    reader->read(&raw, 0, sourceSamples, 0, true, reader->numChannels > 1);
    if (reader->numChannels == 1)
        raw.copyFrom(1, 0, raw, 0, 0, sourceSamples);

    const double targetRate = sampleRate.load(std::memory_order_relaxed);
    juce::AudioBuffer<float> finalBuf;

    if (std::abs(reader->sampleRate - targetRate) > 1.0 && targetRate > 0.0) {
        const double ratio = reader->sampleRate / targetRate;
        const int targetSamples = juce::jmax(1, (int)std::ceil((double)sourceSamples / ratio));
        finalBuf.setSize(2, targetSamples);
        for (int ch = 0; ch < 2; ++ch) {
            juce::LagrangeInterpolator interp;
            interp.reset();
            interp.process(ratio, raw.getReadPointer(ch), finalBuf.getWritePointer(ch), targetSamples);
        }
    } else {
        finalBuf = std::move(raw);
    }

    {
        const juce::SpinLock::ScopedLockType guard(lock);
        const int64_t minCap = (int64_t)std::llround(targetRate * kDefaultCaptureSeconds);
        const int allocLen = (int)juce::jmax<int64_t>(minCap, finalBuf.getNumSamples());
        storage.setSize(2, allocLen, false, true, false);
        storage.clear();
        for (int ch = 0; ch < 2; ++ch)
            storage.copyFrom(ch, 0, finalBuf, ch, 0, finalBuf.getNumSamples());

        originSample.store(0, std::memory_order_relaxed);
        validStartOffset.store(0, std::memory_order_relaxed);
        validEndOffset.store(finalBuf.getNumSamples(), std::memory_order_relaxed);
        hasOrigin.store(true, std::memory_order_relaxed);
        fileLoaded.store(true, std::memory_order_relaxed);
        liveCaptureEnabled.store(false, std::memory_order_relaxed);
        sourceLabel = file.getFileName();
        version.fetch_add(1, std::memory_order_relaxed);
    }

    return true;
}

void AudioTimeline::clear() {
    const juce::SpinLock::ScopedLockType guard(lock);
    const double sr = sampleRate.load(std::memory_order_relaxed);
    const int targetCap = (int)std::llround(sr * kDefaultCaptureSeconds);
    storage.setSize(2, targetCap, false, true, true);
    storage.clear();
    originSample.store(0, std::memory_order_relaxed);
    validStartOffset.store(0, std::memory_order_relaxed);
    validEndOffset.store(0, std::memory_order_relaxed);
    hasOrigin.store(false, std::memory_order_relaxed);
    fileLoaded.store(false, std::memory_order_relaxed);
    liveCaptureEnabled.store(true, std::memory_order_relaxed);
    sourceLabel = "DAW Live Capture";
    version.fetch_add(1, std::memory_order_relaxed);
}

bool AudioTimeline::hasAudio() const noexcept {
    return hasOrigin.load(std::memory_order_relaxed)
        && validEndOffset.load(std::memory_order_relaxed) > validStartOffset.load(std::memory_order_relaxed);
}

int64_t AudioTimeline::getValidStartSample() const noexcept {
    return originSample.load(std::memory_order_relaxed) + validStartOffset.load(std::memory_order_relaxed);
}

int64_t AudioTimeline::getValidEndSample() const noexcept {
    return originSample.load(std::memory_order_relaxed) + validEndOffset.load(std::memory_order_relaxed);
}

int64_t AudioTimeline::getValidLengthSamples() const noexcept {
    return juce::jmax<int64_t>(0, validEndOffset.load(std::memory_order_relaxed) - validStartOffset.load(std::memory_order_relaxed));
}

juce::String AudioTimeline::getSourceLabel() const {
    const juce::SpinLock::ScopedLockType guard(lock);
    return sourceLabel;
}

void AudioTimeline::computePeaks(int numBuckets,
                                 int64_t viewStartSample,
                                 int64_t viewEndSample,
                                 std::vector<PeakPair>& outPeaks) const {
    outPeaks.assign((size_t)juce::jmax(0, numBuckets), PeakPair{});
    if (numBuckets <= 0 || viewEndSample <= viewStartSample)
        return;

    const juce::SpinLock::ScopedLockType guard(lock);
    if (!hasOrigin.load(std::memory_order_relaxed))
        return;

    const int64_t orig = originSample.load(std::memory_order_relaxed);
    const int64_t vStart = validStartOffset.load(std::memory_order_relaxed);
    const int64_t vEnd = validEndOffset.load(std::memory_order_relaxed);
    if (vEnd <= vStart)
        return;

    const int bufLen = storage.getNumSamples();
    const float* ch0 = storage.getReadPointer(0);
    const float* ch1 = storage.getReadPointer(1);
    const double samplesPerBucket = (double)(viewEndSample - viewStartSample) / (double)numBuckets;

    for (int b = 0; b < numBuckets; ++b) {
        const int64_t s0 = viewStartSample + (int64_t)std::floor(b * samplesPerBucket) - orig;
        const int64_t s1 = viewStartSample + (int64_t)std::ceil((b + 1) * samplesPerBucket) - orig;

        const int64_t clamped0 = juce::jmax<int64_t>(vStart, s0);
        const int64_t clamped1 = juce::jmin<int64_t>(vEnd, juce::jmin<int64_t>(bufLen, juce::jmax(clamped0 + 1, s1)));
        if (clamped0 >= clamped1)
            continue;

        const int64_t span = clamped1 - clamped0;
        const int64_t step = juce::jmax<int64_t>(1, span / 64);

        float mn = 0.0f;
        float mx = 0.0f;
        for (int64_t idx = clamped0; idx < clamped1; idx += step) {
            const float v = 0.5f * (ch0[idx] + ch1[idx]);
            mn = juce::jmin(mn, v);
            mx = juce::jmax(mx, v);
        }
        outPeaks[(size_t)b] = { mn, mx };
    }
}

std::vector<float> AudioTimeline::copyMonoRange(int64_t startSample, int64_t numSamples) const {
    if (numSamples <= 0)
        return {};

    const juce::SpinLock::ScopedLockType guard(lock);
    if (!hasOrigin.load(std::memory_order_relaxed))
        return {};

    const int64_t orig = originSample.load(std::memory_order_relaxed);
    const int64_t vStart = validStartOffset.load(std::memory_order_relaxed);
    const int64_t vEnd = validEndOffset.load(std::memory_order_relaxed);
    const int bufLen = storage.getNumSamples();

    std::vector<float> out((size_t)numSamples, 0.0f);
    const float* ch0 = storage.getReadPointer(0);
    const float* ch1 = storage.getReadPointer(1);

    for (int64_t i = 0; i < numSamples; ++i) {
        const int64_t rel = (startSample + i) - orig;
        if (rel >= vStart && rel < vEnd && rel < bufLen)
            out[(size_t)i] = 0.5f * (ch0[rel] + ch1[rel]);
    }
    return out;
}

bool AudioTimeline::extractMono16k(std::vector<float>& outSamples16k,
                                   int64_t& outTimelineStartSample,
                                   double& outSourceSampleRate) const {
    std::vector<float> fullMono;
    if (!extractFullValidMono(fullMono, outTimelineStartSample, outSourceSampleRate))
        return false;

    if (std::abs(outSourceSampleRate - 16000.0) < 1.0) {
        outSamples16k = std::move(fullMono);
        return true;
    }

    const double ratio = outSourceSampleRate / 16000.0;
    const int outLen = juce::jmax(1, (int)std::floor((double)fullMono.size() / ratio));
    outSamples16k.resize((size_t)outLen);

    juce::LagrangeInterpolator interp;
    interp.reset();
    interp.process(ratio, fullMono.data(), outSamples16k.data(), outLen);
    return !outSamples16k.empty();
}

bool AudioTimeline::extractFullValidMono(std::vector<float>& outSamples,
                                         int64_t& outTimelineStartSample,
                                         double& outSourceSampleRate) const {
    const juce::SpinLock::ScopedLockType guard(lock);
    if (!hasOrigin.load(std::memory_order_relaxed))
        return false;

    const int64_t orig = originSample.load(std::memory_order_relaxed);
    const int64_t vStart = validStartOffset.load(std::memory_order_relaxed);
    const int64_t vEnd = juce::jmin<int64_t>(validEndOffset.load(std::memory_order_relaxed), storage.getNumSamples());
    if (vEnd <= vStart)
        return false;

    const int64_t len = vEnd - vStart;
    outTimelineStartSample = orig + vStart;
    outSourceSampleRate = sampleRate.load(std::memory_order_relaxed);
    outSamples.resize((size_t)len);

    const float* ch0 = storage.getReadPointer(0);
    const float* ch1 = storage.getReadPointer(1);
    for (int64_t i = 0; i < len; ++i)
        outSamples[(size_t)i] = 0.5f * (ch0[vStart + i] + ch1[vStart + i]);

    return true;
}
