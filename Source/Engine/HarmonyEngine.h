#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

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

struct VoiceSettings
{
    int interval = 2;
    float levelDb = -4.0f;
    float pan = 0.0f;           // -1..1
    float delayMs = 0.0f;
    float formantSemis = 0.0f;  // manual offset on top of the automatic amount
};

struct HarmonySettings
{
    int keyMode = 0;            // 0 = auto, 1..12 = fixed scale on C..B (major / relative minor)
    int voices = 2;             // 1..3
    VoiceSettings voice[3] = { { 2, -4.0f, 0.35f, 0.0f, 0.0f },     // same defaults as the plugin:
                               { 8, -4.0f, -0.35f, 12.0f, 0.0f },   // 3rd up, 4th down, octave down
                               { 11, -4.0f, 0.0f, 6.0f, 0.0f } };

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
// runs at the input, so every decision about a note (its key, its length, whether it is
// a passing tone) is made with that much of the future already known.
class HarmonyEngine
{
public:
    static constexpr double maxLookaheadSeconds = 2.0;
    static constexpr int maxInstances = 9;   // 3 voices x 3 stack copies

    void prepare (double sampleRate, double lookaheadSeconds);
    void setLookahead (double seconds);      // no allocation; clears all state
    void reset();
    int getLatencySamples() const { return latency; }

    // test/debug: the stored notes, oldest first (start/end in seconds, median MIDI pitch, note name used)
    template <typename Fn>
    void forEachNote (Fn&& fn) const
    {
        for (i64 i = std::max<i64> (0, numNotes - (i64) notes.size() + 1); i < numNotes; ++i)
        {
            const auto& n = note (i);
            fn ((double) n.start / sr, (double) noteEnd (n) / sr, n.midi, (int) std::lround (n.midi - n.tuning));
        }
    }

    // in/out may alias. numIn, numOut: 1 or 2.
    void process (const float* const* in, int numIn, float* const* out, int numOut, int numSamples, const HarmonySettings& s);

    // status for the editor (written on the audio thread)
    std::atomic<int> statusRoot { -1 };      // -1 until a key is known
    std::atomic<bool> statusMinor { false };
    std::atomic<float> statusLeadMidi { 0.0f }; // 0 when unvoiced
    std::atomic<float> statusTuning { 0.0f };   // singer's offset from A440, semitones
    std::atomic<float> statusTarget[3] { { 0.0f }, { 0.0f }, { 0.0f } };

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
        i64 start = 0, end = -1;            // end = -1 while the note is still sounding at the input
        float midi = 0.0f;
        float tuning = 0.0f;                // singer's tuning grid (offset from A440, semitones) the note was named on
        float pitches[32] {};
        int numPitches = 0;

        // per voice: harmony target, cached the first time the voice reads this note
        bool targetDone[3] {};
        int targetInterval[3] {}, targetKey[3] {};
        float target[3] {};
        int leadRef[3] {};                  // scale note the lead was heard as (before harmonizing)

        // per instance: entry timing, cached when the instance reaches this note
        bool entryDone[maxInstances] {};
        float lag[maxInstances] {}, catchUp[maxInstances] {};
        float jitLag[maxInstances] {}, jitDet[maxInstances] {};
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

    // ---- lookups ----
    const Frame& frameAt (double t) const;
    float consonanceAt (double t) const;
    int keyAt (double t) const;
    float tuningAt (double t) const;
    i64 noteIndexAt (double t) const;       // last note starting at or before t, -1 if none
    Note& note (i64 i) { return notes[(size_t) (i & noteMask)]; }
    const Note& note (i64 i) const { return notes[(size_t) (i & noteMask)]; }
    bool noteValid (i64 i) const { return i >= 0 && i < numNotes && i >= numNotes - (i64) notes.size() + 1; }
    i64 noteEnd (const Note& n) const { return n.end >= 0 ? n.end : analysedUpTo; }
    i64 nearestMark (double t) const;
    float sourceAt (double t) const;
    float targetFor (i64 noteIdx, int voice, const HarmonySettings& s, int depth = 0);

    // ---- synthesis ----
    void synthesise (int instance, double genLimit, const HarmonySettings& s);
    void addGrain (Instance& inst, double ts, const Mark& m, float beta, float amp);
    float random01();

    double sr = 48000.0;
    int latency = 0, hop = 256, frameLen = 2048, maxHalf = 1024;
    float uvSpacing = 240.0f;

    std::vector<float> src, dryL, dryR, scratch;
    i64 srcMask = 0, head = 0;

    PitchTracker tracker;
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
    int analysisKeyMode = 0;
    std::vector<float> keyTuning;
    std::vector<KeyFinder::WeightedNote> keyNotes;

    std::vector<Mark> marks;
    i64 markMask = 0, numMarks = 0;
    double lastMark = 0.0;

    Instance inst[maxInstances];
    i64 olaMask = 0;
    float dryGain = 1.0f, wetGain = 1.0f;
    uint32_t rng = 0x9E3779B9u;
};
