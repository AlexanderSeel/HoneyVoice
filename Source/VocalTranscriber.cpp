#include "VocalTranscriber.h"
#include <algorithm>
#include <cmath>
#include <limits>

#if HONEYVOICE_WITH_WHISPER
#include <whisper.h>
#endif

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sapi.h>
#include <objbase.h>
#endif

namespace {

static juce::String sanitizeWordToken(const juce::String& raw) {
    return raw.trim();
}

static int estimateSyllableWeight(const juce::String& word) {
    int vowels = 0;
    bool prevVowel = false;
    const auto lower = word.toLowerCase();
    for (int i = 0; i < lower.length(); ++i) {
        const auto c = lower[i];
        const bool isV = (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y');
        if (isV && !prevVowel)
            ++vowels;
        prevVowel = isV;
    }
    return juce::jmax(1, vowels);
}

static std::vector<juce::String> tokenizeLyrics(const juce::String& text) {
    juce::StringArray rawTokens;
    rawTokens.addTokens(text, " \t\r\n", "\"");
    std::vector<juce::String> out;
    out.reserve((size_t)rawTokens.size());
    for (const auto& t : rawTokens) {
        const auto trimmed = t.trim();
        if (trimmed.isNotEmpty())
            out.push_back(trimmed);
    }
    return out;
}

static std::vector<float> computeRmsFrames10ms(const std::vector<float>& mono16k) {
    constexpr int hop = 160; // 10ms at 16 kHz
    constexpr int win = 320; // 20ms window
    const int n = (int)mono16k.size();
    if (n <= 0)
        return {};

    const int numFrames = juce::jmax(1, (n + hop - 1) / hop);
    std::vector<float> rms((size_t)numFrames, 0.0f);
    for (int f = 0; f < numFrames; ++f) {
        const int start = f * hop;
        const int end = juce::jmin(n, start + win);
        double sumSq = 0.0;
        int count = 0;
        for (int i = start; i < end; ++i) {
            const float s = mono16k[(size_t)i];
            sumSq += (double)s * (double)s;
            ++count;
        }
        rms[(size_t)f] = count > 0 ? (float)std::sqrt(sumSq / (double)count) : 0.0f;
    }
    return rms;
}

} // namespace

juce::File VocalTranscriber::getDefaultModelsDirectory() {
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("HoneyVoice")
                   .getChildFile("models");
    dir.createDirectory();
    return dir;
}

juce::Array<juce::File> VocalTranscriber::findAvailableWhisperModels() {
    juce::Array<juce::File> models;
    const juce::Array<juce::File> searchDirs = {
        getDefaultModelsDirectory(),
        juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".cache").getChildFile("whisper"),
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("HoneyVoice").getChildFile("models")
    };

    for (const auto& d : searchDirs) {
        if (!d.isDirectory())
            continue;
        for (const auto& f : d.findChildFiles(juce::File::findFiles, false, "ggml-*.bin"))
            models.addIfNotAlreadyThere(f);
    }
    return models;
}

bool VocalTranscriber::writeMono16kWav(const juce::File& destFile, const std::vector<float>& mono16k) {
    if (mono16k.empty())
        return false;

    destFile.deleteFile();
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::FileOutputStream> stream(destFile.createOutputStream());
    if (!stream || !stream->openedOk())
        return false;

    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(stream.release(), 16000.0, 1, 16, {}, 0));
    if (!writer)
        return false;

    juce::AudioBuffer<float> buf(1, (int)mono16k.size());
    std::copy(mono16k.begin(), mono16k.end(), buf.getWritePointer(0));
    return writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
}

