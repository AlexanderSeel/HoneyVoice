#pragma once
#include <JuceHeader.h>
#include "WordModels.h"
#include <vector>

/**
 * Multi-backend vocal speech-to-text & acoustic word alignment engine.
 * Supports:
 *  1. Embedded whisper.cpp (when built with HONEYVOICE_WITH_WHISPER=ON and a GGML model)
 *  2. External Whisper CLI (whisper-cli / whisper) if available on PATH
 *  3. Native Windows SAPI 5.4 offline dictation with word-level audio timestamps
 *  4. Acoustic RMS/onset vocal segmenter + lyric-to-audio aligner (universal fallback)
 */
class VocalTranscriber {
public:
    enum class EngineMode {
        Auto = 0,
        WhisperOnly,
        WindowsSapiOnly,
        SegmentOnly
    };

    struct Options {
        EngineMode mode = EngineMode::Auto;
        juce::File whisperModelFile;
        juce::String hintLyrics;
        juce::String language { "en" };
    };

    struct Result {
        bool success = false;
        juce::String engineUsed;
        juce::String statusMessage;
        std::vector<LyricWord> words;
    };

    static Result transcribe(const std::vector<float>& mono16k,
                             int64_t timelineStartSample,
                             double sourceSampleRate,
                             const Options& options);

    /** Detects real voiced/vocal word segments from 16kHz mono audio using RMS + dip splitting. */
    static std::vector<LyricWord> segmentVocalOnsets(const std::vector<float>& mono16k,
                                                     int64_t timelineStartSample,
                                                     double sourceSampleRate);

    /** Aligns user-supplied lyrics text onto detected vocal segments (or full active span). */
    static std::vector<LyricWord> alignLyricsToAudio(const juce::String& lyricsText,
                                                     const std::vector<float>& mono16k,
                                                     int64_t timelineStartSample,
                                                     double sourceSampleRate);

    /** Discovers GGML Whisper models in standard app-data and cache directories. */
    static juce::Array<juce::File> findAvailableWhisperModels();
    static juce::File getDefaultModelsDirectory();

    /** Writes 16kHz 16-bit mono PCM WAV to disk (used for SAPI, Whisper CLI, and tests). */
    static bool writeMono16kWav(const juce::File& destFile, const std::vector<float>& mono16k);

private:
    static Result tryEmbeddedWhisper(const std::vector<float>& mono16k,
                                     int64_t timelineStartSample,
                                     double sourceSampleRate,
                                     const Options& options);

    static Result tryExternalWhisperCli(const std::vector<float>& mono16k,
                                        int64_t timelineStartSample,
                                        double sourceSampleRate,
                                        const Options& options);

    static Result tryWindowsSapi(const std::vector<float>& mono16k,
                                 int64_t timelineStartSample,
                                 double sourceSampleRate);

    static void refineWordBoundaries(std::vector<LyricWord>& words,
                                     const std::vector<float>& mono16k,
                                     int64_t timelineStartSample,
                                     double sourceSampleRate);
};
