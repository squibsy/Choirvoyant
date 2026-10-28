#include "Params.h"
#include "HarmonyEngine.h"

namespace Params
{
juce::StringArray keyNames() { return { "Auto", "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" }; }

juce::StringArray lookaheadNames() { return { "Live (30 ms)", "250 ms", "500 ms", "1 s", "2 s" }; }

double lookaheadSeconds (int index)
{
    static const double s[] = { 0.0, 0.25, 0.5, 1.0, 2.0 };   // 0 = live mode
    return s[juce::jlimit (0, 4, index)];
}

namespace
{
    template <typename NameFn>
    juce::StringArray names (int count, NameFn fn)
    {
        juce::StringArray a;
        for (int i = 0; i < count; ++i)
            a.add (fn (i));
        return a;
    }
}

namespace
{
    using APF = juce::AudioParameterFloat;
    using API = juce::AudioParameterInt;
    using APC = juce::AudioParameterChoice;
    using Attr = juce::AudioParameterFloatAttributes;

    juce::ParameterID pid (const juce::String& s) { return { s, 1 }; }

    juce::NormalisableRange<float> range (float lo, float hi, float step = 0.0f) { return { lo, hi, step }; }

    juce::NormalisableRange<float> skewed (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    Attr withText (std::function<juce::String (float)> f)
    {
        return Attr().withStringFromValueFunction ([f] (float v, int) { return f (v); });
    }

    Attr dbText (float floorDb, const char* floorText)
    {
        return withText ([=] (float v) { return v <= floorDb ? juce::String (floorText) : juce::String (v, 1) + " dB"; });
    }

    Attr msText() { return withText ([] (float v) { return juce::String (juce::roundToInt (v)) + " ms"; }); }
    Attr pctText() { return withText ([] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; }); }
    Attr centsText() { return withText ([] (float v) { return juce::String (juce::roundToInt (v)) + " ct"; }); }

