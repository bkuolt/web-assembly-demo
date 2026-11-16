/**
 * @file TextureDecoderHdr.cpp
 * @brief Radiance HDR (.hdr) and OpenEXR (.exr) floating-point environment texture decoders.
 */
#include "core/Log.hpp"
#include "scene/TextureDecoder.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

namespace bgl::scene {

namespace {

constexpr size_t kMaxTextureDimension{16384};

[[nodiscard]] float HalfToFloat(uint16_t h) noexcept {
    const uint32_t sign{static_cast<uint32_t>(h & 0x8000u) << 16};
    uint32_t exp{static_cast<uint32_t>(h >> 10) & 0x1Fu};
    uint32_t mant{static_cast<uint32_t>(h & 0x03FFu)};

    uint32_t bits{0};
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 1;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x03FFu;
            bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    return std::bit_cast<float>(bits);
}

} // namespace

core::Result<DecodedHdrImage> TextureDecoder::DecodeHDR(std::span<const uint8_t> data) {
    if (data.size() < 16) {
        return std::unexpected("HDR data buffer is too small");
    }

    const std::string_view textView{reinterpret_cast<const char*>(data.data()), data.size()};
    if (!textView.starts_with("#?RADIANCE") && !textView.starts_with("#?RGBE") && !textView.starts_with("#?")) {
        return std::unexpected("Data does not start with Radiance .hdr signature (#?RADIANCE / #?RGBE)");
    }

    size_t pos{0};
    auto readLine = [&]() -> std::string_view {
        if (pos >= textView.size())
            return {};
        const size_t nl{textView.find('\n', pos)};
        if (nl == std::string_view::npos) {
            const std::string_view line{textView.substr(pos)};
            pos = textView.size();
            return line;
        }
        std::string_view line{textView.substr(pos, nl - pos)};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        pos = nl + 1;
        return line;
    };

    // 1. Read header lines until empty line
    while (pos < textView.size()) {
        const std::string_view line{readLine()};
        if (line.empty())
            break;
    }

    // 2. Read resolution line: "-Y <height> +X <width>" or "+X <width> -Y <height>"
    const std::string_view resLine{readLine()};
    if (resLine.empty()) {
        return std::unexpected("Malformed Radiance .hdr file: missing resolution string");
    }

    uint32_t width{0};
    uint32_t height{0};
    {
        auto nextToken = [](std::string_view& s) noexcept -> std::string_view {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
                s.remove_prefix(1);
            }
            const size_t end = s.find_first_of(" \t");
            const std::string_view tok = (end == std::string_view::npos) ? s : s.substr(0, end);
            s.remove_prefix(tok.size());
            return tok;
        };

        std::string_view cursor{resLine};
        bool foundX{false};
        bool foundY{false};
        while (!cursor.empty()) {
            const std::string_view axis = nextToken(cursor);
            const std::string_view valStr = nextToken(cursor);
            if (axis.empty() || valStr.empty()) {
                break;
            }
            uint32_t parsed{0};
            const auto [ptr, ec] = std::from_chars(valStr.data(), valStr.data() + valStr.size(), parsed);
            if (ec != std::errc{} || ptr != valStr.data() + valStr.size()) {
                return std::unexpected("Failed to parse Radiance .hdr width/height");
            }
            if (axis.size() == 2 && (axis[0] == '+' || axis[0] == '-')) {
                if (axis[1] == 'Y') {
                    height = parsed;
                    foundY = true;
                } else if (axis[1] == 'X') {
                    width = parsed;
                    foundX = true;
                }
            }
        }
        if (!foundX || !foundY) {
            return std::unexpected("Malformed Radiance .hdr resolution string");
        }
    }

    if (width == 0 || height == 0 || width > kMaxTextureDimension || height > kMaxTextureDimension) {
        return std::unexpected("Radiance .hdr dimensions out of valid range");
    }

    DecodedHdrImage hdr{};
    hdr.width = width;
    hdr.height = height;
    hdr.rgbaFloat.resize(static_cast<size_t>(width) * height * 4u, 0.0f);

