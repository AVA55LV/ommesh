#include "radio_key_identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

// Field arithmetic and the scalar ladder are adapted from the public-domain
// TweetNaCl 20140427 implementation by Daniel J. Bernstein et al.
// https://github.com/RIOT-OS/tweetnacl/blob/master/tweetnacl.c
using Limb = std::int64_t;
using Field = std::array<Limb, 16>;
using EncodedPoint = std::array<unsigned char, 32>;

struct ExtendedPoint {
    Field x;
    Field y;
    Field z;
    Field t;
};

constexpr Limb LimbBase = 65536;
constexpr Field One{1};
constexpr Field TwiceD{
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406
};
constexpr Field BaseX{
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169
};
constexpr Field BaseY{
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666
};

void carry(Field& value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] += LimbBase;
        const Limb carryOut = value[i] >> 16;
        // 2^256 is 38 modulo 2^255 - 19.
        if (i + 1 == value.size())
            value[0] += 38 * (carryOut - 1);
        else
            value[i + 1] += carryOut - 1;
        value[i] -= carryOut * LimbBase;
    }
}

void conditionalSwap(Field& left, Field& right, unsigned bit) {
    const Limb mask = -static_cast<Limb>(bit);
    for (std::size_t i = 0; i < left.size(); ++i) {
        const Limb difference = mask & (left[i] ^ right[i]);
        left[i] ^= difference;
        right[i] ^= difference;
    }
}

void addField(Field& out, const Field& left, const Field& right) {
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = left[i] + right[i];
}

void subtractField(Field& out, const Field& left, const Field& right) {
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = left[i] - right[i];
}

void multiplyField(Field& out, const Field& left, const Field& right) {
    std::array<Limb, 31> product{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        for (std::size_t j = 0; j < right.size(); ++j)
            product[i + j] += left[i] * right[j];
    }
    for (std::size_t i = 0; i < out.size() - 1; ++i)
        product[i] += 38 * product[i + out.size()];
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = product[i];
    carry(out);
    carry(out);
}

Field inverse(const Field& value) {
    Field out = value;
    // Raise to (2^255 - 19) - 2. The exponent is fixed and public.
    for (int bit = 253; bit >= 0; --bit) {
        multiplyField(out, out, out);
        if (bit != 2 && bit != 4)
            multiplyField(out, out, value);
    }
    return out;
}

EncodedPoint encodeField(Field value) {
    carry(value);
    carry(value);
    carry(value);
    for (int pass = 0; pass < 2; ++pass) {
        // Subtract p and select the result only if it did not underflow.
        Field reduced;
        reduced[0] = value[0] - 0xffed;
        for (std::size_t i = 1; i < 15; ++i) {
            reduced[i] = value[i] - 0xffff - ((reduced[i - 1] >> 16) & 1);
            reduced[i - 1] &= 0xffff;
        }
        reduced[15] = value[15] - 0x7fff - ((reduced[14] >> 16) & 1);
        const unsigned underflow = static_cast<unsigned>((reduced[15] >> 16) & 1);
        reduced[14] &= 0xffff;
        conditionalSwap(value, reduced, 1 - underflow);
    }

    EncodedPoint bytes;
    for (std::size_t i = 0; i < value.size(); ++i) {
        bytes[2 * i] = static_cast<unsigned char>(value[i] & 0xff);
        bytes[2 * i + 1] = static_cast<unsigned char>(value[i] >> 8);
    }
    return bytes;
}

void addPoint(ExtendedPoint& left, const ExtendedPoint& right) {
    // Complete addition on the twisted Edwards curve in extended coordinates.
    Field a, b, c, d, e, f, g, h, temporary;
    subtractField(a, left.y, left.x);
    subtractField(temporary, right.y, right.x);
    multiplyField(a, a, temporary);
    addField(b, left.x, left.y);
    addField(temporary, right.x, right.y);
    multiplyField(b, b, temporary);
    multiplyField(c, left.t, right.t);
    multiplyField(c, c, TwiceD);
    multiplyField(d, left.z, right.z);
    addField(d, d, d);
    subtractField(e, b, a);
    subtractField(f, d, c);
    addField(g, d, c);
    addField(h, b, a);

    multiplyField(left.x, e, f);
    multiplyField(left.y, h, g);
    multiplyField(left.z, g, f);
    multiplyField(left.t, e, h);
}

void conditionalSwap(ExtendedPoint& left, ExtendedPoint& right, unsigned bit) {
    conditionalSwap(left.x, right.x, bit);
    conditionalSwap(left.y, right.y, bit);
    conditionalSwap(left.z, right.z, bit);
    conditionalSwap(left.t, right.t, bit);
}

void erasePoint(ExtendedPoint& point) {
    auto* bytes = reinterpret_cast<volatile unsigned char*>(&point);
    for (std::size_t i = 0; i < sizeof(point); ++i)
        bytes[i] = 0;
}

EncodedPoint multiplyBase(const unsigned char* scalar) {
    ExtendedPoint result{{}, One, One, {}};
    ExtendedPoint next{BaseX, BaseY, One, {}};
    multiplyField(next.t, BaseX, BaseY);

    // Run a fixed-length ladder with masked swaps for the scalar bits.
    for (int index = 255; index >= 0; --index) {
        const unsigned bit = (scalar[index / 8] >> (index % 8)) & 1u;
        conditionalSwap(result, next, bit);
        addPoint(next, result);
        addPoint(result, result);
        conditionalSwap(result, next, bit);
    }

    const Field inverseZ = inverse(result.z);
    Field x, y;
    multiplyField(x, result.x, inverseZ);
    multiplyField(y, result.y, inverseZ);
    EncodedPoint publicKey = encodeField(y);
    publicKey.back() |= static_cast<unsigned char>((encodeField(x).front() & 1u) << 7);
    erasePoint(result);
    erasePoint(next);
    return publicKey;
}

}  // namespace

QByteArray deriveRadioPublicKey(const QByteArray& expandedPrivate) {
    if (expandedPrivate.size() != 64)
        return QByteArray();
    const auto* scalar = reinterpret_cast<const unsigned char*>(expandedPrivate.constData());
    if ((scalar[0] & 7u) != 0 || (scalar[31] & 0xc0u) != 0x40u)
        return QByteArray();

    const EncodedPoint publicKey = multiplyBase(scalar);
    // MeshCore reserves these prefixes. Reject them as the firmware does.
    if (publicKey.front() == 0x00 || publicKey.front() == 0xff)
        return QByteArray();
    return QByteArray(reinterpret_cast<const char*>(publicKey.data()), static_cast<int>(publicKey.size()));
}

bool radioKeyMatchesPublic(const QByteArray& expandedPrivate, const QByteArray& publicKey) {
    return publicKey.size() == 32 && deriveRadioPublicKey(expandedPrivate) == publicKey;
}
