#include "message_reaction.hpp"

#include <algorithm>

namespace meshcore {
namespace {

constexpr std::uint16_t ReactionDataType = 0xff0e;
constexpr std::size_t MaxEmojiBytes = 32;
constexpr std::size_t MaxNameBytes = 32;
constexpr std::string_view Crockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

void appendStamp(Bytes& bytes, std::uint32_t stamp) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(stamp >> shift));
}

std::optional<std::vector<std::uint32_t>> decodeUtf8(std::string_view text) {
    std::vector<std::uint32_t> result;
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<std::uint8_t>(text[i++]);
        unsigned pending = 0;
        std::uint32_t point;
        std::uint32_t minimum = 0;
        if (byte < 0x80) {
            point = byte;
        } else if (byte >= 0xc2 && byte <= 0xdf) {
            point = byte & 0x1f;
            pending = 1;
            minimum = 0x80;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            point = byte & 0x0f;
            pending = 2;
            minimum = 0x800;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            point = byte & 7;
            pending = 3;
            minimum = 0x10000;
        } else {
            return std::nullopt;
        }
        if (i + pending > text.size())
            return std::nullopt;
        while (pending--) {
            const auto next = static_cast<std::uint8_t>(text[i++]);
            if ((next & 0xc0) != 0x80)
                return std::nullopt;
            point = (point << 6) | (next & 0x3f);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))
            return std::nullopt;
        result.push_back(point);
    }
    return result;
}

bool regional(std::uint32_t point) {
    return point >= 0x1f1e6 && point <= 0x1f1ff;
}

bool extension(std::uint32_t point) {
    return point == 0xfe0e || point == 0xfe0f || point == 0x20e3 ||
           (point >= 0x0300 && point <= 0x036f) || (point >= 0x1f3fb && point <= 0x1f3ff) ||
           (point >= 0xe0020 && point <= 0xe007f);
}

bool emojiBase(std::uint32_t point) {
    return point >= 0x2000 && point != 0x200d && !extension(point);
}

std::string crockford(const ReactionHash& hash) {
    unsigned value = 0;
    unsigned bits = 0;
    std::string result;
    for (const auto byte : hash) {
        value = (value << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            char ch = Crockford[(value >> bits) & 31];
            if (ch >= 'A')
                ch = static_cast<char>(ch + 'a' - 'A');
            result += ch;
        }
    }
    return result;
}

std::optional<ReactionHash> readCrockford(std::string_view text) {
    if (text.size() != 8)
        return std::nullopt;
    ReactionHash hash{};
    unsigned value = 0;
    unsigned bits = 0;
    std::size_t at = 0;
    for (auto ch : text) {
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - 'a' + 'A');
        if (ch == 'O')
            ch = '0';
        if (ch == 'I' || ch == 'L')
            ch = '1';
        const auto digit = Crockford.find(ch);
        if (digit == std::string_view::npos)
            return std::nullopt;
        value = (value << 5) | static_cast<unsigned>(digit);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            hash[at++] = static_cast<std::uint8_t>(value >> bits);
        }
    }
    return hash;
}

bool validName(std::string_view name) {
    return !name.empty() && name.size() <= MaxNameBytes && name.find('\0') == std::string_view::npos &&
        decodeUtf8(name).has_value();
}

}  // namespace

bool validReactionEmoji(std::string_view emoji) {
    if (emoji.empty())
        return true;
    if (emoji.size() > MaxEmojiBytes)
        return false;
    const auto points = decodeUtf8(emoji);
    if (!points || points->empty())
        return false;
    const auto& code = *points;
    const bool keycap = code[0] == '#' || code[0] == '*' || (code[0] >= '0' && code[0] <= '9');
    const bool variation = std::find(code.begin(), code.end(), 0xfe0f) != code.end();
    const bool keycapMark = std::find(code.begin(), code.end(), 0x20e3) != code.end();
    if (!emojiBase(code[0]) && code[0] != 0x00a9 && code[0] != 0x00ae && !keycap)
        return false;
    if (code[0] < 0x2000 && !variation && !(keycap && keycapMark))
        return false;
    for (std::size_t i = 1; i < code.size(); ++i) {
        if (extension(code[i]) || (i == 1 && regional(code[0]) && regional(code[i])))
            continue;
        if (code[i] != 0x200d || i + 1 == code.size() || !emojiBase(code[i + 1]))
            return false;
        ++i;
    }
    return true;
}