std::vector<LyricWord> VocalTranscriber::segmentVocalOnsets(const std::vector<float>& mono16k,
                                                            int64_t timelineStartSample,
                                                            double sourceSampleRate) {
    std::vector<LyricWord> segments;
    if (mono16k.empty() || sourceSampleRate <= 0.0)
        return segments;

    const auto rms = computeRmsFrames10ms(mono16k);
    if (rms.empty())
        return segments;

    float maxRms = 0.0f;
    for (float v : rms)
        maxRms = juce::jmax(maxRms, v);
    if (maxRms < 1e-4f)
        return segments;

    std::vector<float> sorted = rms;
    std::sort(sorted.begin(), sorted.end());
    const float noiseFloor = sorted[sorted.size() * 15 / 100];
    const float startThresh = juce::jmax(0.006f, juce::jmax(maxRms * 0.09f, noiseFloor * 3.0f));
    const float stopThresh = startThresh * 0.55f;

    constexpr int minSilenceGapFrames = 8; // 80ms gap merges intra-word consonant pauses
    constexpr int minWordFrames = 6;       // 60ms minimum segment
    constexpr int dipWindowFrames = 14;    // 140ms minimum between split points

    struct FrameSpan { int startF; int endF; };
    std::vector<FrameSpan> activeSpans;

    bool inVoice = false;
    int curStart = 0;
    int belowCount = 0;
    const int numFrames = (int)rms.size();

    for (int f = 0; f < numFrames; ++f) {
        if (!inVoice) {
            if (rms[(size_t)f] >= startThresh) {
                inVoice = true;
                curStart = juce::jmax(0, f - 1);
                belowCount = 0;
            }
        } else {
            if (rms[(size_t)f] < stopThresh) {
                ++belowCount;
                if (belowCount >= minSilenceGapFrames) {
                    const int curEnd = f - belowCount + 1;
                    if (curEnd - curStart >= minWordFrames)
                        activeSpans.push_back({ curStart, curEnd });
                    inVoice = false;
                    belowCount = 0;
                }
            } else {
                belowCount = 0;
            }
        }
    }
    if (inVoice) {
        const int curEnd = numFrames;
        if (curEnd - curStart >= minWordFrames)
            activeSpans.push_back({ curStart, curEnd });
    }

    // Split long phrases at clear energy dips (syllable/word boundaries)
    std::vector<FrameSpan> wordSpans;
    for (const auto& span : activeSpans) {
        if (span.endF - span.startF <= 45) {
            wordSpans.push_back(span);
            continue;
        }

        int segStart = span.startF;
        float runningPeak = rms[(size_t)segStart];
        for (int f = span.startF + dipWindowFrames; f <= span.endF - dipWindowFrames; ++f) {
            runningPeak = juce::jmax(runningPeak * 0.96f, rms[(size_t)f]);
            const float cur = rms[(size_t)f];
            const bool isLocalMin = cur <= rms[(size_t)(f - 1)] && cur <= rms[(size_t)(f + 1)];
            if (isLocalMin && cur < runningPeak * 0.36f && (f - segStart) >= dipWindowFrames) {
                // Find post-dip rise to confirm next word onset
                float postMax = 0.0f;
                for (int k = f + 1; k < juce::jmin(span.endF, f + 12); ++k)
                    postMax = juce::jmax(postMax, rms[(size_t)k]);
                if (postMax > cur * 1.65f) {
                    wordSpans.push_back({ segStart, f });
                    segStart = f;
                    runningPeak = postMax;
                }
            }
        }
        if (span.endF - segStart >= minWordFrames)
            wordSpans.push_back({ segStart, span.endF });
    }

    const double framesPerSec = 100.0; // 10ms hop
    for (size_t i = 0; i < wordSpans.size(); ++i) {
        LyricWord w;
        w.word = "Word " + juce::String((int)i + 1);
        const double t0 = (double)wordSpans[i].startF / framesPerSec;
        const double t1 = (double)wordSpans[i].endF / framesPerSec;
        w.startSample = timelineStartSample + (int64_t)std::llround(t0 * sourceSampleRate);
        w.endSample = timelineStartSample + (int64_t)std::llround(t1 * sourceSampleRate);
        if (w.endSample > w.startSample)
            segments.push_back(std::move(w));
    }

    return segments;
}

