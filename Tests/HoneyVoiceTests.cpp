#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <iostream>

namespace {

class MockPlayHead : public juce::AudioPlayHead {
public:
    int64_t samplePos = 0;
    bool playing = false;

    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo info;
        info.setTimeInSamples(samplePos);
        info.setIsPlaying(playing);
        return info;
    }
};

static double computeRms(const juce::AudioBuffer<float>& buf, int ch = 0, int start = 0, int count = -1) {
    if (count < 0) count = buf.getNumSamples() - start;
    if (count <= 0) return 0.0;
    double sum = 0.0;
    const float* ptr = buf.getReadPointer(ch);
    for (int i = 0; i < count; ++i) {
        const double v = ptr[start + i];
        sum += v * v;
    }
    return std::sqrt(sum / (double)count);
}

static std::vector<float> makeThreeBurstAudio16k() {
    // 3 bursts at 0.2-0.5s, 0.8-1.1s, 1.4-1.7s in 2.0s of 16kHz audio
    const int sr = 16000;
    std::vector<float> audio((size_t)(sr * 2), 0.0f);
    auto fillBurst = [&](double t0, double t1, double freq) {
        const int s0 = (int)(t0 * sr);
        const int s1 = (int)(t1 * sr);
        for (int i = s0; i < s1; ++i) {
            const double t = (double)i / (double)sr;
            audio[(size_t)i] = 0.5f * (float)std::sin(2.0 * juce::MathConstants<double>::pi * freq * t);
        }
    };
    fillBurst(0.20, 0.50, 220.0);
    fillBurst(0.80, 1.10, 330.0);
    fillBurst(1.40, 1.70, 440.0);
    return audio;
}

class HoneyVoiceUnitTests : public juce::UnitTest {
public:
    HoneyVoiceUnitTests() : juce::UnitTest("HoneyVoice Core & DSP Tests") {}

    void runTest() override {
        testAudioTimelineCaptureAndWavLoad();
        testVocalSegmentationAndLyricAlignment();
        testProcessorPerWordFxAndTails();
        testPreverbAndPitchHarmonyDsp();
        testStandalonePlaybackDoublerAndSearch();
        testStateSerializationRoundTrip();
        testEditorLifecycle();
    }

    void testAudioTimelineCaptureAndWavLoad() {
        beginTest("AudioTimeline real capture, peaks, 16k extraction, and WAV load");
        AudioTimeline tl;
        const double sr = 48000.0;
        tl.prepare(sr);

        juce::AudioBuffer<float> block(2, 1024);
        for (int i = 0; i < 1024; ++i) {
            const float v = 0.6f * std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f * (float)i / (float)sr);
            block.setSample(0, i, v);
            block.setSample(1, i, v);
        }

        const int64_t dawOffset = 48000; // 1.0s into timeline
        tl.captureBlock(block, dawOffset, true);
        expect(tl.hasAudio());
        expectEquals(tl.getValidStartSample(), dawOffset);
        expectEquals(tl.getValidEndSample(), dawOffset + 1024);

        std::vector<AudioTimeline::PeakPair> peaks;
        tl.computePeaks(64, dawOffset, dawOffset + 1024, peaks);
        float maxPeak = 0.0f;
        for (const auto& p : peaks)
            maxPeak = juce::jmax(maxPeak, juce::jmax(std::abs(p.minVal), std::abs(p.maxVal)));
        expectGreaterThan(maxPeak, 0.4f);

        std::vector<float> mono16k;
        int64_t outStart = 0;
        double outSr = 0.0;
        expect(tl.extractMono16k(mono16k, outStart, outSr));
        expectEquals(outStart, dawOffset);
        expectWithinAbsoluteError(outSr, sr, 0.1);
        expectGreaterThan((int)mono16k.size(), 300);

