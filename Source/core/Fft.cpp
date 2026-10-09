#include "Fft.h"

#include <cmath>
#include <utility>

namespace bbr
{

int Fft::nextPowerOfTwo (int n) noexcept
{
    int p = 1;

    while (p < n && p < (1 << 30))
        p <<= 1;

    return p;
}

void Fft::prepare (int newSize)
{
    size = nextPowerOfTwo (newSize);
    log2Size = 0;

    while ((1 << log2Size) < size)
        ++log2Size;

    constexpr double twoPi = 6.283185307179586476925286766559;
    twiddles.resize ((size_t) (size / 2));

    for (int k = 0; k < size / 2; ++k)
    {
        const double angle = -twoPi * (double) k / (double) size;
        twiddles[(size_t) k] = std::complex<float> ((float) std::cos (angle), (float) std::sin (angle));
    }

    bitReversed.resize ((size_t) size);

    for (int i = 0; i < size; ++i)
    {
        int reversed = 0;

        for (int bit = 0; bit < log2Size; ++bit)
            if ((i & (1 << bit)) != 0)
                reversed |= 1 << (log2Size - 1 - bit);

        bitReversed[(size_t) i] = reversed;
    }
}

void Fft::perform (std::complex<float>* data) const noexcept
{
    if (data == nullptr || size < 2)
        return;

    for (int i = 0; i < size; ++i)
    {
        const int j = bitReversed[(size_t) i];

        if (i < j)
            std::swap (data[i], data[j]);
    }

    // std::complex<T> is layout-compatible with T[2]; plain floats keep the butterfly fast.
    float* const d = reinterpret_cast<float*> (data);
    const float* const tw = reinterpret_cast<const float*> (twiddles.data());

    for (int len = 2; len <= size; len <<= 1)
    {
        const int half = len / 2;
        const int twiddleStep = size / len;

        for (int start = 0; start < size; start += len)
        {
            float* const lower = d + 2 * start;
            float* const upper = lower + 2 * half;

            for (int k = 0; k < half; ++k)
            {
                const float wr = tw[2 * k * twiddleStep];
                const float wi = tw[2 * k * twiddleStep + 1];
                const float br = upper[2 * k];
                const float bi = upper[2 * k + 1];
                const float tr = br * wr - bi * wi;
                const float ti = br * wi + bi * wr;
                const float ar = lower[2 * k];
                const float ai = lower[2 * k + 1];

                lower[2 * k] = ar + tr;
                lower[2 * k + 1] = ai + ti;
                upper[2 * k] = ar - tr;
                upper[2 * k + 1] = ai - ti;
            }
        }
    }
}

void Fft::performRealMagnitudes (const float* input, float* magnitudesOut,
                                 std::complex<float>* scratch) const noexcept
{
    if (input == nullptr || magnitudesOut == nullptr || scratch == nullptr || size < 1)
        return;

    for (int i = 0; i < size; ++i)
        scratch[i] = std::complex<float> (input[i], 0.0f);

    perform (scratch);

    for (int k = 0; k <= size / 2; ++k)
    {
        const float re = scratch[k].real();
        const float im = scratch[k].imag();
        magnitudesOut[k] = std::sqrt (re * re + im * im);
    }
}

} // namespace bbr