std::vector<LyricWord> VocalTranscriber::alignLyricsToAudio(const juce::String& lyricsText,
                                                            const std::vector<float>& mono16k,
                                                            int64_t timelineStartSample,
                                                            double sourceSampleRate) {
    const auto tokens = tokenizeLyrics(lyricsText);
    if (tokens.empty())
        return segmentVocalOnsets(mono16k, timelineStartSample, sourceSampleRate);

    auto segments = segmentVocalOnsets(mono16k, timelineStartSample, sourceSampleRate);
    const int64_t totalSourceSamples = (int64_t)std::llround(((double)mono16k.size() / 16000.0) * sourceSampleRate);
    if (segments.empty() && totalSourceSamples > 0) {
        LyricWord full;
        full.startSample = timelineStartSample;
        full.endSample = timelineStartSample + totalSourceSamples;
        segments.push_back(full);
    }
    if (segments.empty())
        return {};

    std::vector<LyricWord> aligned;
    aligned.reserve(tokens.size());

    if (tokens.size() <= segments.size()) {
        // Merge excess adjacent segments or pick largest energy spans proportionally
        const double segPerWord = (double)segments.size() / (double)tokens.size();
        for (size_t i = 0; i < tokens.size(); ++i) {
            const size_t s0 = juce::jmin(segments.size() - 1, (size_t)std::floor((double)i * segPerWord));
            const size_t s1 = juce::jmax(s0, juce::jmin(segments.size() - 1, (size_t)std::floor((double)(i + 1) * segPerWord) - 1));
            LyricWord w;
            w.word = tokens[i];
            w.startSample = segments[s0].startSample;
            w.endSample = segments[s1].endSample;
            aligned.push_back(std::move(w));
        }
    } else {
        // More words than detected voiced segments: distribute words across segments weighted by segment duration & word syllables
        int64_t totalVoicedSamples = 0;
        for (const auto& s : segments)
            totalVoicedSamples += juce::jmax<int64_t>(1, s.endSample - s.startSample);

        int totalWeight = 0;
        std::vector<int> weights(tokens.size(), 1);
        for (size_t i = 0; i < tokens.size(); ++i) {
            weights[i] = estimateSyllableWeight(tokens[i]);
            totalWeight += weights[i];
        }

        auto mapVoicedOffsetToTimeline = [&](int64_t voicedPos) -> int64_t {
            int64_t acc = 0;
            for (const auto& s : segments) {
                const int64_t len = juce::jmax<int64_t>(1, s.endSample - s.startSample);
                if (voicedPos <= acc + len)
                    return s.startSample + (voicedPos - acc);
                acc += len;
            }
            return segments.back().endSample;
        };

        int curWeight = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            const int64_t v0 = (totalVoicedSamples * curWeight) / juce::jmax(1, totalWeight);
            curWeight += weights[i];
            const int64_t v1 = (totalVoicedSamples * curWeight) / juce::jmax(1, totalWeight);
            LyricWord w;
            w.word = tokens[i];
            w.startSample = mapVoicedOffsetToTimeline(v0);
            w.endSample = juce::jmax(w.startSample + (int64_t)(sourceSampleRate * 0.04), mapVoicedOffsetToTimeline(v1));
            aligned.push_back(std::move(w));
        }
    }

    return aligned;
}