// OMMesh's experimental companion datagram protocol in packages/meshcore/src/protocol/reaction.ts.
Bytes sendChannelReaction(std::uint8_t channel, const Reaction& reaction) {
    if (!validReactionEmoji(reaction.emoji) || !validName(reaction.by))
        return {};
    Bytes command{0x3e, channel, 0xff, static_cast<std::uint8_t>(ReactionDataType & 255),
                  static_cast<std::uint8_t>(ReactionDataType >> 8), 2};
    command.insert(command.end(), reaction.target.begin(), reaction.target.end());
    appendStamp(command, reaction.stamp);
    command.push_back(static_cast<std::uint8_t>(reaction.emoji.size()));
    command.insert(command.end(), reaction.emoji.begin(), reaction.emoji.end());
    command.insert(command.end(), reaction.by.begin(), reaction.by.end());
    return command;
}

Bytes sendDirectReaction(const Prefix& recipient, const Reaction& reaction) {
    if (!validReactionEmoji(reaction.emoji))
        return {};
    const auto text = reaction.emoji + '\n' + crockford(reaction.target);
    Bytes command{0x02, 1, 0}; // CLI_DATA, which carries no ACK.
    appendStamp(command, reaction.stamp);
    command.insert(command.end(), recipient.begin(), recipient.end());
    command.insert(command.end(), text.begin(), text.end());
    return command;
}

std::optional<Reaction> parseChannelReaction(std::span<const std::uint8_t> responseFrame) {
    constexpr std::size_t header = 9;
    if (responseFrame.size() < header + 8 || responseFrame[0] != response::ChannelData ||
        responseFrame[6] != (ReactionDataType & 255) || responseFrame[7] != (ReactionDataType >> 8) ||
        responseFrame[8] != responseFrame.size() - header) {
        return std::nullopt;
    }
    const auto data = responseFrame.subspan(header);
    const bool stamped = data[0] == 2;
    const std::size_t lengthAt = 6 + (stamped ? 4 : 0);
    if ((!stamped && data[0] != 1) || data.size() <= lengthAt + 1)
        return std::nullopt;
    const auto length = data[lengthAt];
    const auto start = lengthAt + 1;
    if (length > MaxEmojiBytes || start + length >= data.size())
        return std::nullopt;
    const auto bytes = reinterpret_cast<const char*>(data.data());
    const std::string_view emoji(bytes + start, length);
    const std::string_view by(bytes + start + length, data.size() - start - length);
    if (!validReactionEmoji(emoji) || !validName(by))
        return std::nullopt;
    Reaction result;
    std::copy_n(data.begin() + 1, result.target.size(), result.target.begin());
    if (stamped) {
        for (unsigned i = 0; i < 4; ++i)
            result.stamp |= std::uint32_t(data[6 + i]) << (i * 8);
    }
    result.emoji = std::string(emoji);
    result.by = std::string(by);
    return result;
}

std::optional<TextReaction> parseDirectReaction(std::string_view text) {
    const auto newline = text.find('\n');
    if (newline == std::string_view::npos || !validReactionEmoji(text.substr(0, newline)))
        return std::nullopt;
    if (const auto hash = readCrockford(text.substr(newline + 1)))
        return TextReaction{*hash, std::string(text.substr(0, newline)), {}};
    return std::nullopt;
}

std::optional<TextReaction> parseChannelTextReaction(std::string_view text) {
    const auto newline = text.find('\n');
    const auto open = text.find("@[");
    if (newline == std::string_view::npos || open == std::string_view::npos || open >= newline)
        return std::nullopt;
    const auto close = text.find(']', open + 2);
    if (close == std::string_view::npos || close >= newline || (open > 0 && close + 1 != newline))
        return std::nullopt;
    const auto to = text.substr(open + 2, close - open - 2);
    const auto emoji = open == 0 ? text.substr(close + 1, newline - close - 1) : text.substr(0, open);
    if (!validName(to) || emoji.empty() || !validReactionEmoji(emoji))
        return std::nullopt;
    if (const auto hash = readCrockford(text.substr(newline + 1)))
        return TextReaction{*hash, std::string(emoji), std::string(to)};
    return std::nullopt;
}

}  // namespace meshcore
