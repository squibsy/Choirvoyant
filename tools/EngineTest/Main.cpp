// Offline test tool.
//   EngineTest                      synthetic-vocal checks on the engine (key, harmony pitch, CPU)
//   EngineTest --in a.wav           harmonize a WAV with the default settings into renders/
//   EngineTest --vst3 path.vst3     load the built plugin and render the synthetic vocal through it
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdio>

#include "HarmonyEngine.h"

namespace
{
    constexpr double sr = 48000.0;
    constexpr double pi = 3.14159265358979323846;

    struct SungNote { int midi; double start, dur; bool sibilantBefore; };

    // Additive "ah" vowel with vibrato, scoops and sibilants between some notes.
    std::vector<float> synthVocal (const std::vector<SungNote>& melody, double totalSec)
    {
        std::vector<float> out ((size_t) (totalSec * sr), 0.0f);
        juce::Random rnd (1234);

        auto formantGain = [] (double hz) {
            const double f[3] = { 730.0, 1090.0, 2440.0 }, bw[3] = { 80.0, 90.0, 120.0 }, g[3] = { 1.0, 0.5, 0.25 };
            double a = 0.02;
            for (int i = 0; i < 3; ++i)
            {
                const double x = (hz - f[i]) / bw[i];
                a += g[i] / (1.0 + x * x);
            }
            return a;
        };

        for (const auto& n : melody)
        {
            const auto s0 = (size_t) (n.start * sr), len = (size_t) (n.dur * sr);
            double phase = 0.0;
            for (size_t i = 0; i < len && s0 + i < out.size(); ++i)
            {
                const double t = (double) i / sr;
                const double scoop = -0.6 * std::exp (-t / 0.04);
                const double vibEnv = juce::jlimit (0.0, 1.0, (t - 0.15) / 0.2);
                const double midi = n.midi + scoop + 0.3 * vibEnv * std::sin (2.0 * pi * 5.2 * t);
                const double f0 = 440.0 * std::pow (2.0, (midi - 69.0) / 12.0);
                phase += 2.0 * pi * f0 / sr;

                double y = 0.0;
                for (int k = 1; k * f0 < 5000.0; ++k)
                    y += formantGain (k * f0) * std::sin (k * phase) / std::pow (k, 0.7);

                const double env = juce::jmin (1.0, t / 0.02) * juce::jmin (1.0, (n.dur - t) / 0.03);
                out[s0 + i] += (float) (0.15 * env * y);
            }

            if (n.sibilantBefore)
            {
                const auto b0 = (size_t) ((n.start - 0.09) * sr), blen = (size_t) (0.08 * sr);
                float prev = 0.0f;
                for (size_t i = 0; i < blen; ++i)
                {
                    const float w = rnd.nextFloat() * 2.0f - 1.0f;
                    const float env = (float) std::sin (pi * (double) i / (double) blen);
                    out[b0 + i] += 0.12f * env * (w - prev);
                    prev = w;
                }
            }
        }
        return out;
    }

