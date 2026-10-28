#pragma once

#include <complex>
#include <vector>

// YIN pitch detector (de Cheveigne & Kawahara 2002) for one analysis frame.
// The difference function is computed through an FFT autocorrelation, so a
// 2048-sample frame costs two 4096-point FFTs instead of ~1M multiply-adds.
class PitchTracker
{
public:
    struct Result
    {
        float f0 = 0.0f;           // Hz, 0 when no period was found in range
        float aperiodicity = 1.0f; // CMND minimum: ~0 for a clean periodic tone, ~1 for noise
        float rms = 0.0f;
        float brightness = 0.0f;   // first-difference energy / energy: rises with high-frequency content
    };

    void prepare (double sampleRate, int frameLength, float minHz, float maxHz);
    Result analyse (const float* frame); // reads frameLength samples

    int getFrameLength() const { return frameLen; }

private:
    void fft (std::complex<float>* data, bool inverse) const;

    double sr = 48000.0;
    int frameLen = 2048, integrationLen = 1024, fftSize = 4096;
    int tauMin = 2, tauMax = 512;
    std::vector<int> bitReverse;
    std::vector<std::complex<float>> twiddles, spectrumA, spectrumB;
    std::vector<float> centred, energyPrefix, cmnd;
};