VocalTranscriber::Result VocalTranscriber::tryEmbeddedWhisper(const std::vector<float>& mono16k,
                                                              int64_t timelineStartSample,
                                                              double sourceSampleRate,
                                                              const Options& options) {
    Result res;
#if HONEYVOICE_WITH_WHISPER
    juce::File modelFile = options.whisperModelFile;
    if (!modelFile.existsAsFile()) {
        const auto found = findAvailableWhisperModels();
        if (!found.isEmpty())
            modelFile = found.getFirst();
    }
    if (!modelFile.existsAsFile()) {
        res.statusMessage = "No Whisper GGML model (.bin) found in HoneyVoice/models.";
        return res;
    }

    whisper_context_params cparams = whisper_context_default_params();
    whisper_context* ctx = whisper_init_from_file_with_params(modelFile.getFullPathName().toRawUTF8(), cparams);
    if (!ctx) {
        res.statusMessage = "Failed to initialize Whisper model: " + modelFile.getFileName();
        return res;
    }

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.token_timestamps = true;
    wparams.split_on_word = true;
    wparams.max_len = 1;
    wparams.language = options.language.isNotEmpty() ? options.language.toRawUTF8() : "en";
    if (options.hintLyrics.isNotEmpty())
        wparams.initial_prompt = options.hintLyrics.toRawUTF8();

    if (whisper_full(ctx, wparams, mono16k.data(), (int)mono16k.size()) != 0) {
        whisper_free(ctx);
        res.statusMessage = "Whisper inference failed.";
        return res;
    }

    const int nSegments = whisper_full_n_segments(ctx);
    for (int s = 0; s < nSegments; ++s) {
        const int nTokens = whisper_full_n_tokens(ctx, s);
        juce::String curWord;
        int64_t curT0 = -1;
        int64_t curT1 = -1;

        auto flushWord = [&]() {
            const auto cleaned = sanitizeWordToken(curWord);
            if (cleaned.isNotEmpty() && !cleaned.startsWith("[") && !cleaned.startsWith("(") && curT0 >= 0) {
                LyricWord lw;
                lw.word = cleaned;
                lw.startSample = timelineStartSample + (int64_t)std::llround(((double)curT0 * 0.01) * sourceSampleRate);
                lw.endSample = timelineStartSample + (int64_t)std::llround(((double)juce::jmax(curT0 + 5, curT1) * 0.01) * sourceSampleRate);
                res.words.push_back(std::move(lw));
            }
            curWord.clear();
            curT0 = -1;
            curT1 = -1;
        };

        for (int t = 0; t < nTokens; ++t) {
            const char* txtPtr = whisper_full_get_token_text(ctx, s, t);
            if (!txtPtr) continue;
            const juce::String tok = juce::String::fromUTF8(txtPtr);
            if (tok.startsWith("[_") || tok.isEmpty())
                continue;

            const auto td = whisper_full_get_token_data(ctx, s, t);
            if (tok.startsWith(" ") && curWord.isNotEmpty())
                flushWord();

            if (curT0 < 0) curT0 = td.t0;
            curT1 = td.t1;
            curWord += tok;
        }
        flushWord();
    }

    whisper_free(ctx);
    res.success = !res.words.empty();
    res.engineUsed = "Whisper (" + modelFile.getFileName() + ")";
    res.statusMessage = res.success ? ("Transcribed " + juce::String((int)res.words.size()) + " words with Whisper.")
                                    : "Whisper detected no speech words.";
#else
    juce::ignoreUnused(mono16k, timelineStartSample, sourceSampleRate, options);
    res.statusMessage = "Embedded Whisper not compiled in this build.";
#endif
    return res;
}

VocalTranscriber::Result VocalTranscriber::tryExternalWhisperCli(const std::vector<float>& mono16k,
                                                                 int64_t timelineStartSample,
                                                                 double sourceSampleRate,
                                                                 const Options& options) {
    Result res;
    juce::File modelFile = options.whisperModelFile;
    if (!modelFile.existsAsFile()) {
        const auto models = findAvailableWhisperModels();
        if (!models.isEmpty())
            modelFile = models.getFirst();
    }

    const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("HoneyVoiceTranscribe");
    tempDir.createDirectory();
    const auto wavFile = tempDir.getChildFile("capture_16k.wav");
    if (!writeMono16kWav(wavFile, mono16k))
        return res;

    // Try whisper-cli (whisper.cpp CLI) if a GGML model exists
    if (modelFile.existsAsFile()) {
        for (const auto& exeName : juce::StringArray { "whisper-cli", "whisper-cpp", "main" }) {
            const auto outPrefix = tempDir.getChildFile("whisper_out");
            const auto jsonFile = tempDir.getChildFile("whisper_out.json");
            jsonFile.deleteFile();

            juce::ChildProcess proc;
            juce::StringArray args {
                exeName,
                "-m", modelFile.getFullPathName(),
                "-f", wavFile.getFullPathName(),
                "-ojf",
                "-of", outPrefix.getFullPathName(),
                "-l", options.language.isNotEmpty() ? options.language : "en"
            };
            if (proc.start(args) && proc.waitForProcessToFinish(45000) && proc.getExitCode() == 0 && jsonFile.existsAsFile()) {
                const auto parsed = juce::JSON::parse(jsonFile);
                if (auto* root = parsed.getDynamicObject()) {
                    const auto transcription = root->getProperty("transcription");
                    if (auto* arr = transcription.getArray()) {
                        for (const auto& segVar : *arr) {
                            auto* segObj = segVar.getDynamicObject();
                            if (!segObj) continue;
                            auto* tokArr = segObj->getProperty("tokens").getArray();
                            if (tokArr && !tokArr->isEmpty()) {
                                juce::String curWord;
                                int64_t curT0Ms = -1;
                                int64_t curT1Ms = -1;
                                auto flush = [&]() {
                                    const auto cleaned = sanitizeWordToken(curWord);
                                    if (cleaned.isNotEmpty() && !cleaned.startsWith("[") && curT0Ms >= 0) {
                                        LyricWord lw;
                                        lw.word = cleaned;
                                        lw.startSample = timelineStartSample + (int64_t)std::llround(((double)curT0Ms / 1000.0) * sourceSampleRate);
                                        lw.endSample = timelineStartSample + (int64_t)std::llround(((double)juce::jmax(curT0Ms + 60, curT1Ms) / 1000.0) * sourceSampleRate);
                                        res.words.push_back(std::move(lw));
                                    }
                                    curWord.clear();
                                    curT0Ms = -1;
                                    curT1Ms = -1;
                                };
                                for (const auto& tVar : *tokArr) {
                                    auto* tObj = tVar.getDynamicObject();
                                    if (!tObj) continue;
                                    const juce::String tok = tObj->getProperty("text").toString();
                                    if (tok.startsWith("[_")) continue;
                                    auto* offObj = tObj->getProperty("offsets").getDynamicObject();
                                    const int64_t t0 = offObj ? (int64_t)offObj->getProperty("from") : 0;
                                    const int64_t t1 = offObj ? (int64_t)offObj->getProperty("to") : t0 + 100;
                                    if (tok.startsWith(" ") && curWord.isNotEmpty())
                                        flush();
                                    if (curT0Ms < 0) curT0Ms = t0;
                                    curT1Ms = t1;
                                    curWord += tok;
                                }
                                flush();
                            }
                        }
                    }
                }
                wavFile.deleteFile();
                jsonFile.deleteFile();
                if (!res.words.empty()) {
                    res.success = true;
                    res.engineUsed = "Whisper CLI";
                    res.statusMessage = "Transcribed " + juce::String((int)res.words.size()) + " words via Whisper CLI.";
                    return res;
                }
            }
        }
    }

    wavFile.deleteFile();
    return res;
}

