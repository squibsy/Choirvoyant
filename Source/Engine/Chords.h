#pragma once

#include <cstdint>

// Chord vocabulary for the harmonizer: the diatonic triads of a key (with the harmonic-minor
// dominant in minor keys), functional transition costs for choosing a progression, and
// recognition and naming of chords played on MIDI.
namespace Chords
{
    enum Quality : int8_t { Major, Minor, Diminished, Augmented, Sus2, Sus4, Dom7, Maj7, Min7, HalfDim7, Dim7, Other };

    struct Chord
    {
        int root = -1;          // pitch class, -1 = no chord
        uint16_t mask = 0;      // pitch classes present (bit 0 = C)
        Quality quality = Other;

        bool valid() const { return root >= 0 && mask != 0; }
        bool has (int pc) const { return (mask >> (((pc % 12) + 12) % 12)) & 1u; }
    };

    constexpr int numDiatonic = 7;

    // Triads on degrees 1..7 of the key on `tonic`. Major: I ii iii IV V vi vii°.
    // Minor: i ii° III iv V VI VII, with V major (raised leading tone).
    void diatonic (int tonic, bool minor, Chord out[numDiatonic]);

    // Cost of moving from degree a to degree b (0-based, relative to the tonic); 0 when a == b.
    float transitionCost (int from, int to);

    // How unusual a chord is in common progressions (I and IV/V cheapest).
    float priorCost (int degree);

    // Recognises held MIDI notes. Root and quality come from the best-matching template;
    // the mask is always the notes as played.
    Chord fromNotes (const int* notes, int count);

    // "C", "Am", "Bdim", "G7", ... into buf (at least 8 chars)
    void name (const Chord& c, char* buf, int bufSize);
}
