// Maintenance-only export of the fixed third-order AllRAD metering matrix.
// Compile against SAF v1.3.4; never linked into a production target.
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <saf_hoa.h>
#include <saf_utility_complex.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    std::array<float, 22> directions{30, 0,    -30, 0,  0,  0,   90, 0,   -90, 0,    135,
                                     0,  -135, 0,   45, 30, -45, 30, 135, 30,  -135, 30};
    std::array<float, 176> matrix{};
    std::srand(1);
    getLoudspeakerDecoderMtx(directions.data(), 11, LOUDSPEAKER_DECODER_ALLRAD, 3, 1, matrix.data());
    std::ofstream output{argv[1], std::ios::binary};
    for (std::size_t i = 0; i < matrix.size(); ++i) {
        const auto order = static_cast<int>(std::sqrt(static_cast<double>(i % 16)));
        const float value = matrix[i] * std::sqrt(static_cast<float>((2 * order) + 1));
        if (!std::isfinite(value)) {
            return 1;
        }
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            output.put(static_cast<char>((bits >> shift) & 255U));
        }
    }
    return output ? 0 : 1;
}