    auto rgbeToFloat = [](uint8_t r, uint8_t g, uint8_t b, uint8_t e, float* outRgba) noexcept {
        if (e == 0) {
            outRgba[0] = 0.0f;
            outRgba[1] = 0.0f;
            outRgba[2] = 0.0f;
            outRgba[3] = 1.0f;
        } else {
            const float scale{std::ldexp(1.0f, static_cast<int>(e) - (128 + 8))};
            outRgba[0] = (static_cast<float>(r) + 0.5f) * scale;
            outRgba[1] = (static_cast<float>(g) + 0.5f) * scale;
            outRgba[2] = (static_cast<float>(b) + 0.5f) * scale;
            outRgba[3] = 1.0f;
        }
    };

    std::vector<uint8_t> scanline(static_cast<size_t>(width) * 4u);

    for (uint32_t y{0}; y < height; ++y) {
        if (pos + 4 > data.size()) {
            return std::unexpected("Unexpected EOF while reading .hdr scanline");
        }

        const uint8_t b0{data[pos]};
        const uint8_t b1{data[pos + 1]};
        const uint8_t b2{data[pos + 2]};
        const uint8_t b3{data[pos + 3]};

        // Adaptive RLE scanline check
        if (b0 == 2 && b1 == 2 && (b2 & 0x80u) == 0 && width >= 8 && width <= 0x7FFFu) {
            const uint32_t scanW{(static_cast<uint32_t>(b2) << 8) | static_cast<uint32_t>(b3)};
            if (scanW != width) {
                return std::unexpected("Radiance .hdr RLE scanline width mismatch");
            }
            pos += 4;
            for (uint32_t ch{0}; ch < 4; ++ch) {
                uint32_t x{0};
                while (x < width) {
                    if (pos >= data.size()) {
                        return std::unexpected("Unexpected EOF in .hdr RLE channel stream");
                    }
                    const uint8_t code{data[pos++]};
                    if (code > 128) {
                        const uint32_t runCount{static_cast<uint32_t>(code - 128)};
                        if (pos >= data.size() || x + runCount > width) {
                            return std::unexpected("Corrupt RLE run in .hdr scanline");
                        }
                        const uint8_t val{data[pos++]};
                        for (uint32_t i{0}; i < runCount; ++i) {
                            scanline[(x + i) * 4u + ch] = val;
                        }
                        x += runCount;
                    } else {
                        const uint32_t litCount{static_cast<uint32_t>(code)};
                        if (litCount == 0 || pos + litCount > data.size() || x + litCount > width) {
                            return std::unexpected("Corrupt literal run in .hdr scanline");
                        }
                        for (uint32_t i{0}; i < litCount; ++i) {
                            scanline[(x + i) * 4u + ch] = data[pos++];
                        }
                        x += litCount;
                    }
                }
            }
        } else {
            // Uncompressed RGBE scanline
            const size_t rowBytes{static_cast<size_t>(width) * 4u};
            if (pos + rowBytes > data.size()) {
                return std::unexpected("Unexpected EOF in uncompressed .hdr scanline");
            }
            std::memcpy(scanline.data(), data.data() + pos, rowBytes);
            pos += rowBytes;
        }

        float* dstRow{&hdr.rgbaFloat[static_cast<size_t>(y) * width * 4u]};
        for (uint32_t x{0}; x < width; ++x) {
            const size_t xOff{static_cast<size_t>(x) * 4u};
            rgbeToFloat(scanline[xOff + 0], scanline[xOff + 1], scanline[xOff + 2], scanline[xOff + 3], &dstRow[xOff]);
        }
    }

    return hdr;
}

