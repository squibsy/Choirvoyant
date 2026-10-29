#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Chords.h"
#include "KeyFinder.h"
#include "PitchTracker.h"

// Harmony intervals, counted in scale steps of the detected key.
namespace Intervals
{
    constexpr int count = 12;
    inline const char* name (int i)
    {
        static const char* names[count] = { "Unison", "2nd Up", "3rd Up", "4th Up", "5th Up", "6th Up", "Octave Up",
                                            "3rd Down", "4th Down", "5th Down", "6th Down", "Octave Down" };
        return names[i];
    }
    inline int steps (int i)
    {
        static const int s[count] = { 0, 1, 2, 3, 4, 5, 7, -2, -3, -4, -5, -7 };
        return s[i];
    }
}

// Where a voice sits relative to the lead in the chord modes.
namespace Positions
{
    enum : int { Above, AboveWide, Below, BelowWide, Bass, count };
    inline const char* name (int i)
    {
        static const char* names[count] = { "Above", "Above Wide", "Below", "Below Wide", "Bass" };
        return names[i];
    }
}

// How a voice moves from note to note in the chord modes.
namespace Motions
{
    enum : int { Follow, Hold, Contrary, count };
    inline const char* name (int i)
    {
        static const char* names[count] = { "Follow", "Hold", "Contrary" };
        return names[i];
    }
}

namespace HarmonyModes
{
    enum : int { Intervals, ChordsAuto, ChordsMidi, MidiNotes, count };
    inline const char* name (int i)
    {
        static const char* names[count] = { "Intervals", "Chords (Auto)", "Chords (MIDI)", "MIDI Notes" };
        return names[i];
    }
}

namespace SingModes
{
    enum : int { All, LongNotes, PhraseEnds, count };
    inline const char* name (int i)
    {
        static const char* names[count] = { "All Notes", "Long Notes", "Phrase Ends" };
        return names[i];
    }
}

struct VoiceSettings
{
    int interval = 2;           // Intervals mode
    float levelDb = -4.0f;
    float pan = 0.0f;           // -1..1
    float delayMs = 0.0f;
    float formantSemis = 0.0f;  // manual offset on top of the automatic amount
    int position = Positions::Above;    // chord modes
    int motion = Motions::Follow;
};

struct HarmonySettings
{
    int keyMode = 0;            // 0 = auto, 1..12 = tonic C..B
    int scaleMode = 0;          // 0 = auto, 1 = major, 2 = minor (auto with a fixed tonic means major)
    bool keyLock = false;       // freeze the detected key; only the tuning grid keeps following

    int harmonyMode = HarmonyModes::ChordsAuto;
    int singOn = SingModes::All;
    float longNoteMs = 400.0f;

    int voices = 2;             // 1..3
    VoiceSettings voice[3] = { { 2, -4.0f, 0.35f, 0.0f, 0.0f, Positions::Above, Motions::Follow },     // same defaults as the plugin
                               { 8, -4.0f, -0.35f, 12.0f, 0.0f, Positions::Below, Motions::Follow },
                               { 11, -4.0f, 0.0f, 6.0f, 0.0f, Positions::Bass, Motions::Hold } };

    int stack = 1;              // copies per voice, 1..3
    float stackDetuneCents = 8.0f;
    float stackWidth = 0.5f;

    float entryLagMs = 25.0f;
    float humanize = 0.3f;      // 0..1
    float autoFormant = 0.3f;   // fraction of the harmony interval applied to the formants
    float tightness = 0.0f;     // 0 = follow the singer's inflections, 1 = harmonies exactly in tune
    float minNoteMs = 80.0f;

    float vibDepthCents = 20.0f;
    float vibRateHz = 5.5f;
    float vibDelayMs = 250.0f;

    float consonants = 0.7f;    // how much sibilants and breaths are pulled out of the harmonies

    float dryDb = 0.0f;
    float wetDb = 0.0f;
};

// Lookahead harmonizer. The output is the input delayed by the lookahead time; analysis
// runs at the input, so every decision about a note (its key, its chord, its length, whether
// it is a passing tone) is made with that much of the future already known. Live mode keeps
// only the ~30 ms the pitch analysis itself needs.
class HarmonyEngine
{
public:
    static constexpr double maxLookaheadSeconds = 2.0;
    static constexpr double liveLatencySeconds = 0.03;
    static constexpr int maxInstances = 9;   // 3 voices x 3 stack copies

