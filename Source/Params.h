#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace Params
{
    // ---- Voice parameters: prefixed "v1_", "v2_", "v3_" ----
    inline juce::String vid (int voice, const char* name) { return "v" + juce::String (voice + 1) + "_" + name; }

    constexpr const char* interval = "interval";
    constexpr const char* level    = "level";
    constexpr const char* pan      = "pan";
    constexpr const char* delay    = "delay";
    constexpr const char* formant  = "formant";

    // ---- Global ----
    constexpr const char* key         = "key";
    constexpr const char* lookahead   = "lookahead";
    constexpr const char* voices      = "voices";
    constexpr const char* stack       = "stack";
    constexpr const char* stackDetune = "stackdetune";
    constexpr const char* stackWidth  = "stackwidth";
    constexpr const char* entryLag    = "entrylag";
    constexpr const char* humanize    = "humanize";
    constexpr const char* autoFormant = "autoformant";
    constexpr const char* tightness   = "tightness";
    constexpr const char* minNote     = "minnote";
    constexpr const char* vibDepth    = "vibdepth";
    constexpr const char* vibRate     = "vibrate";
    constexpr const char* vibDelay    = "vibdelay";
    constexpr const char* consonants  = "consonants";
    constexpr const char* dry         = "dry";
    constexpr const char* wet         = "wet";

    juce::StringArray keyNames();       // "Auto", then the 12 scales
    juce::StringArray lookaheadNames();
    double lookaheadSeconds (int index);

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
}
