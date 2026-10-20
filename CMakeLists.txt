# Radio Music (unofficial) - build definition.
# An unofficial, clean-room plugin inspired by Music Thing Modular's Radio Music mk2
# (designed by Tom Whitwell with Chris Johnson of Plus Minus Audio).
cmake_minimum_required(VERSION 3.22)

# These must be set before project(). Universal binary, macOS 12 minimum.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build universal binaries" FORCE)
endif()

project(RadioMusicUnofficial VERSION 0.7.0 LANGUAGES C CXX)

# The version is shown in the About screen.
add_compile_definitions(RM_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(RM_BUILD_TESTS "Build the engine test program" ON)
option(RM_BUILD_SNAPSHOT "Build the developer tool that renders the editor to PNG" OFF)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

set(RM_ENGINE_SOURCES
    Source/Engine/Library.cpp
    Source/Engine/Library.h
    Source/Engine/RadioEngine.cpp
    Source/Engine/RadioEngine.h
    Source/Engine/Path.h)

#------------------------------------------------------------------------------
# The plugin (AU instrument + VST3)
juce_add_plugin(RadioMusicUnofficial
    COMPANY_NAME                "particledots"
    PLUGIN_MANUFACTURER_CODE    Pdts
    PLUGIN_CODE                 Rmux
    FORMATS                     AU VST3
    PRODUCT_NAME                "Radio Music Unofficial"
    PLUGIN_NAME                 "Radio Music Unofficial"
    DESCRIPTION                 "Unofficial sample player inspired by Music Thing Modular Radio Music mk2"
    BUNDLE_ID                   com.particledots.radiomusicunofficial
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    AU_MAIN_TYPE                kAudioUnitType_MusicDevice
    VST3_CATEGORIES             Instrument Sampler
    COPY_PLUGIN_AFTER_BUILD     FALSE
    NEEDS_WEB_BROWSER           FALSE
    NEEDS_CURL                  FALSE)

target_sources(RadioMusicUnofficial PRIVATE
    ${RM_ENGINE_SOURCES}
    Source/Plugin/PluginProcessor.cpp
    Source/Plugin/PluginProcessor.h
    Source/Plugin/PluginEditor.cpp
    Source/Plugin/PluginEditor.h)

target_include_directories(RadioMusicUnofficial PRIVATE Source)

target_compile_definitions(RadioMusicUnofficial PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(RadioMusicUnofficial
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

#------------------------------------------------------------------------------
# Engine test program (generates its own test audio, no files needed)
if(RM_BUILD_TESTS)
    juce_add_console_app(RadioEngineTest
        PRODUCT_NAME "RadioEngineTest"
        NEEDS_WEB_BROWSER FALSE
        NEEDS_CURL FALSE)

    target_sources(RadioEngineTest PRIVATE
        ${RM_ENGINE_SOURCES}
        tests/EngineTest.cpp)

    target_include_directories(RadioEngineTest PRIVATE Source)

    target_compile_definitions(RadioEngineTest PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

    target_link_libraries(RadioEngineTest
        PRIVATE
            juce::juce_audio_formats
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()

#------------------------------------------------------------------------------
# Developer tool: renders the editor to PNG files without a screen (off by default, not used by CI).
if(RM_BUILD_SNAPSHOT)
    juce_add_console_app(EditorSnapshot
        PRODUCT_NAME "EditorSnapshot"
        NEEDS_WEB_BROWSER FALSE
        NEEDS_CURL FALSE)

    target_sources(EditorSnapshot PRIVATE
        ${RM_ENGINE_SOURCES}
        Source/Plugin/PluginProcessor.cpp
        Source/Plugin/PluginEditor.cpp
        tests/EditorSnapshot.cpp)

    target_include_directories(EditorSnapshot PRIVATE Source)

    target_compile_definitions(EditorSnapshot PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

    target_link_libraries(EditorSnapshot
        PRIVATE
            juce::juce_audio_processors
            juce::juce_audio_formats
            juce::juce_gui_basics
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