    struct MidiEvent
    {
        int offset;   // sample within the block
        bool on;
        int note;
    };

    void prepare (double sampleRate, double lookaheadSeconds);
    void setLookahead (double seconds);      // <= 0 selects live mode. No allocation; clears all state
    void reset();
    int getLatencySamples() const { return latency; }
    bool isLive() const { return live; }

    // test/debug: what the engine decided for each stored note
    struct NoteInfo
    {
        double start, end;          // seconds
        float midi, tuning;         // sung pitch and the grid it was named on
        int name;                   // note name on that grid
        bool harmonized;
        Chords::Chord chord;        // on the grid
        float target[3];            // harmony notes on the grid (sounding pitch = target + tuning)
        bool voiceOn[3];
    };

    template <typename Fn>
    void forEachNote (Fn&& fn) const   // oldest first
    {
        for (i64 i = std::max<i64> (0, numNotes - (i64) notes.size() + 1); i < numNotes; ++i)
        {
            const auto& n = note (i);
            NoteInfo info { (double) n.start / sr, (double) noteEnd (n) / sr, n.midi, n.tuning, (int) std::lround (n.midi - n.tuning),
                            n.harmDone, n.chord, { n.target[0], n.target[1], n.target[2] }, { n.voiceOn[0], n.voiceOn[1], n.voiceOn[2] } };
            fn (info);
        }
    }

    // in/out may alias. numIn, numOut: 1 or 2. MIDI events must be sorted by offset.
    void process (const float* const* in, int numIn, float* const* out, int numOut, int numSamples, const HarmonySettings& s,
                  const MidiEvent* midi = nullptr, int numMidi = 0);

    // status for the editor (written on the audio thread)
    std::atomic<int> statusRoot { -1 };      // major-scale root, -1 until a key is known
    std::atomic<bool> statusMinor { false };
    std::atomic<float> statusLeadMidi { 0.0f }; // 0 when unvoiced
    std::atomic<float> statusTuning { 0.0f };   // singer's offset from A440, semitones
    std::atomic<float> statusTarget[3] { { 0.0f }, { 0.0f }, { 0.0f } };
    std::atomic<int> statusChordRoot { -1 }, statusChordQuality { 0 };
    std::atomic<uint16_t> statusChordMask { 0 };

private:
    using i64 = int64_t;

    struct Frame
    {
        float f0 = 0.0f, midi = 0.0f, rms = 0.0f, cons = 0.0f, aperiodicity = 1.0f, brightness = 0.0f;
        bool voiced = false;
    };

    struct Mark
    {
        double t = 0.0;
        float period = 0.0f;
        bool voiced = false;
    };

    struct Note
    {
        static constexpr int maxPitches = 256;   // ~1.4 s of frames at 48 kHz; longer notes keep their first part

        i64 start = 0, end = -1;            // end = -1 while the note is still sounding at the input
        float midi = 0.0f;                  // the note's pitch: median of its settled part
        float tuning = 0.0f;                // singer's tuning grid (offset from A440, semitones) the note was named on
        float pitches[maxPitches] {};
        int numPitches = 0;

        // harmony for all voices, cached the first time any voice reads this note
        bool harmDone = false;
        uint32_t harmSig = 0;               // settings it was computed with
        Chords::Chord chord;                // chord modes
        int chordDegree = -1;               // diatonic degree when chosen automatically
        float target[3] {};                 // harmony notes on the singer's grid
        int leadRef[3] {};                  // note the lead was heard as, per voice
        bool voiceOn[3] {};

        // Sing On: once a held tone starting at this note is sung, it is sung to its end
        mutable uint32_t singLatch = 0;     // settings it was latched with, 0 = not latched

        // per instance: entry timing, cached when the instance reaches this note
        bool entryDone[maxInstances] {};
        float lag[maxInstances] {}, catchUp[maxInstances] {};
        float jitLag[maxInstances] {}, jitDet[maxInstances] {};
    };

    struct ChordEntry
    {
        i64 t = 0;
        int notes[8] {};
        int count = 0;
    };