        // Test WAV file load
        const auto tmpWav = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("hv_test_load.wav");
        expect(VocalTranscriber::writeMono16kWav(tmpWav, makeThreeBurstAudio16k()));
        juce::String err;
        expect(tl.loadFromFile(tmpWav, err), err);
        expect(tl.hasAudio());
        expect(tl.isFileLoaded());
        expectGreaterThan(tl.getValidLengthSamples(), (int64_t)(sr * 1.8));
        tmpWav.deleteFile();
    }

    void testVocalSegmentationAndLyricAlignment() {
        beginTest("VocalTranscriber onset segmentation and lyric alignment");
        const auto audio16k = makeThreeBurstAudio16k();
        const double sr = 44100.0;
        const int64_t origin = 10000;

        auto segs = VocalTranscriber::segmentVocalOnsets(audio16k, origin, sr);
        expectEquals((int)segs.size(), 3);
        if (segs.size() == 3) {
            // Burst 1 around 0.2s..0.5s
            expectWithinAbsoluteError((double)(segs[0].startSample - origin) / sr, 0.20, 0.05);
            expectWithinAbsoluteError((double)(segs[0].endSample - origin) / sr, 0.50, 0.05);
            // Burst 2 around 0.8s..1.1s
            expectWithinAbsoluteError((double)(segs[1].startSample - origin) / sr, 0.80, 0.05);
            // Burst 3 around 1.4s..1.7s
            expectWithinAbsoluteError((double)(segs[2].startSample - origin) / sr, 1.40, 0.05);
        }

        auto aligned = VocalTranscriber::alignLyricsToAudio("Hello Honey Voice", audio16k, origin, sr);
        expectEquals((int)aligned.size(), 3);
        if (aligned.size() == 3) {
            expectEquals(aligned[0].word, juce::String("Hello"));
            expectEquals(aligned[1].word, juce::String("Honey"));
            expectEquals(aligned[2].word, juce::String("Voice"));
        }

        VocalTranscriber::Options opts;
        opts.mode = VocalTranscriber::EngineMode::Auto;
        auto res = VocalTranscriber::transcribe(audio16k, origin, sr, opts);
        expect(res.success);
        expect(!res.words.empty());
    }

    void testProcessorPerWordFxAndTails() {
        beginTest("Processor DAW capture + per-word Delay/Reverb/Distortion + tail latching");
        LyricMomentsProcessor proc;
        MockPlayHead playHead;
        proc.setPlayHead(&playHead);

        const double sr = 48000.0;
        const int blockSize = 512;
        proc.prepareToPlay(sr, blockSize);

        LyricWord w;
        w.word = "rebel";
        w.startSample = 0;
        w.endSample = 4800; // 100ms
        w.fx.delayActive = true;
        w.fx.delayAmount = 0.8f;
        w.fx.delayTimeMs = 50.0f; // 2400 samples
        w.fx.delayFeedback = 0.7f;
        w.fx.reverbActive = true;
        w.fx.reverbAmount = 0.6f;
        w.fx.distortionActive = true;
        w.fx.distortionAmount = 0.9f;
        w.fx.drive = 0.8f;
        proc.setWords({ w });
        proc.setMuteDry(true);

        juce::AudioBuffer<float> buf(2, blockSize);
        juce::MidiBuffer midi;
        playHead.playing = true;

        // Feed audio during word [0..4800)
        double activeWetRms = 0.0;
        for (int s = 0; s < 4800; s += blockSize) {
            playHead.samplePos = s;
            for (int i = 0; i < blockSize; ++i) {
                const float v = 0.4f * std::sin(2.0f * juce::MathConstants<float>::pi * 300.0f * (float)(s + i) / (float)sr);
                buf.setSample(0, i, v);
                buf.setSample(1, i, v);
            }
            proc.processBlock(buf, midi);
            activeWetRms = juce::jmax(activeWetRms, computeRms(buf));
        }
        expectGreaterThan(activeWetRms, 0.01);
        expect(proc.getTimeline().hasAudio());

        // Feed silence AFTER word ends -> delay/reverb tail must ring out!
        playHead.samplePos = 5120;
        buf.clear();
        proc.processBlock(buf, midi);
        const double tailRms = computeRms(buf);
        expectGreaterThan(tailRms, 0.005);
    }

    void testPreverbAndPitchHarmonyDsp() {
        beginTest("Preverb reverse-reverb swell + Pitch & Harmony DSP");
        LyricMomentsProcessor proc;
        MockPlayHead playHead;
        proc.setPlayHead(&playHead);

        const double sr = 48000.0;
        const int blockSize = 512;
        proc.prepareToPlay(sr, blockSize);

        // Load real audio into timeline: silence 0..1.0s, tone 1.0..1.5s
        const auto tmpWav = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("hv_preverb_test.wav");
        {
            std::vector<float> a16(16000 * 2, 0.0f);
            for (int i = 16000; i < 24000; ++i)
                a16[(size_t)i] = 0.6f * std::sin(2.0f * juce::MathConstants<float>::pi * 350.0f * (float)i / 16000.0f);
            expect(VocalTranscriber::writeMono16kWav(tmpWav, a16));
        }
        juce::String err;
        expect(proc.getTimeline().loadFromFile(tmpWav, err), err);
        tmpWav.deleteFile();

        LyricWord w;
        w.word = "swell";
        w.startSample = (int64_t)sr;       // 1.0s
        w.endSample = (int64_t)(sr * 1.5); // 1.5s
        w.fx.preverbActive = true;
        w.fx.preverbAmount = 0.9f;
        w.fx.preverbLengthMs = 400.0f;
        w.fx.pitchActive = true;
        w.fx.pitchAmount = 1.0f;
        w.fx.semitoneShift = -12;
        w.fx.harmonyActive = true;
        w.fx.harmonyAmount = 0.7f;
        w.fx.harmonyInterval1 = 4;
        w.fx.harmonyInterval2 = 7;
        proc.setWords({ w });

        // Check preverb output in [0.7s .. 0.95s] while input buffer is silent!
        juce::AudioBuffer<float> buf(2, blockSize);
        juce::MidiBuffer midi;
        playHead.playing = true;
        double preverbRms = 0.0;
        for (int64_t s = (int64_t)(sr * 0.7); s < (int64_t)(sr * 0.95); s += blockSize) {
            playHead.samplePos = s;
            buf.clear();
            proc.processBlock(buf, midi);
            preverbRms = juce::jmax(preverbRms, computeRms(buf));
        }
        expectGreaterThan(preverbRms, 0.005);

        // Check Pitch + Harmony during word [1.0s .. 1.3s]
        proc.setMuteDry(false);
        double shiftedRms = 0.0;
        for (int64_t s = (int64_t)sr; s < (int64_t)(sr * 1.3); s += blockSize) {
            playHead.samplePos = s;
            for (int i = 0; i < blockSize; ++i) {
                const float v = 0.4f * std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f * (float)(s + i) / (float)sr);
                buf.setSample(0, i, v);
                buf.setSample(1, i, v);
            }
            proc.processBlock(buf, midi);
            shiftedRms = juce::jmax(shiftedRms, computeRms(buf));
        }
        expectGreaterThan(shiftedRms, 0.05);
    }

    void testStandalonePlaybackDoublerAndSearch() {
        beginTest("Standalone preview playback, Doubler detune, search & range selection");
        LyricMomentsProcessor proc;
        const double sr = 44100.0;
        const int blockSize = 512;
        proc.prepareToPlay(sr, blockSize);

        const auto tmpWav = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("hv_standalone_test.wav");
        expect(VocalTranscriber::writeMono16kWav(tmpWav, makeThreeBurstAudio16k()));
        juce::String err;
        expect(proc.getTimeline().loadFromFile(tmpWav, err), err);
        tmpWav.deleteFile();

        auto words = VocalTranscriber::alignLyricsToAudio(
            "Rebel Flag Pause",
            makeThreeBurstAudio16k(),
            0,
            sr);
        expectEquals((int)words.size(), 3);
        words[0].fx.doublerActive = true;
        words[0].fx.doublerAmount = 0.85f;
        words[0].fx.doublerDetuneCents = 25.0f;
        proc.setWords(words);

        proc.updateSearchHighlights("flag");
        auto highlighted = proc.getWordsCopy();
        expect(!highlighted[0].isSearchMatch);
        expect(highlighted[1].isSearchMatch);
        expect(!highlighted[2].isSearchMatch);

        proc.selectWordRange(0, 2);
        for (const auto& w : proc.getWordsCopy())
            expect(w.isSelected);

        proc.seekStandaloneSample(words[0].startSample);
        proc.setStandalonePlaying(true);
        juce::AudioBuffer<float> buf(2, blockSize);
        juce::MidiBuffer midi;
        double playbackRms = 0.0;
        for (int b = 0; b < 20; ++b) {
            buf.clear();
            proc.processBlock(buf, midi);
            playbackRms = juce::jmax(playbackRms, computeRms(buf));
        }
        expectGreaterThan(playbackRms, 0.05);
        expect(proc.isTransportPlaying());
    }

    void testStateSerializationRoundTrip() {
        beginTest("State serialization round-trips all words and 7 FX");
        LyricMomentsProcessor procA;
        procA.setMuteDry(true);

        LyricWord w1;
        w1.word = "Honey";
        w1.startSample = 12345;
        w1.endSample = 23456;
        w1.isSelected = true;
        w1.fx.doublerActive = true;
        w1.fx.doublerDetuneCents = 24.5f;
        w1.fx.preverbActive = true;
        w1.fx.preverbLengthMs = 620.0f;
        w1.fx.distortionActive = true;
        w1.fx.drive = 0.85f;
        w1.fx.harmonyActive = true;
        w1.fx.harmonyInterval1 = 3;
        w1.fx.harmonyInterval2 = -5;
        w1.fx.pitchActive = true;
        w1.fx.semitoneShift = -7;

        procA.setWords({ w1 });

        juce::MemoryBlock stateMem;
        procA.getStateInformation(stateMem);
        expectGreaterThan((int)stateMem.getSize(), 20);

        LyricMomentsProcessor procB;
        procB.setStateInformation(stateMem.getData(), (int)stateMem.getSize());

        expect(procB.getMuteDry());
        const auto restored = procB.getWordsCopy();
        expectEquals((int)restored.size(), 1);
        if (restored.size() == 1) {
            expectEquals(restored[0].word, juce::String("Honey"));
            expectEquals(restored[0].startSample, (int64_t)12345);
            expectEquals(restored[0].endSample, (int64_t)23456);
            expect(restored[0].isSelected);
            expect(restored[0].fx.doublerActive);
            expectWithinAbsoluteError(restored[0].fx.doublerDetuneCents, 24.5f, 0.01f);
            expect(restored[0].fx.preverbActive);
            expectWithinAbsoluteError(restored[0].fx.preverbLengthMs, 620.0f, 0.1f);
            expect(restored[0].fx.distortionActive);
            expectWithinAbsoluteError(restored[0].fx.drive, 0.85f, 0.01f);
            expect(restored[0].fx.harmonyActive);
            expectEquals(restored[0].fx.harmonyInterval1, 3);
            expectEquals(restored[0].fx.harmonyInterval2, -5);
            expect(restored[0].fx.pitchActive);
            expectEquals(restored[0].fx.semitoneShift, -7);
        }
    }

    void testEditorLifecycle() {
        beginTest("PluginEditor creation and teardown");
        LyricMomentsProcessor proc;
        std::unique_ptr<juce::AudioProcessorEditor> ed(proc.createEditor());
        expect(ed != nullptr);
        if (ed) {
            ed->setBounds(0, 0, 1000, 580);
            ed->resized();
        }
        ed.reset();
    }
};

static HoneyVoiceUnitTests s_honeyVoiceTests;

} // namespace

class ConsoleTestRunner : public juce::UnitTestRunner {
protected:
    void logMessage(const juce::String& message) override {
        std::cout << message.toStdString() << std::endl;
    }
};

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;
    ConsoleTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.runAllTests();

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i) {
        if (auto* res = runner.getResult(i))
            failures += res->failures;
    }

    std::cout << "Total test failures: " << failures << std::endl;
    return failures == 0 ? 0 : 1;
}
