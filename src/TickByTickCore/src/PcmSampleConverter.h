#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace tickbytick::pcm {

enum class SampleFormat : std::uint8_t {
    Unknown = 0,
    Float32,
    Int16,
    Int24,
    Int32,
};

inline bool IsIntegerFormat(SampleFormat format) noexcept {
    return format == SampleFormat::Int16 ||
           format == SampleFormat::Int24 ||
           format == SampleFormat::Int32;
}

inline std::uint32_t BytesPerSample(SampleFormat format) noexcept {
    switch (format) {
        case SampleFormat::Int16:
            return 2;
        case SampleFormat::Int24:
            return 3;
        case SampleFormat::Float32:
        case SampleFormat::Int32:
            return 4;
        default:
            return 0;
    }
}

inline std::uint32_t IntegerBits(SampleFormat format) noexcept {
    switch (format) {
        case SampleFormat::Int16:
            return 16;
        case SampleFormat::Int24:
            return 24;
        case SampleFormat::Int32:
            return 32;
        default:
            return 0;
    }
}

inline std::int64_t ReadIntegerSample(const std::uint8_t* source,
                                      SampleFormat format) noexcept {
    if (source == nullptr) {
        return 0;
    }

    switch (format) {
        case SampleFormat::Int16: {
            std::int16_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            return value;
        }
        case SampleFormat::Int24: {
            std::uint32_t packed = static_cast<std::uint32_t>(source[0]) |
                    (static_cast<std::uint32_t>(source[1]) << 8U) |
                    (static_cast<std::uint32_t>(source[2]) << 16U);
            if ((packed & 0x00800000U) != 0) {
                packed |= 0xFF000000U;
            }
            std::int32_t value = 0;
            std::memcpy(&value, &packed, sizeof(value));
            return value;
        }
        case SampleFormat::Int32: {
            std::int32_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            return value;
        }
        default:
            return 0;
    }
}

inline void WriteIntegerSample(std::uint8_t* destination,
                               SampleFormat format,
                               std::int64_t value) noexcept {
    if (destination == nullptr) {
        return;
    }

    switch (format) {
        case SampleFormat::Int16: {
            const auto converted = static_cast<std::int16_t>(value);
            std::memcpy(destination, &converted, sizeof(converted));
            break;
        }
        case SampleFormat::Int24: {
            const auto converted = static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(value));
            destination[0] = static_cast<std::uint8_t>(converted & 0xFFU);
            destination[1] = static_cast<std::uint8_t>((converted >> 8U) & 0xFFU);
            destination[2] = static_cast<std::uint8_t>((converted >> 16U) & 0xFFU);
            break;
        }
        case SampleFormat::Int32: {
            const auto converted = static_cast<std::int32_t>(value);
            std::memcpy(destination, &converted, sizeof(converted));
            break;
        }
        default:
            break;
    }
}

inline std::int64_t ClampIntegerSample(std::int64_t value,
                                       std::uint32_t bits) noexcept {
    if (bits == 0 || bits > 32) {
        return 0;
    }
    const std::int64_t scale = std::int64_t{1} << (bits - 1U);
    const std::int64_t minimum = -scale;
    const std::int64_t maximum = scale - 1;
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

inline std::int64_t ConvertIntegerSample(std::int64_t value,
                                         std::uint32_t sourceBits,
                                         std::uint32_t destinationBits) noexcept {
    if (sourceBits == 0 || destinationBits == 0 ||
        sourceBits > 32 || destinationBits > 32) {
        return 0;
    }
    if (destinationBits > sourceBits) {
        const std::uint32_t shift = destinationBits - sourceBits;
        return ClampIntegerSample(value * (std::int64_t{1} << shift),
                                  destinationBits);
    }
    if (destinationBits < sourceBits) {
        const std::uint32_t shift = sourceBits - destinationBits;
        const std::int64_t divisor = std::int64_t{1} << shift;
        const std::int64_t half = divisor / 2;
        value = value >= 0
                ? (value + half) / divisor
                : -((-value + half) / divisor);
    }
    return ClampIntegerSample(value, destinationBits);
}

inline bool ConvertSamples(const std::uint8_t* source,
                           SampleFormat sourceFormat,
                           std::uint8_t* destination,
                           SampleFormat destinationFormat,
                           std::size_t sampleCount) noexcept {
    if (sampleCount == 0) {
        return true;
    }
    if (source == nullptr || destination == nullptr) {
        return false;
    }

    const std::uint32_t sourceBytes = BytesPerSample(sourceFormat);
    const std::uint32_t destinationBytes = BytesPerSample(destinationFormat);
    if (sourceBytes == 0 || destinationBytes == 0) {
        return false;
    }
    if (sampleCount > (std::numeric_limits<std::size_t>::max)() / sourceBytes ||
        sampleCount > (std::numeric_limits<std::size_t>::max)() /
                destinationBytes) {
        return false;
    }
    if (sourceFormat == destinationFormat) {
        std::memmove(destination,
                     source,
                     sampleCount * static_cast<std::size_t>(sourceBytes));
        return true;
    }

    const bool sourceInteger = IsIntegerFormat(sourceFormat);
    const bool destinationInteger = IsIntegerFormat(destinationFormat);
    const std::uint32_t sourceBits = IntegerBits(sourceFormat);
    const std::uint32_t destinationBits = IntegerBits(destinationFormat);

    if (sourceInteger && destinationInteger) {
        for (std::size_t index = 0; index < sampleCount; ++index) {
            const std::int64_t sample = ReadIntegerSample(
                    source + index * sourceBytes, sourceFormat);
            WriteIntegerSample(
                    destination + index * destinationBytes,
                    destinationFormat,
                    ConvertIntegerSample(sample, sourceBits, destinationBits));
        }
        return true;
    }

    if (sourceFormat == SampleFormat::Float32 && destinationInteger) {
        const std::int64_t scale = std::int64_t{1} << (destinationBits - 1U);
        const std::int64_t minimum = -scale;
        const std::int64_t maximum = scale - 1;
        for (std::size_t index = 0; index < sampleCount; ++index) {
            float sample = 0.0f;
            std::memcpy(&sample,
                        source + index * sourceBytes,
                        sizeof(sample));

            std::int64_t converted = 0;
            if (std::isnan(sample)) {
                converted = 0;
            } else if (sample >= 1.0f) {
                converted = maximum;
            } else if (sample <= -1.0f) {
                converted = minimum;
            } else {
                converted = static_cast<std::int64_t>(
                        static_cast<double>(sample) * static_cast<double>(scale));
            }
            WriteIntegerSample(destination + index * destinationBytes,
                               destinationFormat,
                               converted);
        }
        return true;
    }

    if (sourceInteger && destinationFormat == SampleFormat::Float32) {
        const float scale = 1.0f /
                static_cast<float>(std::int64_t{1} << (sourceBits - 1U));
        for (std::size_t index = 0; index < sampleCount; ++index) {
            const std::int64_t sample = ReadIntegerSample(
                    source + index * sourceBytes, sourceFormat);
            const float converted = static_cast<float>(sample) * scale;
            std::memcpy(destination + index * destinationBytes,
                        &converted,
                        sizeof(converted));
        }
        return true;
    }

    return false;
}

}  // namespace tickbytick::pcm