    void writeWav (const juce::File& file, const juce::AudioBuffer<float>& buf)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (file), sr,
                                                                        (unsigned) buf.getNumChannels(), 24, {}, 0));
        if (w != nullptr)
            w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    }

    juce::AudioBuffer<float> runEngine (const std::vector<float>& mono, const HarmonySettings& s, double lookahead,
                                        double* cpuPercent = nullptr, int* rootOut = nullptr)
    {
        HarmonyEngine engine;
        engine.prepare (sr, lookahead);
        const int lat = engine.getLatencySamples();
        const int total = (int) mono.size() + lat;

        juce::AudioBuffer<float> out (2, total);
        std::vector<float> in ((size_t) total, 0.0f);
        std::copy (mono.begin(), mono.end(), in.begin());

        const auto t0 = juce::Time::getHighResolutionTicks();
        for (int pos = 0; pos < total; pos += 256)
        {
            const int n = juce::jmin (256, total - pos);
            const float* ins[1] = { in.data() + pos };
            float* outs[2] = { out.getWritePointer (0, pos), out.getWritePointer (1, pos) };
            engine.process (ins, 1, outs, 2, n, s);
        }
        const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
        if (cpuPercent != nullptr)
            *cpuPercent = 100.0 * secs / ((double) total / sr);
        if (rootOut != nullptr)
            *rootOut = engine.statusRoot.load();

        // drop the latency so output lines up with the input
        juce::AudioBuffer<float> aligned (2, (int) mono.size());
        for (int c = 0; c < 2; ++c)
            aligned.copyFrom (c, 0, out, c, lat, (int) mono.size());
        return aligned;
    }

    float medianPitch (const float* x, int len)
    {
        PitchTracker pt;
        pt.prepare (sr, 2048, 60.0f, 1500.0f);
        std::vector<float> got;
        for (int s = 0; s + 2048 <= len; s += 256)
        {
            const auto r = pt.analyse (x + s);
            if (r.f0 > 0.0f && r.aperiodicity < 0.2f)
                got.push_back (69.0f + 12.0f * std::log2 (r.f0 / 440.0f));
        }
        if (got.empty())
            return 0.0f;
        std::sort (got.begin(), got.end());
        return got[got.size() / 2];
    }

    const char* noteName (int midi)
    {
        static const char* n[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return n[((midi % 12) + 12) % 12];
    }

    int diatonicUp (int lead, int root, int steps)
    {
        static const int scale[7] = { 0, 2, 4, 5, 7, 9, 11 };
        const int rel = ((lead - root) % 12 + 12) % 12;
        int deg = 0;
        for (int d = 0; d < 7; ++d)
            if (scale[d] == rel)
                deg = d;
        const int d2 = deg + steps;
        return lead - rel + scale[((d2 % 7) + 7) % 7] + 12 * (int) std::floor (d2 / 7.0);
    }

    std::vector<SungNote> testMelody (int transpose, double& totalSec)
    {
        // G major: G A B C D B G E F# G, mixed lengths, a quick passing note and a few sibilants
        const int base[] = { 67, 69, 71, 72, 74, 71, 67, 64, 66, 67 };
        const double dur[] = { 0.5, 0.45, 0.6, 0.45, 0.8, 0.07, 0.5, 0.5, 0.45, 0.9 };
        const bool sib[] = { false, false, true, false, false, false, true, false, true, false };
        std::vector<SungNote> m;
        double t = 0.3;
        for (int i = 0; i < 10; ++i)
        {
            if (sib[i])
                t += 0.1;
            m.push_back ({ base[i] + transpose, t, dur[i], sib[i] });
            t += dur[i] + (dur[i] < 0.1 ? 0.0 : 0.06);
        }
        totalSec = t + 0.5;
        return m;
    }

    int runChecks()
    {
        int failures = 0;
        const juce::File renders = juce::File::getCurrentWorkingDirectory().getChildFile ("renders");

        for (int transpose : { 0, -5, 3 })
        {
            double totalSec = 0.0;
            const auto melody = testMelody (transpose, totalSec);
            const auto vocal = synthVocal (melody, totalSec);
            const int root = ((7 + transpose) % 12 + 12) % 12;

            for (int interval : { 2, 4, 7 })   // 3rd up, 5th up, 3rd down
            {
                HarmonySettings s;
                s.voices = 1;
                s.voice[0].interval = interval;
                s.voice[0].levelDb = 0.0f;
                s.dryDb = -60.0f;
                s.entryLagMs = 0.0f;
                s.humanize = 0.0f;
                s.vibDepthCents = 0.0f;
                s.consonants = 0.0f;

                int detected = -1;
                const auto out = runEngine (vocal, s, 1.0, nullptr, &detected);
                std::printf ("transpose %+d  %-9s  key %s (expected %s)\n", transpose, Intervals::name (interval),
                             detected >= 0 ? noteName (detected) : "?", noteName (root));
                if (detected != root)
                    ++failures;

                int bad = 0;
                for (size_t i = 0; i < melody.size(); ++i)
                {
                    const auto& n = melody[i];
                    if (n.dur < 0.1)
                        continue;   // passing note holds the previous harmony note by design
                    const int a = (int) ((n.start + 0.08) * sr), b = (int) ((n.start + n.dur - 0.04) * sr);
                    const float got = medianPitch (out.getReadPointer (0) + a, b - a);
                    const int want = diatonicUp (n.midi, root, Intervals::steps (interval));
                    const bool ok = std::abs (got - (float) want) < 0.5f;
                    if (! ok)
                    {
                        ++bad;
                        std::printf ("    note %zu lead %s%d  want %s%d  got %.2f\n", i, noteName (n.midi), n.midi / 12 - 1,
                                     noteName (want), want / 12 - 1, got);
                        std::printf ("      track:");
                        for (int p = a; p + 4800 <= b; p += 4800)
                            std::printf (" %.1f", medianPitch (out.getReadPointer (0) + p, 4800));
                        std::printf ("\n");
                    }
                }
                std::printf ("    %s: %d of %zu notes off\n", bad == 0 ? "ok" : "FAIL", bad, melody.size() - 1);
                failures += bad;

                if (transpose == 0)
                {
                    juce::AudioBuffer<float> both (2, (int) vocal.size());
                    for (int c = 0; c < 2; ++c)
                        for (int k = 0; k < both.getNumSamples(); ++k)
                            both.setSample (c, k, 0.7f * vocal[(size_t) k] + 0.7f * out.getSample (c, k));
                    writeWav (renders.getChildFile (juce::String ("synth_") + Intervals::name (interval) + ".wav"), both);
                }
            }
        }

        // key change: 8 notes of G major, then 8 notes of Eb major
        {
            const int g[] = { 67, 71, 74, 72, 71, 69, 66, 67 }, eb[] = { 63, 67, 70, 68, 67, 65, 62, 63 };
            std::vector<SungNote> m;
            double t = 0.3;
            for (int i = 0; i < 16; ++i)
            {
                m.push_back ({ i < 8 ? g[i] : eb[i - 8], t, 0.4, false });
                t += 0.46;
            }
            const auto vocal = synthVocal (m, t + 0.5);
            HarmonySettings s;
            s.voices = 1;
            s.voice[0].levelDb = 0.0f;
            s.dryDb = -60.0f;
            s.entryLagMs = s.humanize = s.vibDepthCents = s.consonants = 0.0f;
            const auto out = runEngine (vocal, s, 1.0);

            std::printf ("key change G -> Eb, 3rd up:\n    ");
            int wrongAfter = 0;
            for (int i = 0; i < 16; ++i)
            {
                const auto& n = m[(size_t) i];
                const int a = (int) ((n.start + 0.08) * sr), b = (int) ((n.start + n.dur - 0.04) * sr);
                const float got = medianPitch (out.getReadPointer (0) + a, b - a);
                const int want = diatonicUp (n.midi, i < 8 ? 7 : 3, 2);
                const bool ok = std::abs (got - (float) want) < 0.5f;
                std::printf ("%s%s ", noteName (juce::roundToInt (got)), ok ? "" : "*");
                if (! ok)
                    ++wrongAfter;
            }
            std::printf ("\n    %d of 16 notes in the wrong key\n", wrongAfter);
            if (wrongAfter > 1)
                ++failures;
        }

        // full patch: 3 voices x 3 stack, every feature on
        {
            double totalSec = 0.0;
            const auto melody = testMelody (0, totalSec);
            const auto vocal = synthVocal (melody, totalSec);
            HarmonySettings s;
            s.voices = 3;
            s.voice[0].interval = 2;
            s.voice[1].interval = 8;  s.voice[1].pan = -0.6f; s.voice[1].delayMs = 20.0f;
            s.voice[2].interval = 11; s.voice[2].pan = 0.6f;  s.voice[2].formantSemis = -1.0f;
            s.stack = 3;
            double cpu = 0.0;
            const auto out = runEngine (vocal, s, 1.0, &cpu);
            float peak = out.getMagnitude (0, out.getNumSamples());
            std::printf ("full patch (9 PSOLA voices): %.2f%% of one core, peak %.2f\n", cpu, peak);
            if (! std::isfinite (peak) || peak > 4.0f)
                ++failures;
            juce::AudioBuffer<float> mix (2, out.getNumSamples());
            for (int c = 0; c < 2; ++c)
                mix.copyFrom (c, 0, out, c, 0, out.getNumSamples());
            writeWav (renders.getChildFile ("synth_full.wav"), mix);
        }

        // white noise and silence must not blow up
        {
            std::vector<float> noise ((size_t) (sr * 3.0));
            juce::Random r (7);
            for (auto& x : noise)
                x = 0.3f * (r.nextFloat() * 2.0f - 1.0f);
            std::fill (noise.begin() + (int) sr, noise.begin() + (int) (2 * sr), 0.0f);
            HarmonySettings s;
            s.voices = 3;
            s.stack = 3;
            const auto out = runEngine (noise, s, 0.25);
            const float peak = out.getMagnitude (0, out.getNumSamples());
            std::printf ("noise/silence: peak %.2f\n", peak);
            if (! std::isfinite (peak) || peak > 4.0f)
                ++failures;
        }

        std::printf (failures == 0 ? "\nALL CHECKS PASSED\n" : "\n%d FAILURES\n", failures);
        return failures == 0 ? 0 : 1;
    }

    int harmonizeFile (const juce::File& file)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr)
        {
            std::printf ("cannot read %s\n", file.getFullPathName().toRawUTF8());
            return 1;
        }
        const double rate = reader->sampleRate;
        const bool pitchOnly = std::getenv ("CHV_PITCH") != nullptr;
        juce::AudioBuffer<float> buf ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&buf, 0, buf.getNumSamples(), 0, true, true);
        std::vector<float> mono ((size_t) buf.getNumSamples());
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            float acc = 0.0f;
            for (int c = 0; c < buf.getNumChannels(); ++c)
                acc += buf.getSample (c, i);
            mono[(size_t) i] = acc / (float) buf.getNumChannels();
        }

        std::printf ("%s: %.1f s, %.0f Hz, %d ch\n", file.getFileName().toRawUTF8(), buf.getNumSamples() / rate, rate, buf.getNumChannels());

        if (pitchOnly)   // debug: raw pitch track of the input, one value per 100 ms (MIDI, 0 = unvoiced)
        {
            PitchTracker pt;
            int len = 1;
            while (len < rate * 0.042) len <<= 1;
            pt.prepare (rate, len, 70.0f, 1100.0f);
            const int step = (int) (rate * 0.1);
            for (int p = 0, k = 0; p + len <= (int) mono.size() && k < 300; p += step, ++k)
            {
                const auto r = pt.analyse (mono.data() + p);
                const bool v = r.f0 > 0.0f && r.aperiodicity < 0.25f && r.rms > 0.002f;
                std::printf ("%6.2f%s", v ? 69.0 + 12.0 * std::log2 (r.f0 / 440.0) : 0.0, k % 10 == 9 ? "\n" : " ");
            }
            return 0;
        }

        // default settings, rendered twice: full mix, and the harmonies on their own
        for (int pass = 0; pass < 2; ++pass)
        {
            HarmonySettings s;
            if (const char* k = std::getenv ("CHV_KEY"))   // 0 = auto, 1..12 = fixed scale on C..B
                s.keyMode = std::atoi (k);
            if (pass == 1)
                s.dryDb = -60.0f;

            HarmonyEngine engine;
            engine.prepare (rate, 1.0);
            const int lat = engine.getLatencySamples();
            const int total = (int) mono.size() + lat;
            std::vector<float> in ((size_t) total, 0.0f);
            std::copy (mono.begin(), mono.end(), in.begin());
            juce::AudioBuffer<float> out (2, total);

            const int block = 512;
            const int reportEvery = (int) (rate * 2.0) / block;
            juce::String lastKey;
            const auto t0 = juce::Time::getHighResolutionTicks();
            for (int pos = 0, b = 0; pos < total; pos += block, ++b)
            {
                const int n = juce::jmin (block, total - pos);
                const float* ins[1] = { in.data() + pos };
                float* outs[2] = { out.getWritePointer (0, pos), out.getWritePointer (1, pos) };
                engine.process (ins, 1, outs, 2, n, s);

                // timeline of what the engine decided, in input time (output time minus latency)
                if (pass == 0 && b % reportEvery == 0 && pos >= lat)
                {
                    const int root = engine.statusRoot.load();
                    const juce::String key = root < 0 ? "?" : juce::String (noteName (root)) + (engine.statusMinor.load() ? "m" : "");
                    const float lead = engine.statusLeadMidi.load(), h0 = engine.statusTarget[0].load(), h1 = engine.statusTarget[1].load();
                    std::printf ("  %5.1f s  tune %+3d ct  key %-3s lead %-4s harmony %-4s %-4s%s\n", (pos - lat) / rate,
                                 juce::roundToInt (engine.statusTuning.load() * 100.0f), key.toRawUTF8(),
                                 lead > 0 ? (juce::String (noteName (juce::roundToInt (lead))) + juce::String (juce::roundToInt (lead) / 12 - 1)).toRawUTF8() : "-",
                                 h0 > 0 ? (juce::String (noteName (juce::roundToInt (h0))) + juce::String (juce::roundToInt (h0) / 12 - 1)).toRawUTF8() : "-",
                                 h1 > 0 ? (juce::String (noteName (juce::roundToInt (h1))) + juce::String (juce::roundToInt (h1) / 12 - 1)).toRawUTF8() : "-",
                                 key != lastKey && lastKey.isNotEmpty() ? "   <- key change" : "");
                    lastKey = key;
                }
            }
            const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);

            if (pass == 0 && std::getenv ("CHV_NOTES") != nullptr)
            {
                int k = 0;
                engine.forEachNote ([&k] (double a, double b, float midi, int name) {
                    if (k++ < 80)
                        std::printf ("  note %3d  %6.2f-%6.2f s  %6.2f  -> %s%d\n", k, a, b, midi, noteName (name), name / 12 - 1);
                });
            }

            juce::AudioBuffer<float> aligned (2, (int) mono.size());
            for (int c = 0; c < 2; ++c)
                aligned.copyFrom (c, 0, out, c, lat, (int) mono.size());

            // a fixed-point WAV clips above full scale, so turn the render down if it would
            const float peak = aligned.getMagnitude (0, aligned.getNumSamples());
            if (peak > 0.98f)
                aligned.applyGain (0.98f / peak);

            const char* tag = std::getenv ("CHV_TAG");
            const auto dest = juce::File::getCurrentWorkingDirectory().getChildFile ("renders")
                                  .getChildFile (file.getFileNameWithoutExtension() + (tag != nullptr ? juce::String ("_") + tag : juce::String())
                                                 + (pass == 0 ? "_mix.wav" : "_harmonies.wav"));
            dest.getParentDirectory().createDirectory();
            dest.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (dest), rate, 2, 24, {}, 0));
            if (w != nullptr)
                w->writeFromAudioSampleBuffer (aligned, 0, aligned.getNumSamples());
            std::printf ("wrote %s  (peak %.2f%s, %.1f%% CPU)\n", dest.getFullPathName().toRawUTF8(), peak,
                         peak > 0.98f ? ", scaled to 0.98" : "", 100.0 * secs / (total / rate));
        }
        return 0;
    }

    int smokeTestPlugin (const juce::File& file)
    {
        juce::AudioPluginFormatManager fm;
        fm.addFormat (std::make_unique<juce::VST3PluginFormat>());
        juce::OwnedArray<juce::PluginDescription> types;
        juce::VST3PluginFormat vst3;
        vst3.findAllTypesForFile (types, file.getFullPathName());
        if (types.isEmpty())
        {
            std::printf ("no plugin found in %s\n", file.getFullPathName().toRawUTF8());
            return 1;
        }

        juce::String err;
        auto plugin = fm.createPluginInstance (*types[0], sr, 256, err);
        if (plugin == nullptr)
        {
            std::printf ("load failed: %s\n", err.toRawUTF8());
            return 1;
        }
        plugin->setPlayConfigDetails (2, 2, sr, 256);
        plugin->prepareToPlay (sr, 256);
        std::printf ("loaded %s, %d parameters, latency %d samples\n", plugin->getName().toRawUTF8(),
                     plugin->getParameters().size(), plugin->getLatencySamples());

        double totalSec = 0.0;
        const auto melody = testMelody (0, totalSec);
        const auto vocal = synthVocal (melody, totalSec);
        const int lat = plugin->getLatencySamples();
        const int total = (int) vocal.size() + lat;
        juce::AudioBuffer<float> out (2, total), block (2, 256);
        juce::MidiBuffer midi;
        for (int pos = 0; pos < total; pos += 256)
        {
            const int n = juce::jmin (256, total - pos);
            block.setSize (2, n, false, false, true);
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < n; ++k)
                    block.setSample (c, k, pos + k < (int) vocal.size() ? vocal[(size_t) (pos + k)] : 0.0f);
            plugin->processBlock (block, midi);
            for (int c = 0; c < 2; ++c)
                out.copyFrom (c, pos, block, c, 0, n);
        }
        plugin->releaseResources();

        const float peak = out.getMagnitude (0, total);
        writeWav (juce::File::getCurrentWorkingDirectory().getChildFile ("renders").getChildFile ("vst3_default.wav"), out);
        std::printf ("rendered through VST3, peak %.2f\n", peak);
        return std::isfinite (peak) && peak > 0.01f ? 0 : 1;
    }
}