    struct Instance
    {
        bool active = false;
        double nextMark = -1.0;
        std::vector<float> ola;
        float shift = 0.0f, gain = 0.0f, formantInterval = 0.0f, vibPhase = 0.0f, drift = 0.0f, driftTarget = 0.0f;
        float panL = 0.0f, panR = 0.0f;
        bool primed = false;
    };

    // ---- analysis ----
    void analyse();
    void finaliseFrame (i64 j);
    void segmentNotes (i64 j, const Frame& f);
    void openNote (i64 start, float midi);
    void closeNote (i64 end);
    void computeKey (i64 frameIndex);
    void placeMarks();
    void handleMidi (const MidiEvent& e, i64 t);

    // ---- lookups ----
    const Frame& frameAt (double t) const;
    float consonanceAt (double t) const;
    int keyAt (double t) const;
    bool keyMinorAt (double t) const;
    float tuningAt (double t) const;
    void scaleAt (double t, const HarmonySettings& s, int& majorRoot, int& tonic, bool& minor) const;
    i64 noteIndexAt (double t) const;       // last note starting at or before t, -1 if none
    Note& note (i64 i) { return notes[(size_t) (i & noteMask)]; }
    const Note& note (i64 i) const { return notes[(size_t) (i & noteMask)]; }
    bool noteValid (i64 i) const { return i >= 0 && i < numNotes && i >= numNotes - (i64) notes.size() + 1; }
    i64 noteEnd (const Note& n) const { return n.end >= 0 ? n.end : analysedUpTo; }
    const ChordEntry* midiChordAt (double t) const;
    i64 nearestMark (double t) const;
    float sourceAt (double t) const;

    // ---- harmony ----
    void harmonize (i64 noteIdx, const HarmonySettings& s, int depth = 0);
    int chooseDegree (i64 noteIdx, int prevDegree, const Chords::Chord chords[], float grid);
    void voiceChord (const Note* prev, int lead, int prevLead, const Chords::Chord& chord, const HarmonySettings& s,
                     float out[3]) const;
    bool noteSings (i64 noteIdx, double t, const HarmonySettings& s) const;
    void toneSpan (i64 noteIdx, i64& first, i64& last) const;
    bool isPhraseEnd (i64 first, i64 last) const;
    static uint32_t signature (const HarmonySettings& s, bool live);

    // ---- synthesis ----
    void synthesise (int instance, double genLimit, const HarmonySettings& s);
    void addGrain (Instance& inst, double ts, const Mark& m, float beta, float amp);
    float random01();

    double sr = 48000.0;
    int latency = 0, hop = 256, frameLen = 2048, maxHalf = 1024, medianRadius = 2, onsetFrames = 3;
    bool live = false;
    float uvSpacing = 240.0f;

    std::vector<float> src, dryL, dryR, scratch;
    i64 srcMask = 0, head = 0;

    PitchTracker trackerFull, trackerLive;
    PitchTracker* tracker = &trackerFull;
    std::vector<Frame> rawFrames, frames;
    std::vector<int8_t> keyTrack;
    std::vector<uint8_t> keyMinor;
    i64 frameMask = 0, numRaw = 0, numFinal = 0, keyFirst = -1, keyLast = -1;
    i64 analysedUpTo = 0;

    std::vector<Note> notes;
    i64 noteMask = 0, numNotes = 0;
    bool noteOpen = false;
    int candRun = 0, devRun = 0, uvRun = 0;
    float candMidi = 0.0f, devMidi = 0.0f;
    i64 candStart = 0, devStart = 0, uvStart = 0;

    KeyFinder keyFinder;
    int forcedRoot = -1, lockedRoot = -1;
    std::vector<float> keyTuning;
    std::vector<KeyFinder::WeightedNote> keyNotes;

    bool held[128] {};
    std::vector<ChordEntry> chordLog;
    i64 chordMask = 0, numChords = 0;

    uint32_t currentSig = 0;

    std::vector<Mark> marks;
    i64 markMask = 0, numMarks = 0;
    double lastMark = 0.0;

    Instance inst[maxInstances];
    i64 olaMask = 0;
    float dryGain = 1.0f, wetGain = 1.0f;
    uint32_t rng = 0x9E3779B9u;
};
