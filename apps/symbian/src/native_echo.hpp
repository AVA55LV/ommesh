#ifndef OMMESH_NATIVE_ECHO_HPP
#define OMMESH_NATIVE_ECHO_HPP

#include "meshcore.hpp"

namespace meshcore {

std::array<std::uint8_t, 32> sha256Digest(std::span<const std::uint8_t> bytes);

struct HeardGroup {
    std::int8_t snrQuarterDb = 0;
    std::uint8_t hashWidth = 0;
    std::uint8_t hops = 0;
    Bytes path;
    Bytes payload;
};

// `name` is the normalized UTF-8 channel name, including its leading '#'.
std::array<std::uint8_t, 16> hashtagChannelKey(std::string_view name);

// `prefixedText` is the original UTF-8 "radio name: message" from MeshCore.
Bytes groupTextPayload(std::span<const std::uint8_t> secret, std::uint32_t timestamp,
                       std::string_view prefixedText);
std::optional<HeardGroup> parseGroupEcho(std::span<const std::uint8_t> frame);

}  // namespace meshcore

#endif