#ifdef _WIN32
 #include <windows.h>

// Shows the plugin editor, sings into it for a few seconds and saves a screen grab of the window.
static int captureEditor (const juce::File& file, const juce::File& png)
{
    juce::AudioPluginFormatManager fm;
    fm.addFormat (std::make_unique<juce::VST3PluginFormat>());
    juce::OwnedArray<juce::PluginDescription> types;
    juce::VST3PluginFormat().findAllTypesForFile (types, file.getFullPathName());
    juce::String err;
    auto plugin = types.isEmpty() ? nullptr : fm.createPluginInstance (*types[0], sr, 480, err);
    if (plugin == nullptr)
        return 1;
    plugin->setPlayConfigDetails (2, 2, sr, 480);
    plugin->prepareToPlay (sr, 480);

    juce::DocumentWindow window ("Choirvoyant UI test", juce::Colours::black, 0);
    window.setUsingNativeTitleBar (true);
    window.setContentOwned (plugin->createEditorAndMakeActive(), true);
    window.setTopLeftPosition (20, 20);
    window.setAlwaysOnTop (true);
    window.setVisible (true);
    window.toFront (true);

    double totalSec = 0.0;
    const auto melody = testMelody (0, totalSec);
    const auto vocal = synthVocal (melody, totalSec);
    struct Feeder : juce::Timer
    {
        juce::AudioPluginInstance& p; const std::vector<float>& v; size_t pos = 0;
        Feeder (juce::AudioPluginInstance& pp, const std::vector<float>& vv) : p (pp), v (vv) { startTimer (10); }
        void timerCallback() override
        {
            juce::AudioBuffer<float> b (2, 480);
            juce::MidiBuffer m;
            for (int k = 0; k < 480; ++k, ++pos)
                for (int c = 0; c < 2; ++c)
                    b.setSample (c, k, v[pos % v.size()]);
            p.processBlock (b, m);
        }
    } feeder (*plugin, vocal);

    juce::MessageManager::getInstance()->runDispatchLoopUntil (4500);

    auto* hwnd = (HWND) window.getPeer()->getNativeHandle();
    RECT rc;
    GetClientRect (hwnd, &rc);
    POINT origin { 0, 0 };
    ClientToScreen (hwnd, &origin);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;

    HDC screen = GetDC (nullptr), mem = CreateCompatibleDC (screen);
    HBITMAP bmp = CreateCompatibleBitmap (screen, w, h);
    SelectObject (mem, bmp);
    BitBlt (mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY);

    BITMAPINFO bi {};
    bi.bmiHeader = { sizeof (BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    juce::Image img (juce::Image::ARGB, w, h, true);
    {
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        std::vector<uint8_t> px ((size_t) w * h * 4);
        GetDIBits (mem, bmp, 0, (UINT) h, px.data(), &bi, DIB_RGB_COLORS);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const uint8_t* p = &px[((size_t) y * w + x) * 4];
                bd.setPixelColour (x, y, juce::Colour (p[2], p[1], p[0]));
            }
    }
    DeleteObject (bmp);
    DeleteDC (mem);
    ReleaseDC (nullptr, screen);

    png.deleteFile();
    juce::FileOutputStream os (png);
    juce::PNGImageFormat().writeImageToStream (img, os);
    std::printf ("saved %s (%dx%d)\n", png.getFullPathName().toRawUTF8(), w, h);
    return 0;
}
#endif

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
   #ifdef _WIN32
    if (argc > 3 && juce::String (argv[1]) == "--ui")
        return captureEditor (juce::File (juce::String (argv[2])), juce::File (juce::String (argv[3])));
   #endif
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (juce::String (argv[i]) == "--in")
            return harmonizeFile (juce::File (juce::String (argv[i + 1])));
        if (juce::String (argv[i]) == "--vst3")
            return smokeTestPlugin (juce::File (juce::String (argv[i + 1])));
    }
    return runChecks();
}
