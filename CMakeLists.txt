cmake_minimum_required(VERSION 3.22)

project(Choirvoyant VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(libs/JUCE)

# ---------------------------------------------------------------------------
# DSP engine: plain C++, no JUCE, so the test tool can drive it directly
# ---------------------------------------------------------------------------
add_library(HarmonyEngine STATIC
    Source/Engine/HarmonyEngine.cpp
    Source/Engine/PitchTracker.cpp
    Source/Engine/KeyFinder.cpp)
target_include_directories(HarmonyEngine PUBLIC Source/Engine)
set_target_properties(HarmonyEngine PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(HarmonyEngine PRIVATE /W4 /O2 /fp:fast)
endif()

# ---------------------------------------------------------------------------
# The plugin
# ---------------------------------------------------------------------------
juce_add_plugin(Choirvoyant
    COMPANY_NAME "Bananalytics"
    BUNDLE_ID "ie.bananalytics.choirvoyant"
    PLUGIN_MANUFACTURER_CODE Bnly
    PLUGIN_CODE Chv1
    PRODUCT_NAME "Choirvoyant"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Pitch Shift
    FORMATS VST3)

target_sources(Choirvoyant PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Params.cpp)

target_compile_definitions(Choirvoyant PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Choirvoyant
    PRIVATE
        HarmonyEngine
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Offline test tool: drives the engine on synthetic vocals and smoke-tests the VST3
# ---------------------------------------------------------------------------
option(CHOIRVOYANT_BUILD_TESTS "Build the EngineTest tool" ON)
if(CHOIRVOYANT_BUILD_TESTS)
    juce_add_console_app(EngineTest PRODUCT_NAME "EngineTest")
    target_sources(EngineTest PRIVATE tools/EngineTest/Main.cpp)
    target_compile_definitions(EngineTest PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(EngineTest PRIVATE
        HarmonyEngine
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_recommended_config_flags)
endif()