VocalTranscriber::Result VocalTranscriber::tryWindowsSapi(const std::vector<float>& mono16k,
                                                          int64_t timelineStartSample,
                                                          double sourceSampleRate) {
    Result res;
#if JUCE_WINDOWS
    const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("HoneyVoiceTranscribe");
    tempDir.createDirectory();
    const auto wavFile = tempDir.getChildFile("sapi_input_16k.wav");
    if (!writeMono16kWav(wavFile, mono16k)) {
        res.statusMessage = "Failed to prepare temporary WAV for Windows Speech Recognition.";
        return res;
    }

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = (comHr == S_OK || comHr == S_FALSE);

    ISpRecognizer* recognizer = nullptr;
    ISpStream* audioStream = nullptr;
    ISpRecoContext* recoContext = nullptr;
    ISpRecoGrammar* grammar = nullptr;

    auto cleanup = [&]() {
        if (grammar) { grammar->Release(); grammar = nullptr; }
        if (recoContext) { recoContext->Release(); recoContext = nullptr; }
        if (audioStream) { audioStream->Close(); audioStream->Release(); audioStream = nullptr; }
        if (recognizer) { recognizer->Release(); recognizer = nullptr; }
        wavFile.deleteFile();
        if (needUninit) CoUninitialize();
    };

    HRESULT hr = CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_ISpRecognizer, (void**)&recognizer);
    if (FAILED(hr) || !recognizer) {
        cleanup();
        res.statusMessage = "Windows SAPI recognizer unavailable.";
        return res;
    }

    hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream, (void**)&audioStream);
    if (FAILED(hr) || !audioStream) {
        cleanup();
        res.statusMessage = "Windows SAPI stream unavailable.";
        return res;
    }

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 16000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = 32000;

    // SPDFID_WaveFormatEx = {C31ADBAE-527F-4FF5-A230-F62BB61FF70C}
    static const GUID kSpdfidWaveFormatEx =
        { 0xc31adbae, 0x527f, 0x4ff5, { 0xa2, 0x30, 0xf6, 0x2b, 0xb6, 0x1f, 0xf7, 0x0c } };

    hr = audioStream->BindToFile(wavFile.getFullPathName().toWideCharPointer(),
                                 SPFM_OPEN_READONLY,
                                 &kSpdfidWaveFormatEx,
                                 &wfx,
                                 0);
    if (FAILED(hr)) {
        cleanup();
        res.statusMessage = "Windows SAPI failed to bind audio stream.";
        return res;
    }

    hr = recognizer->SetInput(audioStream, TRUE);
    if (FAILED(hr)) {
        cleanup();
        res.statusMessage = "Windows SAPI failed to attach input stream (check Windows Speech language pack).";
        return res;
    }

    hr = recognizer->CreateRecoContext(&recoContext);
    if (FAILED(hr) || !recoContext) {
        cleanup();
        res.statusMessage = "Windows SAPI failed to create recognition context.";
        return res;
    }

    const ULONGLONG interest = SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_END_SR_STREAM);
    recoContext->SetInterest(interest, interest);
    recoContext->SetNotifyWin32Event();

    hr = recoContext->CreateGrammar(1, &grammar);
    if (FAILED(hr) || !grammar) {
        cleanup();
        res.statusMessage = "Windows SAPI failed to create dictation grammar.";
        return res;
    }

    hr = grammar->LoadDictation(nullptr, SPLO_STATIC);
    if (FAILED(hr)) {
        cleanup();
        res.statusMessage = "Windows SAPI dictation grammar unavailable (install a Windows Speech language pack).";
        return res;
    }

    hr = grammar->SetDictationState(SPRS_ACTIVE);
    if (FAILED(hr)) {
        cleanup();
        res.statusMessage = "Windows SAPI failed to activate dictation.";
        return res;
    }

    const double audioDurSec = (double)mono16k.size() / 16000.0;
    const DWORD timeoutMs = (DWORD)juce::jlimit(8000, 60000, (int)std::ceil(audioDurSec * 2000.0) + 5000);
    const auto deadline = juce::Time::getMillisecondCounter() + timeoutMs;

    bool finished = false;
    while (!finished && juce::Time::getMillisecondCounter() < deadline) {
        recoContext->WaitForNotifyEvent(250);
        SPEVENT event{};
        ULONG fetched = 0;
        while (recoContext->GetEvents(1, &event, &fetched) == S_OK && fetched > 0) {
            if (event.eEventId == SPEI_END_SR_STREAM) {
                finished = true;
            } else if (event.eEventId == SPEI_RECOGNITION && event.elParamType == SPET_LPARAM_IS_OBJECT) {
                auto* recoResult = reinterpret_cast<ISpRecoResult*>(event.lParam);
                if (recoResult) {
                    SPPHRASE* phrase = nullptr;
                    if (SUCCEEDED(recoResult->GetPhrase(&phrase)) && phrase) {
                        for (ULONG i = 0; i < phrase->Rule.ulCountOfElements; ++i) {
                            const auto& elem = phrase->pElements[i];
                            if (!elem.pszDisplayText) continue;
                            const juce::String text = sanitizeWordToken(juce::String(elem.pszDisplayText));
                            if (text.isEmpty()) continue;

                            // SAPI audio offsets are in 100-nanosecond units (1e-7 sec)
                            const double t0Sec = (double)elem.ulAudioTimeOffset * 1e-7;
                            const double durSec = juce::jmax(0.06, (double)elem.ulAudioSizeTime * 1e-7);

                            LyricWord lw;
                            lw.word = text;
                            lw.startSample = timelineStartSample + (int64_t)std::llround(t0Sec * sourceSampleRate);
                            lw.endSample = timelineStartSample + (int64_t)std::llround((t0Sec + durSec) * sourceSampleRate);
                            res.words.push_back(std::move(lw));
                        }
                        CoTaskMemFree(phrase);
                    }
                    recoResult->Release();
                }
            }
        }
    }

    cleanup();
    res.success = !res.words.empty();
    res.engineUsed = "Windows Speech (SAPI)";
    res.statusMessage = res.success
        ? ("Transcribed " + juce::String((int)res.words.size()) + " words with Windows Speech.")
        : "Windows Speech detected no recognizable words.";
