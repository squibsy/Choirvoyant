#include "PitchTracker.h"

#include <algorithm>
#include <cmath>

void PitchTracker::prepare (double sampleRate, int frameLength, float minHz, float maxHz)
{
    sr = sampleRate;
    frameLen = frameLength;
    integrationLen = frameLength / 2;
    fftSize = frameLength * 2;
    tauMax = std::min (integrationLen - 2, (int) std::ceil (sr / minHz));
    tauMin = std::max (2, (int) std::floor (sr / maxHz));

    int bits = 0;
    while ((1 << bits) < fftSize)
        ++bits;

    bitReverse.resize ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        bitReverse[(size_t) i] = r;
    }

    twiddles.resize ((size_t) fftSize / 2);
    for (int i = 0; i < fftSize / 2; ++i)
        twiddles[(size_t) i] = std::polar (1.0f, (float) (-2.0 * 3.14159265358979323846 * i / fftSize));

    spectrumA.assign ((size_t) fftSize, {});
    spectrumB.assign ((size_t) fftSize, {});
    centred.assign ((size_t) frameLen, 0.0f);
    energyPrefix.assign ((size_t) frameLen + 1, 0.0f);
    cmnd.assign ((size_t) integrationLen, 1.0f);
}

void PitchTracker::fft (std::complex<float>* data, bool inverse) const
{
    for (int i = 0; i < fftSize; ++i)
        if (i < bitReverse[(size_t) i])
            std::swap (data[i], data[bitReverse[(size_t) i]]);

    for (int len = 2; len <= fftSize; len <<= 1)
    {
        const int half = len / 2, stride = fftSize / len;
        for (int start = 0; start < fftSize; start += len)
            for (int k = 0; k < half; ++k)
            {
                auto w = twiddles[(size_t) (k * stride)];
                if (inverse)
                    w = std::conj (w);
                const auto a = data[start + k];
                const auto b = data[start + k + half] * w;
                data[start + k] = a + b;
                data[start + k + half] = a - b;
            }
    }
}

PitchTracker::Result PitchTracker::analyse (const float* frame)
{
    Result res;

    double mean = 0.0;
    for (int i = 0; i < frameLen; ++i)
        mean += frame[i];
    mean /= frameLen;

    double energy = 0.0, diffEnergy = 0.0;
    energyPrefix[0] = 0.0f;
    for (int i = 0; i < frameLen; ++i)
    {
        const float x = (float) (frame[i] - mean);
        centred[(size_t) i] = x;
        energy += (double) x * x;
        energyPrefix[(size_t) i + 1] = (float) energy;
        if (i > 0)
        {
            const double d = x - centred[(size_t) i - 1];
            diffEnergy += d * d;
        }
    }

    res.rms = (float) std::sqrt (energy / frameLen);
    res.brightness = energy > 1e-12 ? (float) (diffEnergy / energy) : 0.0f;
    if (energy < 1e-9)
        return res;

    // r(tau) = sum_{j < W} x[j] x[j + tau]  via  IFFT(FFT(x) * conj(FFT(x[0..W))))
    for (int i = 0; i < fftSize; ++i)
    {
        spectrumA[(size_t) i] = { i < frameLen ? centred[(size_t) i] : 0.0f, 0.0f };
        spectrumB[(size_t) i] = { i < integrationLen ? centred[(size_t) i] : 0.0f, 0.0f };
    }
    fft (spectrumA.data(), false);
    fft (spectrumB.data(), false);
    for (int i = 0; i < fftSize; ++i)
        spectrumA[(size_t) i] *= std::conj (spectrumB[(size_t) i]);
    fft (spectrumA.data(), true);

    const float scale = 1.0f / (float) fftSize;
    const float e0 = energyPrefix[(size_t) integrationLen];

    // cumulative mean normalised difference
    cmnd[0] = 1.0f;
    double running = 0.0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        const float eTau = energyPrefix[(size_t) (tau + integrationLen)] - energyPrefix[(size_t) tau];
        const float d = std::max (0.0f, e0 + eTau - 2.0f * spectrumA[(size_t) tau].real() * scale);
        running += d;
        cmnd[(size_t) tau] = running > 0.0 ? (float) (d * tau / running) : 1.0f;
    }

    constexpr float threshold = 0.15f;
    int best = -1;
    for (int tau = tauMin; tau <= tauMax; ++tau)
        if (cmnd[(size_t) tau] < threshold)
        {
            while (tau + 1 <= tauMax && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau])
                ++tau;
            best = tau;
            break;
        }

    if (best < 0)
    {
        best = tauMin;
        for (int tau = tauMin + 1; tau <= tauMax; ++tau)
            if (cmnd[(size_t) tau] < cmnd[(size_t) best])
                best = tau;
    }

    // Taking the first dip favours short periods: when a formant sits on the second harmonic
    // the half-period dip can pass the threshold. Prefer the double period if its dip is clearly deeper.
    while (cmnd[(size_t) best] > 0.04f && best * 2 + 1 <= tauMax)
    {
        int dbl = best * 2;
        for (int tau = dbl - 2; tau <= dbl + 2; ++tau)
            if (tau >= tauMin && tau <= tauMax && cmnd[(size_t) tau] < cmnd[(size_t) dbl])
                dbl = tau;
        if (cmnd[(size_t) dbl] >= 0.5f * cmnd[(size_t) best])
            break;
        best = dbl;
    }

    // parabolic interpolation around the minimum
    double tauExact = best;
    if (best > tauMin && best < tauMax)
    {
        const double a = cmnd[(size_t) best - 1], b = cmnd[(size_t) best], c = cmnd[(size_t) best + 1];
        const double denom = a - 2.0 * b + c;
        if (std::abs (denom) > 1e-9)
            tauExact += std::clamp (0.5 * (a - c) / denom, -0.5, 0.5);
    }

    res.aperiodicity = cmnd[(size_t) best];
    res.f0 = (float) (sr / tauExact);
    return res;
}
