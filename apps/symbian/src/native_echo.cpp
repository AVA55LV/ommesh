#include "native_echo.hpp"
#include "message_reaction.hpp"

#include <algorithm>
#include <bit>

namespace meshcore {
namespace {

constexpr std::size_t AesBlockSize = 16;
constexpr std::size_t AesRounds = 10;
constexpr std::size_t ShaBlockSize = 64;
constexpr std::size_t GroupTextLimit = 160;
constexpr std::size_t GroupHeaderSize = 5;
constexpr std::size_t GroupCipherLimit =
    (GroupHeaderSize + GroupTextLimit + AesBlockSize - 1) / AesBlockSize * AesBlockSize;
constexpr std::size_t GroupPayloadHeader = 3;
constexpr std::size_t MaxPathBytes = 64;

using AesBlock = std::array<std::uint8_t, AesBlockSize>;
using AesKeys = std::array<std::uint8_t, AesBlockSize * (AesRounds + 1)>;
using ShaDigest = std::array<std::uint8_t, 32>;
using ShaState = std::array<std::uint32_t, 8>;

constexpr std::array<std::uint32_t, 64> ShaConstants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

constexpr std::array<std::uint8_t, 256> AesSBox{
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

void compressSha256(ShaState& state, std::span<const std::uint8_t, ShaBlockSize> block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
        const auto start = 4 * i;
        words[i] = (std::uint32_t(block[start]) << 24) | (std::uint32_t(block[start + 1]) << 16) |
                   (std::uint32_t(block[start + 2]) << 8) | block[start + 3];
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
        const auto x = words[i - 15];
        const auto y = words[i - 2];
        const auto low = std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
        const auto high = std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10);
        words[i] = words[i - 16] + low + words[i - 7] + high;
    }
    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto low = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto high = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choose = (e & f) ^ (~e & g);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto t1 = h + high + choose + ShaConstants[i] + words[i];
        const auto t2 = low + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

ShaDigest sha256(std::span<const std::uint8_t> bytes) {
    ShaState state{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    const auto bitLength = std::uint64_t(bytes.size()) * 8;
    while (bytes.size() >= ShaBlockSize) {
        compressSha256(state, bytes.first<ShaBlockSize>());
        bytes = bytes.subspan(ShaBlockSize);
    }

    std::array<std::uint8_t, 2 * ShaBlockSize> tail{};
    std::copy(bytes.begin(), bytes.end(), tail.begin());
    tail[bytes.size()] = 0x80;
    const auto paddedSize = bytes.size() < ShaBlockSize - 8 ? ShaBlockSize : tail.size();
    for (std::size_t i = 0; i < 8; ++i) {
        tail[paddedSize - 1 - i] = static_cast<std::uint8_t>(bitLength >> (8 * i));
    }
    const auto blocks = std::span(tail);
    compressSha256(state, blocks.first<ShaBlockSize>());
    if (paddedSize > ShaBlockSize) {
        compressSha256(state, blocks.last<ShaBlockSize>());
    }

    ShaDigest digest{};
    for (std::size_t i = 0; i < state.size(); ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            digest[4 * i + j] = static_cast<std::uint8_t>(state[i] >> (24 - 8 * j));
        }
    }
    return digest;
}

ShaDigest hmacSha256(std::span<const std::uint8_t> secret, std::span<const std::uint8_t> cipher) {
    std::array<std::uint8_t, ShaBlockSize + GroupCipherLimit> inner{};
    std::array<std::uint8_t, ShaBlockSize + ShaDigest{}.size()> outer{};
    for (std::size_t i = 0; i < ShaBlockSize; ++i) {
        const auto keyByte = i < secret.size() ? secret[i] : 0;
        inner[i] = static_cast<std::uint8_t>(keyByte ^ 0x36);
        outer[i] = static_cast<std::uint8_t>(keyByte ^ 0x5c);
    }
    std::copy(cipher.begin(), cipher.end(), inner.begin() + ShaBlockSize);
    const auto innerHash = sha256(std::span(inner).first(ShaBlockSize + cipher.size()));
    std::copy(innerHash.begin(), innerHash.end(), outer.begin() + ShaBlockSize);
    return sha256(outer);
}

std::uint8_t multiplyByTwo(std::uint8_t byte) {
    return static_cast<std::uint8_t>((byte << 1) ^ ((byte & 0x80) != 0 ? 0x1b : 0));
}

AesKeys expandAesKey(std::span<const std::uint8_t, AesBlockSize> key) {
    AesKeys keys{};
    std::copy(key.begin(), key.end(), keys.begin());
    std::uint8_t roundConstant = 1;
    for (std::size_t i = AesBlockSize; i < keys.size(); i += 4) {
        auto a = keys[i - 4];
        auto b = keys[i - 3];
        auto c = keys[i - 2];
        auto d = keys[i - 1];
        if (i % AesBlockSize == 0) {
            const auto first = a;
            a = AesSBox[b] ^ roundConstant;
            b = AesSBox[c];
            c = AesSBox[d];
            d = AesSBox[first];
            roundConstant = multiplyByTwo(roundConstant);
        }
        keys[i] = keys[i - AesBlockSize] ^ a;
        keys[i + 1] = keys[i + 1 - AesBlockSize] ^ b;
        keys[i + 2] = keys[i + 2 - AesBlockSize] ^ c;
        keys[i + 3] = keys[i + 3 - AesBlockSize] ^ d;
    }
    return keys;
}

void addRoundKey(AesBlock& state, const AesKeys& keys, std::size_t round) {
    for (std::size_t i = 0; i < state.size(); ++i) {
        state[i] ^= keys[round * AesBlockSize + i];
    }
}

void mixColumns(AesBlock& state) {
    for (std::size_t i = 0; i < state.size(); i += 4) {
        const auto a = state[i];
        const auto b = state[i + 1];
        const auto c = state[i + 2];
        const auto d = state[i + 3];
        const auto sum = a ^ b ^ c ^ d;
        state[i] = static_cast<std::uint8_t>(a ^ sum ^ multiplyByTwo(a ^ b));
        state[i + 1] = static_cast<std::uint8_t>(b ^ sum ^ multiplyByTwo(b ^ c));
        state[i + 2] = static_cast<std::uint8_t>(c ^ sum ^ multiplyByTwo(c ^ d));
        state[i + 3] = static_cast<std::uint8_t>(d ^ sum ^ multiplyByTwo(d ^ a));
    }
}

void encryptBlocks(std::span<const std::uint8_t, AesBlockSize> key, std::span<std::uint8_t> blocks) {
    const auto keys = expandAesKey(key);
    while (!blocks.empty()) {
        AesBlock state{};
        std::copy_n(blocks.begin(), state.size(), state.begin());
        addRoundKey(state, keys, 0);
        for (std::size_t round = 1; round <= AesRounds; ++round) {
            AesBlock shifted{};
            for (std::size_t column = 0; column < 4; ++column) {
                for (std::size_t row = 0; row < 4; ++row) {
                    const auto source = 4 * ((column + row) % 4) + row;
                    shifted[4 * column + row] = AesSBox[state[source]];
                }
            }
            state = shifted;
            if (round < AesRounds) {
                mixColumns(state);
            }
            addRoundKey(state, keys, round);
        }
        std::copy(state.begin(), state.end(), blocks.begin());
        blocks = blocks.subspan(AesBlockSize);
    }
}

}  // namespace

std::array<std::uint8_t, 32> sha256Digest(std::span<const std::uint8_t> bytes) {
    return sha256(bytes);
}

std::array<std::uint8_t, 16> hashtagChannelKey(std::string_view name) {
    const auto digest = sha256({reinterpret_cast<const std::uint8_t*>(name.data()), name.size()});
    std::array<std::uint8_t, 16> key{};
    std::copy_n(digest.begin(), key.size(), key.begin());
    return key;
}

Bytes groupTextPayload(std::span<const std::uint8_t> secret, std::uint32_t timestamp,
                       std::string_view prefixedText) {
    if (secret.size() != 16 && secret.size() != 32) {
        return {};
    }

    // MeshCore BaseChatMesh::sendGroupMessage and Utils::encryptThenMAC:
    // timestamp, plain-text type byte, and UTF-8 text, zero-padded for AES-128.
    const auto text = prefixedText.substr(0, GroupTextLimit);
    const auto dataLength = GroupHeaderSize + text.size();
    const auto paddedLength = (dataLength + AesBlockSize - 1) / AesBlockSize * AesBlockSize;
    Bytes payload(GroupPayloadHeader + paddedLength, 0);
    auto cipher = std::span(payload).subspan(GroupPayloadHeader);
    for (std::size_t i = 0; i < 4; ++i) {
        cipher[i] = static_cast<std::uint8_t>(timestamp >> (8 * i));
    }
    std::copy(text.begin(), text.end(), cipher.begin() + GroupHeaderSize);
    encryptBlocks(secret.first<AesBlockSize>(), cipher);

    // Firmware stores 128-bit keys in a zero-extended 32-byte field.
    const auto extension = secret.subspan(16);
    const bool paddedKey = std::all_of(extension.begin(), extension.end(),
                                      [](std::uint8_t byte) { return byte == 0; });
    const auto channelHash = sha256(paddedKey ? secret.first(16) : secret);
    const auto mac = hmacSha256(secret, cipher);
    payload[0] = channelHash[0];
    payload[1] = mac[0];
    payload[2] = mac[1];
    return payload;
}

ReactionHash reactionHash(std::string_view text, std::uint32_t stamp) {
    Bytes bytes(text.begin(), text.end());
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(stamp >> shift));
    const auto digest = sha256(bytes);
    ReactionHash hash{};
    std::copy_n(digest.begin(), hash.size(), hash.begin());
    return hash;
}