#else
    juce::ignoreUnused(mono16k, timelineStartSample, sourceSampleRate);
    res.statusMessage = "Windows SAPI only available on Windows.";
#endif
    return res;
}

void VocalTranscriber::refineWordBoundaries(std::vector<LyricWord>& words,
                                            const std::vector<float>& mono16k,
                                            int64_t timelineStartSample,
                                            double sourceSampleRate) {
    if (words.empty() || mono16k.empty() || sourceSampleRate <= 0.0)
        return;

    const auto rms = computeRmsFrames10ms(mono16k);
    if (rms.empty())
        return;

    float globalPeak = 0.0f;
    for (float v : rms) globalPeak = juce::jmax(globalPeak, v);
    const float silenceCut = juce::jmax(0.003f, globalPeak * 0.04f);

    for (auto& w : words) {
        const double s0Sec = (double)(w.startSample - timelineStartSample) / sourceSampleRate;
        const double s1Sec = (double)(w.endSample - timelineStartSample) / sourceSampleRate;
        int f0 = juce::jlimit(0, (int)rms.size() - 1, (int)std::floor(s0Sec * 100.0));
        int f1 = juce::jlimit(f0 + 1, (int)rms.size(), (int)std::ceil(s1Sec * 100.0));

        while (f0 + 2 < f1 && rms[(size_t)f0] < silenceCut) ++f0;
        while (f1 - 2 > f0 && rms[(size_t)(f1 - 1)] < silenceCut) --f1;

        w.startSample = timelineStartSample + (int64_t)std::llround(((double)f0 * 0.01) * sourceSampleRate);
        w.endSample = timelineStartSample + (int64_t)std::llround(((double)f1 * 0.01) * sourceSampleRate);
    }
}

