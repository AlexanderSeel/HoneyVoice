cmake_minimum_required(VERSION 3.22)
project(HoneyVoice VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HONEYVOICE_BUILD_TESTS "Build HoneyVoice headless unit tests" ON)
option(HONEYVOICE_WITH_WHISPER "Build with embedded whisper.cpp transcription engine" OFF)
set(HONEYVOICE_JUCE_DIR "" CACHE PATH "Optional path to a local JUCE checkout")
set(HONEYVOICE_WHISPER_DIR "" CACHE PATH "Optional path to a local whisper.cpp checkout")

if(HONEYVOICE_JUCE_DIR AND EXISTS "${HONEYVOICE_JUCE_DIR}/CMakeLists.txt")
    message(STATUS "Using local JUCE: ${HONEYVOICE_JUCE_DIR}")
    add_subdirectory("${HONEYVOICE_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

if(HONEYVOICE_WITH_WHISPER)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    if(HONEYVOICE_WHISPER_DIR AND EXISTS "${HONEYVOICE_WHISPER_DIR}/CMakeLists.txt")
        add_subdirectory("${HONEYVOICE_WHISPER_DIR}" "${CMAKE_BINARY_DIR}/whisper.cpp")
    else()
        include(FetchContent)
        FetchContent_Declare(
            whispercpp
            GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
            GIT_TAG v1.7.5
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(whispercpp)
    endif()
endif()

set(HONEYVOICE_SOURCES
    PluginProcessor.cpp
    PluginEditor.cpp
    Source/AudioTimeline.cpp
    Source/VocalTranscriber.cpp
)

juce_add_plugin(HoneyVoice
    COMPANY_NAME "AlexanderSeel"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    PLUGIN_MANUFACTURER_CODE Asee
    PLUGIN_CODE HnVc
    FORMATS VST3 Standalone
    PRODUCT_NAME "HoneyVoice"
)

juce_generate_juce_header(HoneyVoice)

target_sources(HoneyVoice PRIVATE ${HONEYVOICE_SOURCES})

target_include_directories(HoneyVoice PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
)

target_compile_definitions(HoneyVoice PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    HONEYVOICE_WITH_WHISPER=$<BOOL:${HONEYVOICE_WITH_WHISPER}>
)

target_link_libraries(HoneyVoice PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

if(WIN32)
    target_link_libraries(HoneyVoice PRIVATE ole32 sapi)
endif()

if(HONEYVOICE_WITH_WHISPER AND TARGET whisper)
    target_link_libraries(HoneyVoice PRIVATE whisper)
endif()

if(HONEYVOICE_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(HoneyVoiceTests
        PRODUCT_NAME "HoneyVoiceTests"
    )
    juce_generate_juce_header(HoneyVoiceTests)

    target_sources(HoneyVoiceTests PRIVATE
        Tests/HoneyVoiceTests.cpp
        ${HONEYVOICE_SOURCES}
    )

    target_include_directories(HoneyVoiceTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    )

    target_compile_definitions(HoneyVoiceTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1
        HONEYVOICE_WITH_WHISPER=$<BOOL:${HONEYVOICE_WITH_WHISPER}>
    )

    target_link_libraries(HoneyVoiceTests PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )

    if(WIN32)
        target_link_libraries(HoneyVoiceTests PRIVATE ole32 sapi)
    endif()

    if(HONEYVOICE_WITH_WHISPER AND TARGET whisper)
        target_link_libraries(HoneyVoiceTests PRIVATE whisper)
    endif()

    add_test(NAME HoneyVoiceUnitTests COMMAND HoneyVoiceTests)
endif()
