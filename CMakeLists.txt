cmake_minimum_required(VERSION 3.22)

# CI passes -DBBR_BUILD_NUMBER=<github run number>; every release is 1.0.<build number>.
set(BBR_BUILD_NUMBER 0 CACHE STRING "Build number, becomes the patch version")
project(BeatboxReplacer VERSION 1.0.${BBR_BUILD_NUMBER} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Link the MSVC runtime statically so the Windows plugin needs no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
endif()

option(BBR_BUILD_PLUGIN "Build the JUCE plugin (downloads JUCE)" ON)
option(BBR_BUILD_TESTS "Build the core unit tests" ON)
option(BBR_COPY_AFTER_BUILD "Copy the built plugin into the system plugin folder (local dev only)" OFF)
# Alternative bus layout (sidechain as the only input, reported as aux). Off = main input + sidechain.
option(BBR_AUX_ONLY_SIDECHAIN "Declare the sidechain as the only input bus" OFF)

# Where the updater looks for releases. Change these if the repo moves.
set(BBR_GITHUB_OWNER "dylancleverdon" CACHE STRING "GitHub owner of the release repo")
set(BBR_GITHUB_REPO "beatbox-replacer" CACHE STRING "GitHub repo with the releases")
# Only needed if the repo is ever made private: a read-only token baked into the binary.
set(BBR_GITHUB_TOKEN "" CACHE STRING "Optional GitHub token for private release repos")

# ------------------------------------------------------------------------------------------------
# JUCE-free analysis core (unit-tested on every platform)
# ------------------------------------------------------------------------------------------------
add_library(bbr_core STATIC
    Source/core/Analysis.cpp
    Source/core/Capture.cpp
    Source/core/Classifier.cpp
    Source/core/Clustering.cpp
    Source/core/FeatureExtractor.cpp
    Source/core/Fft.cpp
    Source/core/LearnSession.cpp
    Source/core/LiveEngine.cpp
    Source/core/MidiFile.cpp
    Source/core/OnsetDetector.cpp
    Source/core/Types.cpp
    Source/core/Version.cpp)
target_include_directories(bbr_core PUBLIC Source)
if(MSVC)
    target_compile_options(bbr_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(bbr_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BBR_BUILD_TESTS)
    enable_testing()
    add_executable(bbr_core_tests tests/CoreTests.cpp tests/TestSignals.cpp)
    target_link_libraries(bbr_core_tests PRIVATE bbr_core)
    add_test(NAME bbr_core_tests COMMAND bbr_core_tests)
endif()

# ------------------------------------------------------------------------------------------------
# The plugin
# ------------------------------------------------------------------------------------------------
if(BBR_BUILD_PLUGIN)
    include(FetchContent)
    # To build offline, pass -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE)
    FetchContent_MakeAvailable(JUCE)

    set(BBR_FORMATS VST3)
    if(APPLE)
        list(APPEND BBR_FORMATS AU)
    endif()

    juce_add_plugin(BeatboxReplacer
        PRODUCT_NAME "BeatboxReplacer"
        COMPANY_NAME "Dylan Cleverdon"
        BUNDLE_ID "com.dylancleverdon.beatboxreplacer"
        PLUGIN_MANUFACTURER_CODE Dycl
        PLUGIN_CODE Bbxr
        FORMATS ${BBR_FORMATS}
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        VST3_CATEGORIES Instrument Drum
        AU_MAIN_TYPE kAudioUnitType_MusicDevice
        COPY_PLUGIN_AFTER_BUILD ${BBR_COPY_AFTER_BUILD})

    juce_generate_juce_header(BeatboxReplacer)

    target_sources(BeatboxReplacer PRIVATE
        Source/plugin/PluginProcessor.cpp
        Source/plugin/PluginEditor.cpp
        Source/plugin/Updater.cpp
        Source/plugin/ProfileIO.cpp
        Source/plugin/ui/LookAndFeel.cpp
        Source/plugin/ui/HeaderBar.cpp
        Source/plugin/ui/LearnTab.cpp
        Source/plugin/ui/PlayTab.cpp
        Source/plugin/ui/SetupTab.cpp
        Source/plugin/ui/WaveformView.cpp
        Source/plugin/ui/SlotsPanel.cpp
        Source/plugin/ui/TakeView.cpp)

    target_compile_definitions(BeatboxReplacer PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        BBR_AUX_ONLY_SIDECHAIN=$<BOOL:${BBR_AUX_ONLY_SIDECHAIN}>
        BBR_GITHUB_OWNER="${BBR_GITHUB_OWNER}"
        BBR_GITHUB_REPO="${BBR_GITHUB_REPO}"
        BBR_GITHUB_TOKEN="${BBR_GITHUB_TOKEN}")

    target_link_libraries(BeatboxReplacer
        PRIVATE
            bbr_core
            juce::juce_audio_utils
            juce::juce_audio_formats
            juce::juce_cryptography
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()