VocalTranscriber::Result VocalTranscriber::transcribe(const std::vector<float>& mono16k,
                                                      int64_t timelineStartSample,
                                                      double sourceSampleRate,
                                                      const Options& options) {
    Result finalRes;
    if (mono16k.empty() || sourceSampleRate <= 0.0) {
        finalRes.statusMessage = "No captured audio available to transcribe.";
        return finalRes;
    }

    if (options.mode == EngineMode::SegmentOnly) {
        finalRes.words = options.hintLyrics.trim().isNotEmpty()
            ? alignLyricsToAudio(options.hintLyrics, mono16k, timelineStartSample, sourceSampleRate)
            : segmentVocalOnsets(mono16k, timelineStartSample, sourceSampleRate);
        finalRes.success = !finalRes.words.empty();
        finalRes.engineUsed = options.hintLyrics.trim().isNotEmpty() ? "Lyric Aligner" : "Vocal Segmenter";
        finalRes.statusMessage = finalRes.success
            ? ("Aligned " + juce::String((int)finalRes.words.size()) + " vocal segments from real audio.")
            : "Audio is silent — no vocal segments detected.";
        return finalRes;
    }

    // 1. Embedded Whisper
    if (options.mode == EngineMode::Auto || options.mode == EngineMode::WhisperOnly) {
        auto wRes = tryEmbeddedWhisper(mono16k, timelineStartSample, sourceSampleRate, options);
        if (wRes.success) {
            refineWordBoundaries(wRes.words, mono16k, timelineStartSample, sourceSampleRate);
            return wRes;
        }
        auto cliRes = tryExternalWhisperCli(mono16k, timelineStartSample, sourceSampleRate, options);
        if (cliRes.success) {
            refineWordBoundaries(cliRes.words, mono16k, timelineStartSample, sourceSampleRate);
            return cliRes;
        }
        if (options.mode == EngineMode::WhisperOnly)
            return wRes;
    }

    // 2. Native Windows SAPI 5.4 offline speech recognizer
    if (options.mode == EngineMode::Auto || options.mode == EngineMode::WindowsSapiOnly) {
        auto sapiRes = tryWindowsSapi(mono16k, timelineStartSample, sourceSampleRate);
        if (sapiRes.success) {
            refineWordBoundaries(sapiRes.words, mono16k, timelineStartSample, sourceSampleRate);
            return sapiRes;
        }
        if (options.mode == EngineMode::WindowsSapiOnly)
            return sapiRes;
    }

    // 3. Universal Acoustic Segmenter (+ hint lyric alignment if provided)
    finalRes.words = options.hintLyrics.trim().isNotEmpty()
        ? alignLyricsToAudio(options.hintLyrics, mono16k, timelineStartSample, sourceSampleRate)
        : segmentVocalOnsets(mono16k, timelineStartSample, sourceSampleRate);
    finalRes.success = !finalRes.words.empty();
    finalRes.engineUsed = options.hintLyrics.trim().isNotEmpty() ? "Lyric Aligner" : "Vocal Segmenter";
    finalRes.statusMessage = finalRes.success
        ? ("Detected " + juce::String((int)finalRes.words.size()) + " real vocal segments (add a Whisper model or lyrics for word text).")
        : "No audible vocal phrases detected in captured audio.";
    return finalRes;
}
