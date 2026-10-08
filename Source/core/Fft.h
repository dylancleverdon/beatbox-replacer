#pragma once

#include <complex>
#include <vector>

namespace bbr
{

// Minimal in-place radix-2 complex FFT. Sizes must be powers of two.
// prepare() allocates; perform() is real-time safe (no allocation, no locks).
class Fft
{
public:
    void prepare (int size);
    int getSize() const noexcept { return size; }

    // Forward transform of `data` (length getSize()), in place, unnormalised.
    void perform (std::complex<float>* data) const noexcept;

    // Convenience: forward transform of a real signal. `input` has getSize() samples,
    // `magnitudesOut` receives getSize() / 2 + 1 magnitudes |X[k]| for k = 0 .. N/2.
    // `scratch` must have getSize() elements (caller-owned so this stays allocation-free).
    void performRealMagnitudes (const float* input, float* magnitudesOut,
                                std::complex<float>* scratch) const noexcept;

    static bool isPowerOfTwo (int n) noexcept { return n > 0 && (n & (n - 1)) == 0; }
    static int nextPowerOfTwo (int n) noexcept;

private:
    int size = 0;
    int log2Size = 0;
    std::vector<std::complex<float>> twiddles; // size / 2
    std::vector<int> bitReversed;              // size
};

} // namespace bbr
