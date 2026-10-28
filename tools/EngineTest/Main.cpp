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

    // MIDI for a test run: absolute sample positions
    using MidiTrack = std::vector<HarmonyEngine::MidiEvent>;
    using NoteList = std::vector<HarmonyEngine::NoteInfo>;

    juce::AudioBuffer<float> runEngine (const std::vector<float>& mono, const HarmonySettings& s, double lookahead,
                                        double* cpuPercent = nullptr, int* rootOut = nullptr, const MidiTrack* midi = nullptr,
                                        NoteList* notesOut = nullptr, int* latencyOut = nullptr)
    {
        HarmonyEngine engine;
        engine.prepare (sr, lookahead);
        const int lat = engine.getLatencySamples();
        const int total = (int) mono.size() + lat;
        if (latencyOut != nullptr)
            *latencyOut = lat;

        juce::AudioBuffer<float> out (2, total);
        std::vector<float> in ((size_t) total, 0.0f);
        std::copy (mono.begin(), mono.end(), in.begin());

        MidiTrack block;
        size_t next = 0;
        const auto t0 = juce::Time::getHighResolutionTicks();
        for (int pos = 0; pos < total; pos += 256)
        {
            const int n = juce::jmin (256, total - pos);
            block.clear();
            while (midi != nullptr && next < midi->size() && (*midi)[next].offset < pos + n)
            {
                auto e = (*midi)[next++];
                e.offset = juce::jmax (0, e.offset - pos);
                block.push_back (e);
            }
            const float* ins[1] = { in.data() + pos };
            float* outs[2] = { out.getWritePointer (0, pos), out.getWritePointer (1, pos) };
            engine.process (ins, 1, outs, 2, n, s, block.data(), (int) block.size());
        }
        const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
        if (cpuPercent != nullptr)
            *cpuPercent = 100.0 * secs / ((double) total / sr);
        if (rootOut != nullptr)
            *rootOut = engine.statusRoot.load();
        if (notesOut != nullptr)
        {
            notesOut->clear();
            engine.forEachNote ([notesOut] (const HarmonyEngine::NoteInfo& info) { notesOut->push_back (info); });
        }

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

    //==========================================================================
    // v2 checks: chord modes, MIDI, Sing On, live mode
    //==========================================================================

    // C major, one bar per chord: I vi IV V I. Each bar: two chord tones, a quick passing note, a chord tone.
    struct Bar { int degree; int notes[4]; };
    const Bar progression[] = { { 0, { 64, 67, 69, 67 } }, { 5, { 69, 72, 71, 69 } }, { 3, { 65, 69, 67, 65 } },
                                { 4, { 67, 71, 72, 74 } }, { 0, { 64, 60, 62, 60 } } };
    constexpr double barDur[4] = { 0.45, 0.45, 0.12, 0.6 };

    std::vector<SungNote> progressionMelody (std::vector<int>& barOf, std::vector<double>& barStart, double& totalSec)
    {
        std::vector<SungNote> m;
        double t = 0.3;
        for (int b = 0; b < 5; ++b)
        {
            barStart.push_back (t);
            for (int k = 0; k < 4; ++k)
            {
                m.push_back ({ progression[b].notes[k], t, barDur[k], false });
                barOf.push_back (b);
                t += barDur[k] + (k == 2 ? 0.0 : 0.05);
            }
        }
        barStart.push_back (t);
        totalSec = t + 0.5;
        return m;
    }

    const HarmonyEngine::NoteInfo* findNote (const NoteList& notes, double start)
    {
        const HarmonyEngine::NoteInfo* best = nullptr;
        for (const auto& n : notes)
            if (std::abs (n.start - start) < 0.07 && (best == nullptr || std::abs (n.start - start) < std::abs (best->start - start)))
                best = &n;
        return best;
    }

    int pc (int p) { return ((p % 12) + 12) % 12; }

    HarmonySettings plainVoices (int numVoices)
    {
        HarmonySettings s;
        s.voices = numVoices;
        s.dryDb = -60.0f;
        s.entryLagMs = s.humanize = s.vibDepthCents = s.consonants = 0.0f;
        for (auto& v : s.voice)
            v.levelDb = 0.0f;
        return s;
    }

    int runV2Checks()
    {
        int failures = 0;
        std::vector<int> barOf;
        std::vector<double> barStart;
        double totalSec = 0.0;
        const auto melody = progressionMelody (barOf, barStart, totalSec);
        const auto vocal = synthVocal (melody, totalSec);
        Chords::Chord diatonicC[Chords::numDiatonic];
        Chords::diatonic (0, false, diatonicC);

        // ---- chord choice + voice leading (Chords Auto, 3 voices) ----
        // The key is fixed: the first two bars never sing F or F#, so C and G major fit equally
        // well and key detection (tested above) could take either. This checks the chord choice.
        {
            auto s = plainVoices (3);
            s.harmonyMode = HarmonyModes::ChordsAuto;
            s.keyMode = 1;      // C
            s.scaleMode = 1;    // major
            NoteList notes;
            runEngine (vocal, s, 1.0, nullptr, nullptr, nullptr, &notes);

            int right = 0, passingKept = 0, passing = 0, inChord = 0, ordered = 0, doubled = 0, counted = 0, bassMoves = 0, chordChanges = 0;
            int shared = 0, noThird = 0;
            std::vector<float> moves;
            const HarmonyEngine::NoteInfo* prev = nullptr;
            std::printf ("chords (auto), expected I vi IV V I:\n    ");
            for (size_t i = 0; i < melody.size(); ++i)
            {
                const auto* n = findNote (notes, melody[i].start);
                if (n == nullptr)
                {
                    std::printf ("?? ");
                    continue;
                }
                char name[16];
                Chords::name (n->chord, name, (int) sizeof (name));
                const bool ok = n->chord.mask == diatonicC[progression[barOf[i]].degree].mask;
                std::printf ("%s%s ", name, ok ? "" : "*");
                right += ok ? 1 : 0;
                if (melody[i].dur < 0.2)
                {
                    ++passing;
                    passingKept += ok ? 1 : 0;
                }
                else
                {
                    ++counted;
                    const int t0 = juce::roundToInt (n->target[0]), t1 = juce::roundToInt (n->target[1]), t2 = juce::roundToInt (n->target[2]);
                    inChord += n->chord.has (t0) && n->chord.has (t1) && pc (t2) == n->chord.root ? 1 : 0;
                    ordered += t0 > n->name && n->name > t1 && t1 > t2 ? 1 : 0;
                    doubled += pc (t0) == pc (n->name) || pc (t1) == pc (n->name) ? 1 : 0;
                    // doubling the root (and leaving out the 5th) is a normal close-harmony voicing;
                    // two voices on the 3rd or 5th is the weak case
                    shared += pc (t0) == pc (t1) && pc (t0) != n->chord.root ? 1 : 0;
                    const int third = pc (n->chord.root + (n->chord.has (n->chord.root + 4) ? 4 : 3));
                    noThird += pc (t0) != third && pc (t1) != third && pc (n->name) != third ? 1 : 0;
                }
                if (prev != nullptr)
                {
                    moves.push_back (std::abs (n->target[0] - prev->target[0]));
                    moves.push_back (std::abs (n->target[1] - prev->target[1]));
                    bassMoves += juce::roundToInt (n->target[2]) != juce::roundToInt (prev->target[2]) ? 1 : 0;
                    chordChanges += n->chord.mask != prev->chord.mask ? 1 : 0;
                }
                prev = n;
            }
            std::sort (moves.begin(), moves.end());
            const float medianMove = moves.empty() ? 99.0f : moves[moves.size() / 2];
            std::printf ("\n    %d/%zu chords right (%d/%d passing notes kept their bar's chord)\n", right, melody.size(), passingKept, passing);
            std::printf ("    voice leading: chord tones %d/%d, ordered %d/%d, lead doubled %d/%d, median move %.1f st, bass moves %d (chord changes %d)\n",
                         inChord, counted, ordered, counted, doubled, counted, medianMove, bassMoves, chordChanges);
            std::printf ("    upper voices doubling the 3rd or 5th %d/%d, chord missing its 3rd %d/%d\n", shared, counted, noThird, counted);
            if (right * 10 < (int) melody.size() * 8 || inChord != counted || ordered != counted || doubled * 10 > counted
                || medianMove > 3.0f || bassMoves > chordChanges || shared * 10 > counted || noThird * 10 > counted)
                ++failures;
        }

        // ---- the synthesized voice really sings the chosen target ----
        {
            auto s = plainVoices (1);
            s.harmonyMode = HarmonyModes::ChordsAuto;
            NoteList notes;
            const auto out = runEngine (vocal, s, 1.0, nullptr, nullptr, nullptr, &notes);
            int bad = 0, checked = 0;
            for (const auto& m : melody)
            {
                const auto* n = findNote (notes, m.start);
                if (m.dur < 0.2 || n == nullptr)
                    continue;
                const int a = (int) ((m.start + 0.08) * sr), b = (int) ((m.start + m.dur - 0.04) * sr);
                const float got = medianPitch (out.getReadPointer (0) + a, b - a);
                ++checked;
                bad += std::abs (got - (n->target[0] + n->tuning)) < 0.5f ? 0 : 1;
            }
            std::printf ("    rendered voice on its chord target: %d/%d notes\n", checked - bad, checked);
            if (bad > 0)
                ++failures;
        }

        // ---- MIDI chords override the progression; MIDI notes are sung exactly ----
        {
            // Em  F  Dm  G7  C  (each fits the melody, none is the auto choice except the last)
            const std::vector<std::vector<int>> chords = { { 52, 55, 59 }, { 53, 57, 60 }, { 50, 53, 57 }, { 55, 59, 62, 65 }, { 48, 52, 55 } };
            MidiTrack track;
            std::vector<Chords::Chord> expected;
            for (int b = 0; b < 5; ++b)
            {
                const int on = (int) ((barStart[(size_t) b] - 0.02) * sr), off = (int) ((barStart[(size_t) b + 1] - 0.03) * sr);
                for (int nn : chords[(size_t) b])
                    track.push_back ({ on, true, nn });
                for (int nn : chords[(size_t) b])
                    track.push_back ({ off, false, nn });
                expected.push_back (Chords::fromNotes (chords[(size_t) b].data(), (int) chords[(size_t) b].size()));
            }

            auto s = plainVoices (1);
            s.harmonyMode = HarmonyModes::ChordsMidi;
            NoteList notes;
            const auto out = runEngine (vocal, s, 1.0, nullptr, nullptr, &track, &notes);
            int chordOk = 0, pitchOk = 0, checked = 0;
            for (size_t i = 0; i < melody.size(); ++i)
            {
                const auto& m = melody[i];
                const auto* n = findNote (notes, m.start);
                if (m.dur < 0.2 || n == nullptr)
                    continue;
                ++checked;
                const auto& want = expected[(size_t) barOf[i]];
                chordOk += n->chord.mask == want.mask ? 1 : 0;
                const int a = (int) ((m.start + 0.08) * sr), b = (int) ((m.start + m.dur - 0.04) * sr);
                pitchOk += want.has (juce::roundToInt (medianPitch (out.getReadPointer (0) + a, b - a))) ? 1 : 0;
            }
            std::printf ("chords (MIDI): chord followed %d/%d, rendered pitch in the MIDI chord %d/%d\n", chordOk, checked, pitchOk, checked);
            if (chordOk != checked || pitchOk != checked)
                ++failures;

            // MIDI notes: hold E5 + C5 throughout; voice 1 sings E5, voice 2 C5
            MidiTrack held = { { 0, true, 76 }, { 0, true, 72 } };
            auto s2 = plainVoices (2);
            s2.harmonyMode = HarmonyModes::MidiNotes;
            s2.voice[1].levelDb = -60.0f;   // listen to voice 1 alone
            const auto out2 = runEngine (vocal, s2, 1.0, nullptr, nullptr, &held, &notes);
            int exact = 0, targetsOk = 0;
            checked = 0;
            for (const auto& m : melody)
            {
                const auto* n = findNote (notes, m.start);
                if (m.dur < 0.2 || n == nullptr)
                    continue;
                ++checked;
                targetsOk += std::abs (n->target[0] + n->tuning - 76.0f) < 0.01f && std::abs (n->target[1] + n->tuning - 72.0f) < 0.01f ? 1 : 0;
                const int a = (int) ((m.start + 0.08) * sr), b = (int) ((m.start + m.dur - 0.04) * sr);
                exact += std::abs (medianPitch (out2.getReadPointer (0) + a, b - a) - 76.0f) < 0.3f ? 1 : 0;
            }
            std::printf ("MIDI notes: targets %d/%d, rendered voice 1 on E5 within 0.3 st %d/%d\n", targetsOk, checked, exact, checked);
            if (targetsOk != checked || exact != checked)
                ++failures;
        }

        // ---- Sing On: Long Notes ----
        {
            std::vector<SungNote> m;
            double t = 0.3;
            for (int k = 0; k < 8; ++k)
            {
                const double d = k % 2 ? 0.8 : 0.25;
                m.push_back ({ 67 + (k % 3) * 2, t, d, false });
                t += d + 0.05;
            }
            const auto v = synthVocal (m, t + 0.5);
            auto s = plainVoices (1);
            s.harmonyMode = HarmonyModes::ChordsAuto;
            s.singOn = SingModes::LongNotes;
            s.longNoteMs = 400.0f;
            const auto out = runEngine (v, s, 1.0);

            double eShort = 0.0, eLong = 0.0;
            for (const auto& n : m)
            {
                const int a = (int) ((n.start + 0.05) * sr), b = (int) ((n.start + n.dur - 0.05) * sr);
                double e = 0.0;
                for (int k = a; k < b; ++k)
                    e += (double) out.getSample (0, k) * out.getSample (0, k);
                (n.dur < 0.4 ? eShort : eLong) += e / (b - a);
            }
            const double diff = 10.0 * std::log10 ((eLong + 1e-12) / (eShort + 1e-12));
            std::printf ("sing on long notes: long notes %.1f dB louder than short ones\n", diff);
            if (diff < 30.0)
                ++failures;
        }

        // ---- live mode ----
        {
            double total = 0.0;
            const auto m = testMelody (0, total);
            const auto v = synthVocal (m, total);
            auto s = plainVoices (1);
            s.harmonyMode = HarmonyModes::Intervals;
            int lat = 0;
            double cpu = 0.0;
            const auto out = runEngine (v, s, 0.0, &cpu, nullptr, nullptr, nullptr, &lat);
            int bad = 0, checked = 0;
            for (const auto& n : m)
            {
                if (n.start < 2.0 || n.dur < 0.1)
                    continue;   // the key needs a couple of seconds without lookahead
                const int a = (int) ((n.start + 0.08) * sr), b = (int) ((n.start + n.dur - 0.04) * sr);
                const float got = medianPitch (out.getReadPointer (0) + a, b - a);
                ++checked;
                bad += std::abs (got - (float) diatonicUp (n.midi, 7, 2)) < 0.5f ? 0 : 1;
            }
            std::printf ("live mode: latency %.1f ms, 3rd up right on %d/%d notes after 2 s, %.2f%% CPU\n", 1000.0 * lat / sr,
                         checked - bad, checked, cpu);
            if (lat > (int) (0.03 * sr) + 1 || bad > 1)
                ++failures;
        }

        // ---- full patch in chord mode: CPU and stability ----
        {
            auto s = plainVoices (3);
            s.harmonyMode = HarmonyModes::ChordsAuto;
            s.stack = 3;
            s.humanize = 0.3f;
            s.entryLagMs = 25.0f;
            double cpu = 0.0;
            const auto out = runEngine (vocal, s, 1.0, &cpu);
            const float peak = out.getMagnitude (0, out.getNumSamples());
            std::printf ("chord mode full patch: %.2f%% CPU, peak %.2f\n", cpu, peak);
            if (! std::isfinite (peak) || peak > 4.0f)
                ++failures;
        }

        return failures;
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
                s.harmonyMode = HarmonyModes::Intervals;
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
            s.harmonyMode = HarmonyModes::Intervals;
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
            s.harmonyMode = HarmonyModes::Intervals;
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

        failures += runV2Checks();

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
                engine.forEachNote ([&k] (const HarmonyEngine::NoteInfo& n) {
                    char chord[16];
                    Chords::name (n.chord, chord, (int) sizeof (chord));
                    if (k++ < 80)
                        std::printf ("  note %3d  %6.2f-%6.2f s  %6.2f  -> %s%d  chord %s\n", k, n.start, n.end, n.midi,
                                     noteName (n.name), n.name / 12 - 1, chord);
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
