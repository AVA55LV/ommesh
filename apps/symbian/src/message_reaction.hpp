#ifndef OMMESH_MESSAGE_REACTION_HPP
#define OMMESH_MESSAGE_REACTION_HPP

#include "meshcore.hpp"

namespace meshcore {

using ReactionHash = std::array<std::uint8_t, 5>;

struct Reaction {
    ReactionHash target{};
    std::string emoji;
    std::string by;
    std::uint32_t stamp = 0;
};

struct TextReaction {
    ReactionHash target{};
    std::string emoji;
    std::string to;
};

ReactionHash reactionHash(std::string_view text, std::uint32_t stamp);
bool validReactionEmoji(std::string_view emoji);
Bytes sendChannelReaction(std::uint8_t channel, const Reaction& reaction);
Bytes sendDirectReaction(const Prefix& recipient, const Reaction& reaction);
std::optional<Reaction> parseChannelReaction(std::span<const std::uint8_t> response);
std::optional<TextReaction> parseDirectReaction(std::string_view text);
std::optional<TextReaction> parseChannelTextReaction(std::string_view text);

}  // namespace meshcore

#endif
