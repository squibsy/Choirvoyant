#pragma once

// Fits a diatonic scale *and* the singer's tuning grid to the sung notes at once.
//
// For each scale root and each grid offset (5-cent steps), it measures how far the notes sit from
// that scale's notes, and combines that with the Krumhansl-Kessler key profiles (which pick the
// tonic). Fitting both together matters for real singers: an untrained voice may land its third
// 3.8 semitones above the tonic, which rounding to semitones first would misname, but which still
// sits closest to the right scale note.
//
// Two windows of notes come in. The long one (several seconds) decides the key and lets the grid
// follow the singer's drift slowly; a melody fragment of three or four notes fits several keys, so
// a short memory alone wanders. The short one (about a second, plus the lookahead) only watches for
// contradictions: notes clearly outside the current scale. Then the key moves to the best fit of
// what is being sung now, so real key changes are still followed within a note or two.
//
// A major key and its relative minor share one scale, so the harmonizer only needs the root;
// the minor flag is for display.
class KeyFinder
{
public:
    struct WeightedNote
    {
        float midi;     // sung pitch, fractional MIDI
        float weight;   // seconds of singing, kernel-weighted
    };

    void reset();

    // forcedRoot >= 0 fixes the scale (0 = C) and only fits the singer's tuning grid to it
    void update (const WeightedNote* longNotes, int numLong, const WeightedNote* shortNotes, int numShort,
                 float updatesPerSecond, int forcedRoot = -1);

    int root() const { return current; }          // 0 = C major / A minor, on the singer's grid
    float tuning() const { return grid; }         // singer's grid offset from A440, semitones (unwrapped)
    bool minor() const { return currentMinor; }

private:
    struct Fit
    {
        float score, outside;
        bool minor;
    };
    static Fit evaluate (const WeightedNote* notes, int numNotes, int root, float offset);
    static void search (const WeightedNote* notes, int numNotes, float centre, float range, int& bestRoot, float& bestOffset,
                        float& bestScore);

    int current = 0, candidate = -1, candidateRun = 0;
    float grid = 0.0f, candidateGrid = 0.0f;
    bool currentMinor = false, established = false;
};