std::optional<HeardGroup> parseGroupEcho(std::span<const std::uint8_t> frame) {
    constexpr std::uint8_t rawRxFrame = 0x88;
    constexpr std::uint8_t groupTextType = 5;
    constexpr std::size_t rawHeaderOffset = 3;
    if (frame.size() <= rawHeaderOffset || frame.size() > MaxFrameSize || frame[0] != rawRxFrame) {
        return std::nullopt;
    }
    const auto header = frame[rawHeaderOffset];
    const auto routeType = header & 3;
    if (((header >> 2) & 15) != groupTextType || routeType > 1) {
        return std::nullopt;
    }
    // A transport flood includes two 16-bit transport codes before path length.
    const std::size_t pathOffset = routeType == 0 ? 9 : 5;
    if (frame.size() < pathOffset) {
        return std::nullopt;
    }
    const auto pathLength = frame[pathOffset - 1];
    const auto hops = static_cast<std::uint8_t>(pathLength & 63);
    const auto hashWidth = static_cast<std::uint8_t>((pathLength >> 6) + 1);
    const std::size_t pathBytes = hops * hashWidth;
    if (pathBytes > MaxPathBytes || pathOffset + pathBytes > frame.size()) {
        return std::nullopt;
    }
    const auto cipher = frame.subspan(pathOffset + pathBytes);
    if (cipher.size() < GroupPayloadHeader + AesBlockSize ||
        (cipher.size() - GroupPayloadHeader) % AesBlockSize != 0) {
        return std::nullopt;
    }
    const auto path = frame.subspan(pathOffset, pathBytes);
    return HeardGroup{std::bit_cast<std::int8_t>(frame[1]), hashWidth, hops,
                      Bytes(path.begin(), path.end()), Bytes(cipher.begin(), cipher.end())};
}

}  // namespace meshcore