    Attr panText()
    {
        return withText ([] (float v) {
            const int p = juce::roundToInt (v * 100.0f);
            return p == 0 ? juce::String ("C") : juce::String (std::abs (p)) + (p < 0 ? " L" : " R");
        });
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    const auto intervals = names (Intervals::count, Intervals::name);
    const auto positions = names (Positions::count, Positions::name);
    const auto motions = names (Motions::count, Motions::name);

    // defaults: a chord tone above and below the lead, and a held bass; in Intervals mode 3rd up, 4th down, octave down
    const int defInterval[3] = { 2, 8, 11 };
    const int defPosition[3] = { Positions::Above, Positions::Below, Positions::Bass };
    const int defMotion[3] = { Motions::Follow, Motions::Follow, Motions::Hold };
    const float defPan[3] = { 0.35f, -0.35f, 0.0f };
    const float defDelay[3] = { 0.0f, 12.0f, 6.0f };

    for (int v = 0; v < 3; ++v)
    {
        const juce::String n = "V" + juce::String (v + 1) + " ";
        auto group = std::make_unique<juce::AudioProcessorParameterGroup> ("voice" + juce::String (v + 1), "Voice " + juce::String (v + 1), "|");

        group->addChild (std::make_unique<APC> (pid (vid (v, interval)), n + "Interval", intervals, defInterval[v]));
        group->addChild (std::make_unique<APC> (pid (vid (v, position)), n + "Position", positions, defPosition[v]));
        group->addChild (std::make_unique<APC> (pid (vid (v, motion)), n + "Motion", motions, defMotion[v]));
        group->addChild (std::make_unique<APF> (pid (vid (v, level)), n + "Level", range (-48.0f, 6.0f, 0.1f), -4.0f, dbText (-47.9f, "Off")));
        group->addChild (std::make_unique<APF> (pid (vid (v, pan)), n + "Pan", range (-1.0f, 1.0f, 0.01f), defPan[v], panText()));
        group->addChild (std::make_unique<APF> (pid (vid (v, delay)), n + "Delay", skewed (0.0f, 250.0f, 40.0f), defDelay[v], msText()));
        group->addChild (std::make_unique<APF> (pid (vid (v, formant)), n + "Formant", range (-6.0f, 6.0f, 0.1f), 0.0f,
                                                withText ([] (float x) { return (x > 0.0f ? "+" : "") + juce::String (x, 1) + " st"; })));
        layout.add (std::move (group));
    }

    auto global = std::make_unique<juce::AudioProcessorParameterGroup> ("global", "Global", "|");
    global->addChild (std::make_unique<APC> (pid (key), "Key", keyNames(), 0));
    global->addChild (std::make_unique<APC> (pid (mode), "Mode", juce::StringArray { "Auto", "Major", "Minor" }, 0));
    global->addChild (std::make_unique<juce::AudioParameterBool> (pid (keyLock), "Key Lock", false));
    global->addChild (std::make_unique<APC> (pid (lookahead), "Lookahead", lookaheadNames(), 3));
    global->addChild (std::make_unique<APC> (pid (harmony), "Harmony Mode", names (HarmonyModes::count, HarmonyModes::name),
                                             HarmonyModes::ChordsAuto));
    global->addChild (std::make_unique<APC> (pid (singOn), "Sing On", names (SingModes::count, SingModes::name), SingModes::All));
    global->addChild (std::make_unique<APF> (pid (longNote), "Long Note", skewed (150.0f, 1500.0f, 400.0f), 400.0f, msText()));
    global->addChild (std::make_unique<API> (pid (voices), "Voices", 1, 3, 2));
    global->addChild (std::make_unique<APF> (pid (dry), "Dry", range (-60.0f, 6.0f, 0.1f), 0.0f, dbText (-59.9f, "Off")));
    global->addChild (std::make_unique<APF> (pid (wet), "Harmony", range (-60.0f, 6.0f, 0.1f), 0.0f, dbText (-59.9f, "Off")));
    layout.add (std::move (global));

    auto character = std::make_unique<juce::AudioProcessorParameterGroup> ("character", "Character", "|");
    character->addChild (std::make_unique<APF> (pid (entryLag), "Entry Lag", range (0.0f, 150.0f, 1.0f), 25.0f, msText()));
    character->addChild (std::make_unique<APF> (pid (humanize), "Humanize", range (0.0f, 1.0f, 0.01f), 0.3f, pctText()));
    character->addChild (std::make_unique<APF> (pid (autoFormant), "Auto Formant", range (0.0f, 1.0f, 0.01f), 0.3f, pctText()));
    character->addChild (std::make_unique<APF> (pid (tightness), "Tightness", range (0.0f, 1.0f, 0.01f), 0.0f, pctText()));
    character->addChild (std::make_unique<APF> (pid (minNote), "Min Note", range (0.0f, 250.0f, 1.0f), 80.0f, msText()));
    character->addChild (std::make_unique<APF> (pid (consonants), "Consonants", range (0.0f, 1.0f, 0.01f), 0.7f,
                                                withText ([] (float v) { return "-" + juce::String (juce::roundToInt (v * 100.0f)) + "%"; })));
    layout.add (std::move (character));

    auto vibrato = std::make_unique<juce::AudioProcessorParameterGroup> ("vibrato", "Vibrato", "|");
    vibrato->addChild (std::make_unique<APF> (pid (vibDepth), "Vibrato Depth", range (0.0f, 80.0f, 1.0f), 20.0f, centsText()));
    vibrato->addChild (std::make_unique<APF> (pid (vibRate), "Vibrato Rate", range (3.0f, 8.0f, 0.01f), 5.5f,
                                              withText ([] (float v) { return juce::String (v, 2) + " Hz"; })));
    vibrato->addChild (std::make_unique<APF> (pid (vibDelay), "Vibrato Delay", range (0.0f, 800.0f, 1.0f), 250.0f, msText()));
    layout.add (std::move (vibrato));

    auto stackGroup = std::make_unique<juce::AudioProcessorParameterGroup> ("stack", "Stack", "|");
    stackGroup->addChild (std::make_unique<API> (pid (stack), "Stack", 1, 3, 1));
    stackGroup->addChild (std::make_unique<APF> (pid (stackDetune), "Stack Detune", range (0.0f, 30.0f, 0.1f), 8.0f, centsText()));
    stackGroup->addChild (std::make_unique<APF> (pid (stackWidth), "Stack Width", range (0.0f, 1.0f, 0.01f), 0.5f, pctText()));
    layout.add (std::move (stackGroup));

    return layout;
}
}
