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

    struct SungNote
    {
        int midi;
        double start, dur;
        bool sibilantBefore;
        double settleSemis = 0.0, settleSec = 0.0;   // start this far off pitch for this long, then settle (a sung scoop)
    };

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
                const double settle = t < n.settleSec ? n.settleSemis : n.settleSemis * std::exp (-(t - n.settleSec) / 0.015);
                const double midi = n.midi + scoop + settle + 0.3 * vibEnv * std::sin (2.0 * pi * 5.2 * t);
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
        if (const char* k = std::getenv ("CHV_TIGHT"))
            s.tightness = (float) std::atof (k);
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
                const bool off = std::abs (got - (n->target[0] + n->tuning)) >= 0.5f;
                bad += off ? 1 : 0;
                if (off)
                {
                    char chord[16];
                    Chords::name (n->chord, chord, (int) sizeof (chord));
                    std::printf ("      note at %.2f s (%.2f s long): sung %d (tracked %.2f, named %d), chord %s, target %.2f, rendered %.2f\n",
                                 m.start, m.dur, m.midi, n->midi, n->name, chord, n->target[0] + n->tuning, got);
                }
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

        // ---- Sing On with sung scoops, entry lag and phrases: each tone sung from its start, or not at all ----
        {
            // three phrases; '*' tones start 0.6 st sharp for 150 ms (the tracker splits these into two notes)
            struct Tone { int midi; double dur; bool scoop, longNote, phraseEnd; };
            const std::vector<std::vector<Tone>> phrases = {
                { { 67, 0.3, false, false, false }, { 69, 0.3, false, false, false }, { 71, 0.7, true, true, false },
                  { 72, 0.25, false, false, false }, { 74, 0.9, true, true, true } },
                { { 72, 0.3, false, false, false }, { 71, 0.3, false, false, false }, { 69, 1.3, true, true, true } },
                { { 67, 0.3, false, false, false }, { 69, 0.6, true, true, true }, { 67, 0.15, false, false, false } },
            };
            std::vector<SungNote> m;
            std::vector<Tone> tones;
            double t = 0.3;
            for (const auto& p : phrases)
            {
                for (const auto& tone : p)
                {
                    SungNote n { tone.midi, t, tone.dur, false };
                    if (tone.scoop)
                    {
                        n.settleSemis = 0.6;
                        n.settleSec = 0.15;
                    }
                    m.push_back (n);
                    tones.push_back (tone);
                    t += tone.dur + 0.04;
                }
                t += 0.6;   // pause between phrases
            }
            const auto v = synthVocal (m, t + 0.5);

            auto run = [&] (int mode, double lookahead, const char* label) {
                auto s = plainVoices (1);
                s.harmonyMode = HarmonyModes::ChordsAuto;
                s.singOn = mode;
                s.longNoteMs = 400.0f;
                s.entryLagMs = 25.0f;   // realistic: voices re-read the previous note while entering
                s.humanize = 0.3f;
                NoteList notes;
                const auto out = runEngine (v, s, lookahead, nullptr, nullptr, nullptr, &notes);
                if (std::getenv ("CHV_DEBUG") != nullptr)
                    for (const auto& n : notes)
                        std::printf ("    note %.2f-%.2f  %.2f\n", n.start, n.end, n.midi);

                int wrong = 0;
                std::printf ("%s:", label);
                for (size_t i = 0; i < m.size(); ++i)
                {
                    const bool want = mode == SingModes::LongNotes ? tones[i].longNote : tones[i].phraseEnd;
                    // from just after the entry lag to just before the end, and separately the first 150 ms
                    const int a = (int) ((m[i].start + 0.07) * sr), b = (int) ((m[i].start + m[i].dur - 0.05) * sr);
                    const int c = (int) ((m[i].start + 0.07) * sr), d = (int) ((m[i].start + 0.22) * sr);
                    auto energy = [&] (const float* x, int from, int to) {
                        double e = 0.0;
                        for (int k = from; k < to; ++k)
                            e += (double) x[k] * x[k];
                        return e / juce::jmax (1, to - from);
                    };
                    const double lead = energy (v.data(), a, b);
                    const double body = energy (out.getReadPointer (0), a, b) / lead;
                    const double head = energy (out.getReadPointer (0), c, juce::jmin (d, b)) / lead;
                    const bool sung = body > 0.05, entered = head > 0.05, silent = body < 1e-4;
                    const bool ok = want ? (sung && entered) : silent;
                    wrong += ok ? 0 : 1;
                    std::printf (" %s%s", want ? (entered ? "S" : "late") : (silent ? "-" : "leak"), ok ? "" : "*");
                }
                std::printf ("   (%d wrong)\n", wrong);
                return wrong;
            };
            int wrong = run (SingModes::LongNotes, 1.0, "long notes, 1 s  ");
            wrong += run (SingModes::PhraseEnds, 2.0, "phrase ends, 2 s ");
            wrong += run (SingModes::PhraseEnds, 1.0, "phrase ends, 1 s ");
            if (wrong > 0)
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

        // ---- Instrument: a bass line down to E1 (41 Hz), 3rd up in E minor ----
        {
            const int line[] = { 28, 31, 33, 35, 31, 28, 35, 33 };   // E1 G1 A1 B1 G1 E1 B1 A1
            std::vector<SungNote> m;
            double t = 0.3;
            for (int nn : line)
            {
                m.push_back ({ nn, t, 0.6, false });
                t += 0.65;
            }
            const auto v = synthVocal (m, t + 0.5);
            auto s = plainVoices (1);
            s.instrument = true;
            s.harmonyMode = HarmonyModes::Intervals;
            s.voice[0].interval = 2;   // 3rd up
            s.keyMode = 5;             // E
            s.scaleMode = 2;           // minor
            const auto out = runEngine (v, s, 1.0);

            PitchTracker low;
            low.prepare (sr, 8192, 25.0f, 600.0f);
            int right = 0, checked = 0;
            for (const auto& n : m)
            {
                const int a = (int) ((n.start + 0.1) * sr);
                if (a + 8192 > out.getNumSamples())
                    continue;
                const auto r = low.analyse (out.getReadPointer (0) + a);
                const float got = r.f0 > 0.0f ? 69.0f + 12.0f * std::log2 (r.f0 / 440.0f) : 0.0f;
                ++checked;
                right += std::abs (got - (float) diatonicUp (n.midi, 7, 2)) < 0.5f ? 1 : 0;   // E minor = G major scale
            }
            std::printf ("instrument bass line (E1-B1): 3rd up right on %d/%d notes\n", right, checked);
            if (right != checked)
                ++failures;
        }

        // ---- Double: two copies of the lead, detuned by LFOs, one each side ----
        {
            double total = 0.0;
            const auto m = testMelody (0, total);
            const auto v = synthVocal (m, total);
            auto s = plainVoices (1);
            s.voice[0].levelDb = -48.0f;   // harmony off: listen to the doubles only
            s.doubleDb = 0.0f;
            s.doubleDetuneCents = 10.0f;
            s.doubleRateHz = 0.5f;
            const auto out = runEngine (v, s, 1.0);

            // per 50 ms, pitch of each side against the lead, over the long notes
            PitchTracker pt;
            pt.prepare (sr, 2048, 60.0f, 1500.0f);
            std::vector<float> devL, devR;
            double corrNum = 0.0, eL = 0.0, eR = 0.0;
            for (const auto& n : m)
            {
                if (n.dur < 0.4)
                    continue;
                for (double at = n.start + 0.1; at + 0.05 < n.start + n.dur - 0.05; at += 0.05)
                {
                    const int a = (int) (at * sr);
                    const auto lead = pt.analyse (v.data() + a);
                    const auto l = pt.analyse (out.getReadPointer (0) + a);
                    const auto r = pt.analyse (out.getReadPointer (1) + a);
                    if (lead.f0 > 0 && l.f0 > 0 && r.f0 > 0)
                    {
                        devL.push_back (1200.0f * std::log2 (l.f0 / lead.f0));
                        devR.push_back (1200.0f * std::log2 (r.f0 / lead.f0));
                    }
                }
                for (int k = (int) (n.start * sr); k < (int) ((n.start + n.dur) * sr); ++k)
                {
                    corrNum += (double) out.getSample (0, k) * out.getSample (1, k);
                    eL += (double) out.getSample (0, k) * out.getSample (0, k);
                    eR += (double) out.getSample (1, k) * out.getSample (1, k);
                }
            }
            auto range = [] (std::vector<float> d) {
                std::sort (d.begin(), d.end());
                return d.empty() ? 0.0f : d[d.size() * 9 / 10] - d[d.size() / 10];
            };
            auto meanAbs = [] (const std::vector<float>& d) {
                double a = 0.0;
                for (float x : d)
                    a += std::abs (x);
                return d.empty() ? 99.0 : a / (double) d.size();
            };
            const double corr = corrNum / std::sqrt (eL * eR + 1e-12);
            std::printf ("double: left %.1f / right %.1f cents from the lead on average, swinging %.0f / %.0f cents; L/R correlation %.2f\n",
                         meanAbs (devL), meanAbs (devR), range (devL), range (devR), corr);
            // within the detune depth, actually moving, and the two sides different
            if (meanAbs (devL) > 12.0 || meanAbs (devR) > 12.0 || range (devL) < 4.0f || range (devR) < 4.0f || corr > 0.95 || eL < 1e-6)
                ++failures;
        }

        // ---- mute / solo ----
        {
            double total = 0.0;
            const auto m = testMelody (0, total);
            const auto v = synthVocal (m, total);
            auto energy = [] (const juce::AudioBuffer<float>& b) {
                double e = 0.0;
                for (int k = 0; k < b.getNumSamples(); ++k)
                    e += (double) b.getSample (0, k) * b.getSample (0, k);
                return e;
            };
            // how much of the output isn't the dry input
            auto harmonyLeft = [&] (const juce::AudioBuffer<float>& b) {
                double e = 0.0;
                for (int k = 0; k < b.getNumSamples(); ++k)
                {
                    const double d = b.getSample (0, k) - v[(size_t) k];
                    e += d * d;
                }
                return e;
            };
            auto base = [] {
                HarmonySettings s;
                s.harmonyMode = HarmonyModes::Intervals;
                s.voices = 2;
                s.voice[0].pan = s.voice[1].pan = 0.0f;
                return s;
            };
            const double dry = energy (runEngine (v, [&] { auto s = base(); s.voices = 1; s.voice[0].levelDb = -48.0f; return s; }(), 1.0));

            auto s1 = base();
            s1.voice[0].mute = s1.voice[1].mute = true;
            const double mutedVoices = harmonyLeft (runEngine (v, s1, 1.0)) / dry;

            auto s2 = base();
            s2.drySolo = true;
            const double drySoloed = harmonyLeft (runEngine (v, s2, 1.0)) / dry;

            auto s3 = base();
            s3.voice[1].solo = true;
            const auto soloOut = runEngine (v, s3, 1.0);
            auto s4 = base();
            s4.dryMute = true;
            s4.voice[0].mute = true;
            const auto voice2Only = runEngine (v, s4, 1.0);
            double diff = 0.0;
            for (int k = 0; k < soloOut.getNumSamples(); ++k)
            {
                const double d = soloOut.getSample (0, k) - voice2Only.getSample (0, k);
                diff += d * d;
            }
            const double soloMatches = diff / energy (voice2Only);

            std::printf ("mute/solo: voices muted leaves %.0f dB of harmony, dry soloed %.0f dB, voice 2 soloed = dry and voice 1 muted to %.0f dB\n",
                         10.0 * std::log10 (mutedVoices + 1e-12), 10.0 * std::log10 (drySoloed + 1e-12), 10.0 * std::log10 (soloMatches + 1e-12));
            if (mutedVoices > 1e-4 || drySoloed > 1e-4 || soloMatches > 1e-3)
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
                NoteList dbgNotes;
                const auto out = runEngine (vocal, s, 1.0, nullptr, &detected, nullptr, &dbgNotes);
                if (std::getenv ("CHV_DEBUG") != nullptr && ! dbgNotes.empty())
                    std::printf ("    first note %.2f-%.2f midi %.2f grid %.2f name %d target %.1f\n", dbgNotes[0].start, dbgNotes[0].end,
                                 dbgNotes[0].midi, dbgNotes[0].tuning, dbgNotes[0].name, dbgNotes[0].target[0]);
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
                    if (i == 0)
                        continue;   // warm-up: with 1 s of lookahead the opening notes (e.g. D, E) fit more than one key
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

    bool loadMono (const juce::File& file, std::vector<float>& mono, double& rate)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr)
            return false;
        rate = reader->sampleRate;
        juce::AudioBuffer<float> buf ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&buf, 0, buf.getNumSamples(), 0, true, true);
        mono.assign ((size_t) buf.getNumSamples(), 0.0f);
        for (int c = 0; c < buf.getNumChannels(); ++c)
            for (int i = 0; i < buf.getNumSamples(); ++i)
                mono[(size_t) i] += buf.getSample (c, i) / (float) buf.getNumChannels();
        return true;
    }

    // harmony only (left channel), aligned with the input
    std::vector<float> renderAt (const std::vector<float>& mono, double rate, const HarmonySettings& s, NoteList* notesOut = nullptr)
    {
        HarmonyEngine engine;
        const char* la = std::getenv ("CHV_LOOKAHEAD");   // seconds, 0 = live
        engine.prepare (rate, la != nullptr ? std::atof (la) : 1.0);
        const int lat = engine.getLatencySamples(), total = (int) mono.size() + lat;
        std::vector<float> in ((size_t) total, 0.0f), l ((size_t) total), r ((size_t) total);
        std::copy (mono.begin(), mono.end(), in.begin());
        for (int pos = 0; pos < total; pos += 512)
        {
            const int n = juce::jmin (512, total - pos);
            const float* ins[1] = { in.data() + pos };
            float* outs[2] = { l.data() + pos, r.data() + pos };
            engine.process (ins, 1, outs, 2, n, s);
        }
        if (notesOut != nullptr)
        {
            notesOut->clear();
            engine.forEachNote ([notesOut] (const HarmonyEngine::NoteInfo& info) { notesOut->push_back (info); });
        }
        std::vector<float> outL ((size_t) mono.size()), outR ((size_t) mono.size());
        for (size_t i = 0; i < mono.size(); ++i)
            outL[i] = 0.5f * (l[i + (size_t) lat] + r[i + (size_t) lat]);
        return outL;
    }

    // What actually sounds: for every 10 ms where the lead and one harmony voice are both clearly
    // pitched, the interval between them (rounded, and its tuning error) and how clean the voice is.
    int analyzeFile (const juce::File& file)
    {
        std::vector<float> mono;
        double rate = 0.0;
        if (! loadMono (file, mono, rate))
            return 1;

        int len = 1;
        while (len < rate * 0.042)
            len <<= 1;
        PitchTracker pt;
        pt.prepare (rate, len, 60.0f, 1700.0f);
        const int step = (int) (rate * 0.01);

        struct Variant { const char* name; int mode, position, interval; float formant; };
        const Variant variants[] = {
            { "chords, voice above", HarmonyModes::ChordsAuto, Positions::Above, 2, 0.3f },
            { "chords, voice below", HarmonyModes::ChordsAuto, Positions::Below, 2, 0.3f },
            { "intervals, 3rd up  ", HarmonyModes::Intervals, Positions::Above, 2, 0.3f },
            { "intervals, 3rd up, formant 0", HarmonyModes::Intervals, Positions::Above, 2, 0.0f },
        };
        static const char* names[12] = { "P1/P8", "m2", "M2", "m3", "M3", "P4", "TT", "P5", "m6", "M6", "m7", "M7" };

        std::printf ("%s (%.0f Hz)\n", file.getFileName().toRawUTF8(), rate);

        // null test: a unison voice with no formant shift should give back the input
        {
            HarmonySettings s;
            s.harmonyMode = HarmonyModes::Intervals;
            s.voices = 1;
            s.voice[0].interval = 0;
            s.voice[0].levelDb = 0.0f;
            s.voice[0].pan = 0.0f;
            s.autoFormant = 0.0f;
            s.dryDb = -60.0f;
            s.entryLagMs = s.humanize = s.vibDepthCents = s.consonants = s.minNoteMs = 0.0f;
            const auto h = renderAt (mono, rate, s);
            double sig = 0.0, err = 0.0;
            for (size_t i = 0; i < mono.size(); ++i)
                if (std::abs (mono[i]) > 1e-4f)
                {
                    sig += (double) mono[i] * mono[i];
                    const double e = h[i] - mono[i];
                    err += e * e;
                }
            std::printf ("  unison null test: input vs output %.1f dB SNR (above 20 dB = near-transparent)\n", 10.0 * std::log10 (sig / (err + 1e-12)));
        }

        for (const auto& var : variants)
        {
            HarmonySettings s;
            s.harmonyMode = var.mode;
            s.voices = 1;
            s.voice[0].position = var.position;
            s.voice[0].interval = var.interval;
            s.voice[0].levelDb = 0.0f;
            s.autoFormant = var.formant;
            s.dryDb = -60.0f;
            s.entryLagMs = s.humanize = s.vibDepthCents = s.consonants = 0.0f;
            const auto h = renderAt (mono, rate, s);

            int hist[12] {}, both = 0, leadVoiced = 0, rough = 0;
            double centsErr = 0.0, leadAp = 0.0, harmAp = 0.0;
            for (int p = 0; p + len <= (int) mono.size(); p += step)
            {
                const auto a = pt.analyse (mono.data() + p);
                if (! (a.f0 > 0.0f && a.aperiodicity < 0.15f && a.rms > 0.01f))
                    continue;
                ++leadVoiced;
                const auto b = pt.analyse (h.data() + p);
                if (! (b.f0 > 0.0f && b.aperiodicity < 0.2f))
                {
                    ++rough;
                    continue;
                }
                ++both;
                leadAp += a.aperiodicity;
                harmAp += b.aperiodicity;
                const double iv = 12.0 * std::log2 (b.f0 / a.f0);
                const int ri = (int) std::lround (iv);
                hist[((ri % 12) + 12) % 12]++;
                centsErr += std::abs (iv - ri) * 100.0;
            }
            std::printf ("  %-30s rough/unpitched %4.1f%%  interval error %4.1f ct  aperiodicity lead %.3f harmony %.3f\n    ", var.name,
                         100.0 * rough / juce::jmax (1, leadVoiced), centsErr / juce::jmax (1, both), leadAp / juce::jmax (1, both),
                         harmAp / juce::jmax (1, both));
            for (int k = 0; k < 12; ++k)
                if (hist[k] * 100 >= both)   // show intervals heard at least 1% of the time
                    std::printf ("%s %d%%  ", names[k], hist[k] * 100 / juce::jmax (1, both));
            std::printf ("\n");
        }
        return 0;
    }

    // Consonants in a soloed voice: 20 ms frames where the lead is making unpitched sound ("s", "h",
    // breaths) next to sung notes, and how loud voice 1 is there compared with its level on the vowels.
    int consonantsFile (const juce::File& file)
    {
        std::vector<float> mono;
        double rate = 0.0;
        if (! loadMono (file, mono, rate))
            return 1;
        int len = 1;
        while (len < rate * 0.042)
            len <<= 1;
        PitchTracker pt;
        pt.prepare (rate, len, 60.0f, 1700.0f);
        const int step = (int) (rate * 0.02);
        const int frames = ((int) mono.size() - len) / step;

        HarmonySettings s;
        s.voice[0].solo = true;
        if (const char* k = std::getenv ("CHV_TIGHT"))
            s.tightness = (float) std::atof (k);
        const auto h = renderAt (mono, rate, s);

        std::vector<int> kind ((size_t) frames, 0);   // 0 silence, 1 sung (pitched), 2 unpitched sound
        std::vector<float> ld ((size_t) frames), hd ((size_t) frames);
        for (int k = 0; k < frames; ++k)
        {
            const int c = k * step + len / 4;   // YIN measures the first half of its frame
            double el = 0.0, eh = 0.0;
            for (int i = c - step / 2; i < c + step / 2; ++i)
            {
                el += (double) mono[(size_t) i] * mono[(size_t) i];
                eh += (double) h[(size_t) i] * h[(size_t) i];
            }
            ld[(size_t) k] = (float) (10.0 * std::log10 (el / step + 1e-12));
            hd[(size_t) k] = (float) (10.0 * std::log10 (eh / step + 1e-12));
            const auto r = pt.analyse (mono.data() + k * step);
            kind[(size_t) k] = r.f0 > 0.0f && r.aperiodicity < 0.25f ? 1 : ld[(size_t) k] > -50.0f ? 2 : 0;
        }
        // the voice's level on vowels, relative to the lead
        std::vector<float> vowel, cons;
        for (int k = 0; k < frames; ++k)
            if (kind[(size_t) k] == 1 && ld[(size_t) k] > -40.0f)
                vowel.push_back (hd[(size_t) k] - ld[(size_t) k]);
        // consonant frames within 250 ms of a sung frame
        int cut = 0;
        for (int k = 0; k < frames; ++k)
        {
            if (kind[(size_t) k] != 2)
                continue;
            bool near = false;
            for (int j = juce::jmax (0, k - 12); j <= juce::jmin (frames - 1, k + 12) && ! near; ++j)
                near = kind[(size_t) j] == 1;
            if (! near)
                continue;
            cons.push_back (hd[(size_t) k] - ld[(size_t) k]);
        }
        std::sort (vowel.begin(), vowel.end());
        const float vowelRel = vowel.empty() ? 0.0f : vowel[vowel.size() / 2];
        for (float x : cons)
            cut += x < vowelRel - 20.0f ? 1 : 0;
        std::sort (cons.begin(), cons.end());
        std::printf ("%s: voice 1 on vowels %.1f dB vs the lead; on consonants (%zu frames) median %.1f dB, cut (20 dB below its vowel level) %.0f%%\n",
                     file.getFileName().toRawUTF8(), vowelRel, cons.size(), cons.empty() ? 0.0f : cons[cons.size() / 2],
                     100.0 * cut / juce::jmax ((size_t) 1, cons.size()));

        // the two the listener pointed at: the end of "grace" and the start of "how"
        for (double t : { 6.14, 6.18, 6.22, 6.26, 6.30, 6.34, 6.38, 6.42, 6.46, 7.10, 7.18 })
        {
            const int k = (int) ((t * rate - len / 4) / step);
            if (k >= 0 && k < frames)
                std::printf ("  %.2f s: lead %s %.0f dB, voice 1 %.0f dB\n", t, kind[(size_t) k] == 1 ? "sung" : kind[(size_t) k] == 2 ? "consonant" : "quiet",
                             ld[(size_t) k], hd[(size_t) k]);
        }
        return 0;
    }

    // What the engine decided, note by note, in the singer's own tuning: harmony notes that clash with
    // the lead's note, and voices that change note while the lead's note doesn't (decision flicker).
    int choicesFile (const juce::File& file)
    {
        std::vector<float> mono;
        double rate = 0.0;
        if (! loadMono (file, mono, rate))
            return 1;
        HarmonySettings s;
        s.voices = 3;
        if (const char* k = std::getenv ("CHV_HARMONY"))
            s.harmonyMode = std::atoi (k);
        if (const char* k = std::getenv ("CHV_KEY"))
            s.keyMode = std::atoi (k);
        if (const char* k = std::getenv ("CHV_SCALE"))
            s.scaleMode = std::atoi (k);
        if (const char* k = std::getenv ("CHV_MINNOTE"))
            s.minNoteMs = (float) std::atof (k);
        NoteList notes;
        renderAt (mono, rate, s, &notes);

        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        double total = 0.0, clashTime[3] {};
        int sameNoteChanges[3] {}, sameNotePairs = 0;
        std::vector<std::string> examples;
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes[i];
            if (! n.harmonized)
                continue;
            const double d = n.end - n.start;
            total += d;
            bool any = false;
            for (int v = 0; v < 3; ++v)
            {
                const int iv = (((int) std::lround (n.target[v]) - n.name) % 12 + 12) % 12;
                if (n.voiceOn[v] && (iv == 1 || iv == 2 || iv == 6 || iv == 10 || iv == 11))
                {
                    clashTime[v] += d;
                    any = true;
                }
            }
            if (any && examples.size() < 14 && d > 0.15)
            {
                char buf[160], chord[16];
                Chords::name (n.chord, chord, (int) sizeof (chord));
                std::snprintf (buf, sizeof (buf), "    %6.2f s (%.2f s) lead %s, harmony %s %s %s, chord %s", n.start, d, names[((n.name % 12) + 12) % 12],
                               names[(((int) std::lround (n.target[0]) % 12) + 12) % 12], names[(((int) std::lround (n.target[1]) % 12) + 12) % 12],
                               names[(((int) std::lround (n.target[2]) % 12) + 12) % 12], chord);
                examples.push_back (buf);
            }
            if (i > 0 && notes[i - 1].harmonized && notes[i - 1].name == n.name && n.start - notes[i - 1].end < 0.05)
            {
                ++sameNotePairs;
                for (int v = 0; v < 3; ++v)
                    sameNoteChanges[v] += std::lround (notes[i - 1].target[v]) != std::lround (n.target[v]) ? 1 : 0;
            }
        }
        std::printf ("%s: %zu notes\n", file.getFileName().toRawUTF8(), notes.size());
        for (int v = 0; v < 3; ++v)
            std::printf ("  voice %d: clashes with the lead's note %.1f%% of sung time; changed note within a held lead note %d of %d times\n",
                         v + 1, 100.0 * clashTime[v] / juce::jmax (1e-9, total), sameNoteChanges[v], sameNotePairs);
        std::printf ("  examples (names on the singer's grid):\n");
        for (const auto& e : examples)
            std::printf ("%s\n", e.c_str());
        return 0;
    }

    // Tightness on a known vibrato: one long note at A3 with ±50 cents of 5.5 Hz vibrato, a 3rd up.
    // At Tightness 0 the harmony copies the vibrato; at 100% it should hold its target steadily.
    int steadyCheck()
    {
        const int total = (int) (sr * 4.0);
        std::vector<float> v ((size_t) total, 0.0f);
        double phase = 0.0;
        for (int i = (int) (0.3 * sr); i < (int) (3.7 * sr); ++i)
        {
            const double t = i / sr;
            const double midi = 57.0 + (std::getenv ("CHV_NOVIB") != nullptr ? 0.0 : 0.5) * std::sin (2.0 * pi * 5.5 * t);
            phase += 2.0 * pi * 440.0 * std::pow (2.0, (midi - 69.0) / 12.0) / sr;
            double y = 0.0;
            for (int k = 1; k <= 20; ++k)
                y += std::sin (k * phase) / k;
            v[(size_t) i] = (float) (0.2 * y);
        }
        PitchTracker pt;
        pt.prepare (sr, 2048, 60.0f, 1500.0f);
        for (float tight : { 0.0f, 0.5f, 1.0f })
        {
            auto s = plainVoices (1);
            s.harmonyMode = HarmonyModes::Intervals;
            s.keyMode = 1;   // C major: A3 -> C4
            s.scaleMode = 1;
            s.tightness = tight;
            NoteList notes;
            const auto out = runEngine (v, s, 1.0, nullptr, nullptr, nullptr, &notes);
            if (std::getenv ("CHV_DEBUG") != nullptr)
                for (const auto& n : notes)
                    std::printf ("    note %.2f-%.2f midi %.3f grid %.3f\n", n.start, n.end, n.midi, n.tuning);
            std::vector<float> p;
            for (int a = (int) (1.0 * sr); a + 2048 < (int) (3.2 * sr); a += (int) (0.01 * sr))
            {
                const auto r = pt.analyse (out.getReadPointer (0) + a);
                if (r.f0 > 0)
                    p.push_back (1200.0f * std::log2 (r.f0 / 261.63f));   // cents from C4
            }
            double mean = 0.0, ss = 0.0;
            for (float x : p)
                mean += x;
            mean /= juce::jmax ((size_t) 1, p.size());
            for (float x : p)
                ss += (x - mean) * (x - mean);
            std::printf ("tightness %3.0f%%: harmony %+.0f ct from C4 on average, wobbling %.0f ct (lead: 35 ct)\n", tight * 100.0f,
                         mean, std::sqrt (ss / juce::jmax ((size_t) 1, p.size())));
        }
        return 0;
    }

    // Each harmony voice soloed against the lead: where the lead sings but the voice is silent
    // (missed syllables), how much the voice's interval to the lead wobbles within a held note
    // (pitch shake), and how much its level flutters relative to the lead.
    int voicesFile (const juce::File& file)
    {
        std::vector<float> mono;
        double rate = 0.0;
        if (! loadMono (file, mono, rate))
            return 1;

        int len = 1;
        while (len < rate * 0.042)
            len <<= 1;
        PitchTracker pt;
        pt.prepare (rate, len, 60.0f, 1700.0f);
        const int step = (int) (rate * 0.02);
        const int frames = ((int) mono.size() - len) / step;

        struct F { float pitch, db; };
        auto analyse = [&] (const std::vector<float>& x) {
            std::vector<F> out ((size_t) juce::jmax (0, frames));
            for (int k = 0; k < frames; ++k)
            {
                const float* p = x.data() + k * step;
                const auto r = pt.analyse (p);
                double e = 0.0;
                for (int i = len / 4 - step / 2; i < len / 4 + step / 2; ++i)   // YIN measures the first half: centre there
                    e += (double) p[i] * p[i];
                out[(size_t) k] = { r.f0 > 0.0f && r.aperiodicity < 0.25f ? 69.0f + 12.0f * std::log2 (r.f0 / 440.0f) : 0.0f,
                                    (float) (10.0 * std::log10 (e / step + 1e-12)) };
            }
            return out;
        };
        const auto lead = analyse (mono);

        // singing: lead pitched and within 30 dB of its loudest stretches
        std::vector<float> dbs;
        for (const auto& f : lead)
            if (f.pitch > 0)
                dbs.push_back (f.db);
        std::sort (dbs.begin(), dbs.end());
        const float loud = dbs.empty() ? 0.0f : dbs[dbs.size() * 95 / 100];
        auto singing = [&] (int k) { return lead[(size_t) k].pitch > 0 && lead[(size_t) k].db > loud - 30.0f; };

        std::printf ("%s: %.0f s, lead sings %.0f%% of the time\n", file.getFileName().toRawUTF8(), mono.size() / rate,
                     100.0 * std::count_if (lead.begin(), lead.end(), [&] (const F& f) { return f.pitch > 0 && f.db > loud - 30.0f; }) / juce::jmax (1, frames));

        // candidate settings: entry lag ms, humanize, tightness, vibrato cents, delay of voices 2 and 3 (ms)
        struct Candidate { const char* name; float lag, humanize, tight, vib, delay2, delay3; };
        const HarmonySettings defaults;
        const Candidate all[] = {
            { "current defaults", defaults.entryLagMs, defaults.humanize, defaults.tightness, defaults.vibDepthCents,
              defaults.voice[1].delayMs, defaults.voice[2].delayMs },
            { "lag 10, humanize 15%, tightness 70%, vibrato 15", 10.0f, 0.15f, 0.7f, 15.0f, 12.0f, 6.0f },
            { "as above, delays 6/3 ms", 10.0f, 0.15f, 0.7f, 15.0f, 6.0f, 3.0f },
            { "lag 0, humanize 15%, tightness 70%, vibrato 15, delays 6/3", 0.0f, 0.15f, 0.7f, 15.0f, 6.0f, 3.0f },
            { "everything off, tightness 100%", 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f },
        };
        const int numVariants = std::getenv ("CHV_ALLVARIANTS") != nullptr ? 5 : 1;
        for (int variant = 0; variant < numVariants; ++variant)
        {
            const auto& c = all[variant];
            std::printf ("  %s:\n", c.name);
            for (int v = 0; v < 3; ++v)
            {
                HarmonySettings s;
                s.voices = 3;
                s.voice[v].solo = true;
                s.entryLagMs = c.lag;
                s.humanize = c.humanize;
                s.tightness = c.tight;
                s.vibDepthCents = c.vib;
                s.voice[1].delayMs = c.delay2;
                s.voice[2].delayMs = c.delay3;
                const auto h = analyse (renderAt (mono, rate, s));

                // the voice's typical level relative to the lead while both sound
                std::vector<float> rel;
                for (int k = 0; k < frames; ++k)
                    if (singing (k) && h[(size_t) k].db > lead[(size_t) k].db - 40.0f)
                        rel.push_back (h[(size_t) k].db - lead[(size_t) k].db);
                std::sort (rel.begin(), rel.end());
                const float typical = rel.empty() ? -20.0f : rel[rel.size() / 2];

                // misses: singing frames where the voice is well below its typical level (CHV_MISSDB, default 20 dB)
                const float missDb = std::getenv ("CHV_MISSDB") != nullptr ? (float) std::atof (std::getenv ("CHV_MISSDB")) : 20.0f;
                int missFrames = 0, singFrames = 0, events = 0;
                std::vector<double> times;
                bool inMiss = false;
                for (int k = 0; k < frames; ++k)
                {
                    if (! singing (k))
                    {
                        inMiss = false;
                        continue;
                    }
                    ++singFrames;
                    const bool miss = h[(size_t) k].db - lead[(size_t) k].db < typical - missDb;
                    missFrames += miss ? 1 : 0;
                    if (miss && ! inMiss)
                    {
                        ++events;
                        if (times.size() < 12)
                            times.push_back (k * step / rate + (double) len / 4 / rate);
                    }
                    inMiss = miss;
                }

                // shake: within runs where the rounded interval holds, spread of the interval around its median
                std::vector<float> spreads, flutter;
                std::vector<float> run;
                int runInterval = 1000;
                auto flush = [&] {
                    if (run.size() >= 8)
                    {
                        std::sort (run.begin(), run.end());
                        const float med = run[run.size() / 2];
                        double ss = 0.0;
                        for (float x : run)
                            ss += (x - med) * (x - med);
                        spreads.push_back ((float) std::sqrt (ss / run.size()));
                    }
                    run.clear();
                };
                for (int k = 1; k < frames; ++k)
                {
                    const auto& a = lead[(size_t) k];
                    const auto& b = h[(size_t) k];
                    if (singing (k) && b.pitch > 0)
                    {
                        const float iv = (b.pitch - a.pitch) * 100.0f;
                        const int r = (int) std::lround (iv / 100.0f);
                        if (r != runInterval)
                        {
                            flush();
                            runInterval = r;
                        }
                        run.push_back (iv);
                        if (singing (k - 1) && h[(size_t) k - 1].pitch > 0)
                            flutter.push_back (std::abs ((b.db - a.db) - (h[(size_t) k - 1].db - lead[(size_t) k - 1].db)));
                    }
                    else
                    {
                        flush();
                        runInterval = 1000;
                    }
                }
                flush();
                auto median = [] (std::vector<float> d) {
                    std::sort (d.begin(), d.end());
                    return d.empty() ? 0.0f : d[d.size() / 2];
                };

                // what a soloed voice sounds like on its own: its own pitch wobble within a note, and its
                // frame-to-frame pitch jitter, next to the lead's
                std::vector<float> ownSpread, ownRun, jitH, jitL;
                int ownNote = -1000;
                for (int k = 1; k < frames; ++k)
                {
                    const float p = h[(size_t) k].pitch;
                    const int n = p > 0 && singing (k) ? (int) std::lround (p) : -1000;
                    if (n != ownNote)
                    {
                        if (ownRun.size() >= 8)
                        {
                            const float m = median (ownRun);
                            double ss = 0.0;
                            for (float x : ownRun)
                                ss += (x - m) * (x - m) * 1e4;
                            ownSpread.push_back ((float) std::sqrt (ss / ownRun.size()));
                        }
                        ownRun.clear();
                        ownNote = n;
                    }
                    if (n != -1000)
                    {
                        ownRun.push_back (p);
                        const float pp = h[(size_t) k - 1].pitch, lp = lead[(size_t) k - 1].pitch;
                        if (pp > 0 && std::abs (p - pp) < 0.5f)
                            jitH.push_back (std::abs (p - pp) * 100.0f);
                        if (lp > 0 && lead[(size_t) k].pitch > 0 && std::abs (lead[(size_t) k].pitch - lp) < 0.5f)
                            jitL.push_back (std::abs (lead[(size_t) k].pitch - lp) * 100.0f);
                    }
                }

                // "seizure": the voice leaves its note and comes back within 140 ms (A-B-A) while the lead's
                // note stays the same; and clashing intervals with the lead (2nds, tritone, 7ths)
                std::vector<int> hn ((size_t) frames, -1000), ln ((size_t) frames, -1000);
                int clash = 0, both = 0;
                for (int k = 0; k < frames; ++k)
                    if (singing (k) && h[(size_t) k].pitch > 0)
                    {
                        hn[(size_t) k] = (int) std::lround (h[(size_t) k].pitch);
                        ln[(size_t) k] = (int) std::lround (lead[(size_t) k].pitch);
                        const int iv = ((hn[(size_t) k] - ln[(size_t) k]) % 12 + 12) % 12;
                        clash += iv == 1 || iv == 2 || iv == 6 || iv == 10 || iv == 11 ? 1 : 0;
                        ++both;
                    }
                struct Seg { int note, start, len; };
                std::vector<Seg> segs;
                for (int k = 0; k < frames; ++k)
                {
                    if (! segs.empty() && segs.back().note == hn[(size_t) k] && segs.back().start + segs.back().len == k)
                        ++segs.back().len;
                    else
                        segs.push_back ({ hn[(size_t) k], k, 1 });
                }
                int flicks = 0;
                std::vector<double> flickTimes;
                for (size_t q = 1; q + 1 < segs.size(); ++q)
                {
                    const auto &a = segs[q - 1], &b = segs[q], &c = segs[q + 1];
                    if (a.note < 0 || b.note < 0 || a.note != c.note || b.len > 7 || a.len < 2 || c.len < 2)
                        continue;
                    bool leadSteady = true;
                    for (int k = b.start - 1; k <= b.start + b.len && leadSteady; ++k)
                        leadSteady = ln[(size_t) k] == ln[(size_t) b.start - 1];
                    if (leadSteady)
                    {
                        ++flicks;
                        if (flickTimes.size() < 10)
                            flickTimes.push_back (b.start * step / rate + (double) len / 4 / rate);
                    }
                }

                std::printf ("    voice %d: missed %.1f%% of sung time in %d gaps; interval wobble %.0f ct; level flutter %.1f dB/20ms\n"
                             "      own pitch wobble %.0f ct, jitter %.1f ct/20ms (lead's jitter %.1f); clashing with lead %.1f%%;"
                             " A-B-A flicks %d (%.1f/min)",
                             v + 1, 100.0 * missFrames / juce::jmax (1, singFrames), events, median (spreads), median (flutter),
                             median (ownSpread), median (jitH), median (jitL), 100.0 * clash / juce::jmax (1, both), flicks,
                             flicks * 60.0 / (mono.size() / rate));
                for (double tm : flickTimes)
                    std::printf (" %.2f", tm);
                std::printf ("\n      first misses at:");
                for (double tm : times)
                    std::printf (" %.2f", tm);
                std::printf ("\n");
            }
            std::printf ("    lead's own pitch wobble within notes: ");
            {
                std::vector<float> spreads, run;
                int runNote = -1000;
                for (int k = 0; k < frames; ++k)
                {
                    const float p = lead[(size_t) k].pitch;
                    const int n = p > 0 ? (int) std::lround (p) : -1000;
                    if (n != runNote || ! singing (k))
                    {
                        if (run.size() >= 8)
                        {
                            std::sort (run.begin(), run.end());
                            const float med = run[run.size() / 2];
                            double ss = 0.0;
                            for (float x : run)
                                ss += (x - med) * (x - med) * 1e4;
                            spreads.push_back ((float) std::sqrt (ss / run.size()));
                        }
                        run.clear();
                        runNote = n;
                    }
                    if (singing (k))
                        run.push_back (p);
                }
                std::sort (spreads.begin(), spreads.end());
                std::printf ("%.0f ct\n", spreads.empty() ? 0.0f : spreads[spreads.size() / 2]);
            }
        }
        return 0;
    }

    // How long after each note starts does the lead, and the harmony, reach its settled pitch?
    // A harmony that takes much longer than the lead smears ("meows") into every note.
    int onsetsFile (const juce::File& file)
    {
        std::vector<float> mono;
        double rate = 0.0;
        if (! loadMono (file, mono, rate))
            return 1;

        int len = 1;
        while (len < rate * 0.03)
            len <<= 1;
        PitchTracker pt;
        pt.prepare (rate, len, 60.0f, 1700.0f);
        const int hop = (int) (rate * 0.005);
        auto track = [&] (const std::vector<float>& x) {
            std::vector<float> p;
            for (int i = 0; i + len <= (int) x.size(); i += hop)
            {
                const auto r = pt.analyse (x.data() + i);
                p.push_back (r.f0 > 0.0f && r.aperiodicity < 0.25f ? 69.0f + 12.0f * std::log2 (r.f0 / 440.0f) : 0.0f);
            }
            return p;
        };
        const auto lead = track (mono);

        // note starts from the lead: a jump of more than 0.7 st that then holds for 60 ms, or voicing starting
        std::vector<int> starts;
        for (int i = 1; i + 12 < (int) lead.size(); ++i)
        {
            const bool jump = lead[(size_t) i] > 0 && (lead[(size_t) i - 1] == 0 || std::abs (lead[(size_t) i] - lead[(size_t) i - 1]) > 0.7f);
            bool holds = jump;
            for (int k = 1; k <= 12 && holds; ++k)
                holds = lead[(size_t) (i + k)] > 0 && std::abs (lead[(size_t) (i + k)] - lead[(size_t) i]) < 0.7f;
            if (holds && (starts.empty() || i - starts.back() > 20))
                starts.push_back (i);
        }

        // settle time: until the pitch stays within 40 cents of its value 150-300 ms in
        auto settleMs = [&] (const std::vector<float>& p, int s) {
            std::vector<float> ref;
            for (int k = s + 30; k < s + 60 && k < (int) p.size(); ++k)
                if (p[(size_t) k] > 0)
                    ref.push_back (p[(size_t) k]);
            if (ref.size() < 10)
                return -1.0;
            std::sort (ref.begin(), ref.end());
            const float target = ref[ref.size() / 2];
            int last = s;
            for (int k = s; k < s + 30 && k < (int) p.size(); ++k)
                if (p[(size_t) k] == 0 || std::abs (p[(size_t) k] - target) > 0.4f)
                    last = k + 1;
            return (last - s) * 5.0;
        };

        std::printf ("%s: %zu note starts\n", file.getFileName().toRawUTF8(), starts.size());
        for (int variant = 0; variant < 3; ++variant)
        {
            HarmonySettings s;
            s.harmonyMode = HarmonyModes::Intervals;
            s.voices = 1;
            s.voice[0].interval = 2;
            s.voice[0].levelDb = 0.0f;
            s.dryDb = -60.0f;
            s.autoFormant = s.consonants = s.vibDepthCents = 0.0f;
            s.keyMode = 12;
            s.scaleMode = 2;
            const char* name = "default (entry lag 25 ms, humanize 30%)";
            if (variant == 1)
            {
                s.entryLagMs = s.humanize = 0.0f;
                name = "entry lag 0, humanize 0";
            }
            else if (variant == 2)
            {
                s.instrument = true;
                name = "Source: Instrument";
            }
            const auto h = renderAt (mono, rate, s);
            const auto harm = track (h);
            double leadSum = 0.0, harmSum = 0.0;
            int n = 0, slow = 0;
            for (int st : starts)
            {
                const double a = settleMs (lead, st), b = settleMs (harm, st);
                if (a < 0 || b < 0)
                    continue;
                leadSum += a;
                harmSum += b;
                slow += b > a + 30.0 ? 1 : 0;
                ++n;
            }
            std::printf ("  %-40s lead settles in %5.1f ms, harmony in %5.1f ms; harmony >30 ms slower on %d/%d notes\n", name,
                         leadSum / juce::jmax (1, n), harmSum / juce::jmax (1, n), slow, n);
        }
        return 0;
    }

    // When does the harmony actually sound in Sing On: Long Notes? Prints each stretch where
    // voice 1 is audible, next to the sung notes.
    int gatesFile (const juce::File& file)
    {
        std::vector<float> mono;
        double rate = 0.0;
        if (! loadMono (file, mono, rate))
            return 1;

        HarmonySettings s;
        s.voices = 1;
        s.voice[0].levelDb = 0.0f;
        s.dryDb = -60.0f;
        s.singOn = SingModes::LongNotes;
        s.consonants = 0.0f;
        if (const char* t = std::getenv ("CHV_LONG"))
            s.longNoteMs = (float) std::atof (t);
        if (std::getenv ("CHV_PHRASE") != nullptr)
            s.singOn = SingModes::PhraseEnds;
        if (std::getenv ("CHV_NOLAG") != nullptr)
            s.entryLagMs = s.humanize = 0.0f;
        const auto h = renderAt (mono, rate, s);

        // reference: the lead's average loudness while singing
        const int step = (int) (rate * 0.02);
        double leadSum = 0.0;
        int leadFrames = 0;
        for (int p = 0; p + step <= (int) mono.size(); p += step)
        {
            double el = 0.0;
            for (int k = p; k < p + step; ++k)
                el += (double) mono[(size_t) k] * mono[(size_t) k];
            if (el > 1e-5 * step)
            {
                leadSum += el;
                ++leadFrames;
            }
        }
        const double leadRef = leadSum / juce::jmax (1, leadFrames);

        bool on = false;
        double since = 0.0;
        std::printf ("harmony audible (%s):\n", s.singOn == SingModes::PhraseEnds ? "phrase ends"
                                                  : juce::String::formatted ("long notes >= %.0f ms", s.longNoteMs).toRawUTF8());
        for (int p = 0; p + step <= (int) mono.size(); p += step)
        {
            double eh = 0.0;
            for (int k = p; k < p + step; ++k)
                eh += (double) h[(size_t) k] * h[(size_t) k];
            const bool now = eh > 0.01 * leadRef;   // within 20 dB of the lead's average level
            if (now != on)
            {
                const double t = p / rate;
                if (on)
                    std::printf ("  %6.2f - %6.2f s  (%.2f s)\n", since, t, t - since);
                on = now;
                since = t;
            }
        }
        return 0;
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
            pt.prepare (rate, len, 70.0f, 1600.0f);
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
            if (const char* k = std::getenv ("CHV_KEY"))   // 0 = auto, 1..12 = fixed tonic C..B
                s.keyMode = std::atoi (k);
            if (const char* k = std::getenv ("CHV_SCALE"))   // 0 = auto, 1 = major, 2 = minor
                s.scaleMode = std::atoi (k);
            if (const char* k = std::getenv ("CHV_HARMONY"))   // HarmonyModes index
                s.harmonyMode = std::atoi (k);
            if (const char* k = std::getenv ("CHV_VOICES"))
                s.voices = std::atoi (k);
            if (std::getenv ("CHV_INSTRUMENT") != nullptr)   // Source: Instrument
                s.instrument = true;
            if (const char* k = std::getenv ("CHV_DOUBLE"))   // Double level, dB
                s.doubleDb = (float) std::atof (k);
            if (const char* k = std::getenv ("CHV_TIGHT"))   // Tightness 0..1
                s.tightness = (float) std::atof (k);
            if (const char* k = std::getenv ("CHV_SOLO"))   // solo voice 1..3
                s.voice[juce::jlimit (1, 3, std::atoi (k)) - 1].solo = true;
            if (std::getenv ("CHV_MUTEVOICES") != nullptr)
                for (auto& v : s.voice)
                    v.mute = true;
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
                    if (k++ < (std::getenv ("CHV_ALLNOTES") != nullptr ? 100000 : 80))
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
        if (juce::String (argv[i]) == "--consonants")
            return consonantsFile (juce::File (juce::String (argv[i + 1])));
        if (juce::String (argv[i]) == "--choices")
            return choicesFile (juce::File (juce::String (argv[i + 1])));
        if (juce::String (argv[i]) == "--steady")
            return steadyCheck();
        if (juce::String (argv[i]) == "--voices")
            return voicesFile (juce::File (juce::String (argv[i + 1])));
        if (juce::String (argv[i]) == "--onsets")
            return onsetsFile (juce::File (juce::String (argv[i + 1])));
        if (juce::String (argv[i]) == "--gates")
            return gatesFile (juce::File (juce::String (argv[i + 1])));
        if (juce::String (argv[i]) == "--analyze")
            return analyzeFile (juce::File (juce::String (argv[i + 1])));
        if (juce::String (argv[i]) == "--in")
            return harmonizeFile (juce::File (juce::String (argv[i + 1])));
        if (juce::String (argv[i]) == "--vst3")
            return smokeTestPlugin (juce::File (juce::String (argv[i + 1])));
    }
    return runChecks();
}