core::Result<DecodedHdrImage> TextureDecoder::DecodeEXR(std::span<const uint8_t> data) {
    // OpenEXR magic: 0x76, 0x2F, 0x31, 0x01
    static constexpr std::array<uint8_t, 4> kExrMagic{0x76, 0x2F, 0x31, 0x01};
    if (data.size() < 32 || std::memcmp(data.data(), kExrMagic.data(), kExrMagic.size()) != 0) {
        return std::unexpected("Data is not a valid OpenEXR (.exr) stream");
    }

    size_t pos{8}; // Skip 4-byte magic + 4-byte version field
    auto readU32 = [&](size_t offset) noexcept -> uint32_t {
        uint32_t v{0};
        std::memcpy(&v, data.data() + offset, sizeof(uint32_t));
        return v;
    };
    auto readI32 = [&](size_t offset) noexcept -> int32_t {
        int32_t v{0};
        std::memcpy(&v, data.data() + offset, sizeof(int32_t));
        return v;
    };
    auto readU64 = [&](size_t offset) noexcept -> uint64_t {
        uint64_t v{0};
        std::memcpy(&v, data.data() + offset, sizeof(uint64_t));
        return v;
    };
    auto readCString = [&](size_t& cur, size_t limit) -> std::string_view {
        const size_t start{cur};
        while (cur < limit && data[cur] != 0) {
            ++cur;
        }
        if (cur >= limit)
            return {};
        const std::string_view s{reinterpret_cast<const char*>(data.data() + start), cur - start};
        ++cur; // Skip null terminator
        return s;
    };

    struct ExrChannel {
        std::string name{};
        int32_t pixelType{1}; // 0=UINT, 1=HALF, 2=FLOAT
        size_t byteSize{2};
        int rgbaComp{-1}; // 0=R, 1=G, 2=B, 3=A
    };

    std::vector<ExrChannel> channels{};
    uint8_t compression{0};
    int32_t minX{0}, minY{0}, maxX{-1}, maxY{-1};

    // Parse OpenEXR header attributes until null byte
    while (pos < data.size()) {
        if (data[pos] == 0) {
            ++pos;
            break;
        }
        const std::string_view attrName{readCString(pos, data.size())};
        const std::string_view attrType{readCString(pos, data.size())};
        if (pos + 4 > data.size()) {
            return std::unexpected("Truncated OpenEXR attribute header");
        }
        const uint32_t attrSize{readU32(pos)};
        pos += 4;
        if (pos + attrSize > data.size()) {
            return std::unexpected("Truncated OpenEXR attribute payload");
        }

        const size_t attrEnd{pos + attrSize};
        if (attrName == "channels" && attrType == "chlist") {
            size_t chPos{pos};
            while (chPos < attrEnd && data[chPos] != 0) {
                const std::string_view chName{readCString(chPos, attrEnd)};
                if (chPos + 16 > attrEnd)
                    break;
                const int32_t pType{readI32(chPos)};
                chPos += 16; // pixelType(4) + pLinear(1) + reserved(3) + xSampling(4) + ySampling(4)

                ExrChannel ch{};
                ch.name = std::string{chName};
                ch.pixelType = pType;
                ch.byteSize = (pType == 1) ? 2u : 4u;
                if (chName == "R" || chName == "r")
                    ch.rgbaComp = 0;
                else if (chName == "G" || chName == "g")
                    ch.rgbaComp = 1;
                else if (chName == "B" || chName == "b")
                    ch.rgbaComp = 2;
                else if (chName == "A" || chName == "a")
                    ch.rgbaComp = 3;
                channels.push_back(std::move(ch));
            }
        } else if (attrName == "compression" && attrSize >= 1) {
            compression = data[pos];
        } else if (attrName == "dataWindow" && attrSize >= 16) {
            minX = readI32(pos + 0);
            minY = readI32(pos + 4);
            maxX = readI32(pos + 8);
            maxY = readI32(pos + 12);
        }
        pos = attrEnd;
    }

    if (maxX < minX || maxY < minY || channels.empty()) {
        return std::unexpected("Invalid OpenEXR dataWindow or channel list");
    }
    if (compression != 0) {
        return std::unexpected("OpenEXR compression mode " + std::to_string(compression) +
                               " not supported (use uncompressed EXR or Radiance .hdr)");
    }

    const uint32_t width{static_cast<uint32_t>(maxX - minX + 1)};
    const uint32_t height{static_cast<uint32_t>(maxY - minY + 1)};
    if (width == 0 || height == 0 || width > kMaxTextureDimension || height > kMaxTextureDimension) {
        return std::unexpected("OpenEXR dimensions exceed limits");
    }

    // Skip scanline offset table (height * uint64_t for NO_COMPRESSION)
    if (pos + static_cast<size_t>(height) * sizeof(uint64_t) > data.size()) {
        return std::unexpected("Truncated OpenEXR scanline offset table");
    }

    DecodedHdrImage hdr{};
    hdr.width = width;
    hdr.height = height;
    hdr.rgbaFloat.resize(static_cast<size_t>(width) * height * 4u, 0.0f);
    for (size_t i{0}; i < static_cast<size_t>(width) * height; ++i) {
        hdr.rgbaFloat[i * 4u + 3u] = 1.0f;
    }

    for (uint32_t lineIdx{0}; lineIdx < height; ++lineIdx) {
        const uint64_t chunkOffset{readU64(pos + static_cast<size_t>(lineIdx) * sizeof(uint64_t))};
        if (chunkOffset + 8 > data.size()) {
            return std::unexpected("OpenEXR scanline chunk offset out of bounds");
        }
        const int32_t yCoord{readI32(chunkOffset)};
        const uint32_t dataSize{readU32(chunkOffset + 4)};
        const int32_t relY{yCoord - minY};
        if (relY < 0 || static_cast<uint32_t>(relY) >= height || chunkOffset + 8 + dataSize > data.size()) {
            return std::unexpected("OpenEXR scanline payload out of bounds");
        }

        size_t cPtr{chunkOffset + 8};
        float* dstRow{&hdr.rgbaFloat[static_cast<size_t>(relY) * width * 4u]};
        for (const auto& ch : channels) {
            const size_t chRowBytes{static_cast<size_t>(width) * ch.byteSize};
            if (cPtr + chRowBytes > chunkOffset + 8 + dataSize)
                break;
            if (ch.rgbaComp >= 0 && ch.rgbaComp < 4) {
                for (uint32_t x{0}; x < width; ++x) {
                    float val{0.0f};
                    if (ch.pixelType == 1) {
                        uint16_t h{0};
                        std::memcpy(&h, data.data() + cPtr + static_cast<size_t>(x) * 2u, sizeof(uint16_t));
                        val = HalfToFloat(h);
                    } else if (ch.pixelType == 2) {
                        std::memcpy(&val, data.data() + cPtr + static_cast<size_t>(x) * 4u, sizeof(float));
                    }
                    dstRow[static_cast<size_t>(x) * 4u + static_cast<size_t>(ch.rgbaComp)] = val;
                }
            }
            cPtr += chRowBytes;
        }
    }

    return hdr;
}

core::Result<DecodedHdrImage> TextureDecoder::DecodeHdrAuto(std::span<const uint8_t> data,
                                                            std::string_view mimeOrPath) {
    if (data.empty()) {
        return std::unexpected("Cannot decode empty HDR image buffer");
    }
    static constexpr std::array<uint8_t, 4> kExrMagic{0x76, 0x2F, 0x31, 0x01};
    if (data.size() >= 4 && std::memcmp(data.data(), kExrMagic.data(), 4) == 0) {
        return DecodeEXR(data);
    }
    const std::string_view textPrefix{reinterpret_cast<const char*>(data.data()), std::min<size_t>(data.size(), 16)};
    if (textPrefix.starts_with("#?RADIANCE") || textPrefix.starts_with("#?RGBE") || textPrefix.starts_with("#?")) {
        return DecodeHDR(data);
    }
    if (mimeOrPath.ends_with(".exr") || mimeOrPath.ends_with(".EXR")) {
        return DecodeEXR(data);
    }
    return DecodeHDR(data);
}

} // namespace bgl::scene
