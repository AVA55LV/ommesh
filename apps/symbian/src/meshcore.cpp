#include "meshcore.hpp"

#include <algorithm>

namespace meshcore {
namespace {

constexpr std::uint8_t FromApp = '<';
constexpr std::uint8_t FromRadio = '>';
constexpr std::uint64_t PartialFrameTimeout = 1000;
constexpr std::size_t PacketTextLimit = 160;
constexpr std::uint8_t PlainText = 0;
constexpr std::uint8_t SignedText = 2;

void appendTimestamp(Bytes& bytes, std::uint32_t timestamp)
{
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(timestamp >> shift));
    }
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t start)
{
    return static_cast<std::uint32_t>(bytes[start])
        | (static_cast<std::uint32_t>(bytes[start + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[start + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[start + 3]) << 24);
}

std::string readText(std::span<const std::uint8_t> bytes)
{
    const auto end = std::find(bytes.begin(), bytes.end(), 0);
    return std::string(reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(end - bytes.begin()));
}

bool validText(std::string_view text, std::size_t limit)
{
    return !text.empty() && text.size() <= limit && text.find('\0') == std::string_view::npos;
}

bool hasType(std::span<const std::uint8_t> payload, std::uint8_t type, std::size_t minimumSize)
{
    return payload.size() >= minimumSize && payload.front() == type;
}

}

std::vector<Bytes> Decoder::feed(std::span<const std::uint8_t> chunk, std::int64_t milliseconds)
{
    if (chunk.empty()) {
        return {};
    }
    if (state_ != State::Marker && milliseconds >= lastChunk_) {
        const auto elapsed = static_cast<std::uint64_t>(milliseconds) - static_cast<std::uint64_t>(lastChunk_);
        if (elapsed >= PartialFrameTimeout) {
            reset();
        }
    }
    lastChunk_ = milliseconds;

    std::vector<Bytes> complete;
    for (const auto byte : chunk) {
        switch (state_) {
        case State::Marker:
            if (byte == FromRadio) {
                state_ = State::LengthLow;
            }
            break;
        case State::LengthLow:
            expected_ = byte;
            state_ = State::LengthHigh;
            break;
        case State::LengthHigh: {
            const std::size_t size = expected_ + (static_cast<std::size_t>(byte) << 8);
            if (size >= 1 && size <= MaxFrameSize) {
                expected_ = size;
                received_ = 0;
                state_ = State::Payload;
            } else if (expected_ == FromRadio) {
                // A marker in a rejected header may start the next frame.
                expected_ = byte;
            } else {
                state_ = byte == FromRadio ? State::LengthLow : State::Marker;
            }
            break;
        }
        case State::Payload:
            payload_[received_++] = byte;
            if (received_ == expected_) {
                complete.emplace_back(payload_.data(), payload_.data() + received_);
                state_ = State::Marker;
            }
            break;
        }
    }
    return complete;
}

void Decoder::reset()
{
    state_ = State::Marker;
}

Bytes frame(std::span<const std::uint8_t> payload)
{
    if (payload.empty() || payload.size() > MaxFrameSize) {
        return {};
    }
    Bytes bytes = {FromApp, static_cast<std::uint8_t>(payload.size()), 0};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

Bytes deviceQuery()
{
    return {0x16, 3};
}

Bytes appStart()
{
    Bytes bytes(8, 0);
    bytes.front() = 0x01;
    constexpr std::string_view name = "MeshCore Nokia";
    bytes.insert(bytes.end(), name.begin(), name.end());
    return bytes;
}

Bytes getContacts()
{
    return {0x04};
}

Bytes getChannel(std::uint8_t index)
{
    return {0x1f, index};
}

Bytes setLocation(std::int32_t latitude, std::int32_t longitude)
{
    Bytes command{14};
    for (const auto coordinate : {latitude, longitude}) {
        const auto bits = static_cast<std::uint32_t>(coordinate);
        for (unsigned shift = 0; shift < 32; shift += 8)
            command.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
    return command;
}

Bytes setChannel(std::uint8_t index, std::string_view name, const std::array<std::uint8_t, 16>& key)
{
    if (name.size() >= 32 || name.find('\0') != std::string_view::npos)
        return {};
    Bytes result(50, 0);
    result[0] = 0x20;
    result[1] = index;
    std::copy(name.begin(), name.end(), result.begin() + 2);
    std::copy(key.begin(), key.end(), result.begin() + 34);
    return result;
}

Bytes syncNext()
{
    return {0x0a};
}

Bytes sendDirect(const Prefix& recipient, std::string_view text, std::uint32_t timestamp, std::uint8_t attempt)
{
    if (!validText(text, DirectLimit)) {
        return {};
    }
    Bytes bytes = {0x02, PlainText, attempt};
    appendTimestamp(bytes, timestamp);
    bytes.insert(bytes.end(), recipient.begin(), recipient.end());
    bytes.insert(bytes.end(), text.begin(), text.end());
    return bytes;
}

Bytes resetPath(const Key& recipient)
{
    Bytes command{0x0d};
    command.insert(command.end(), recipient.begin(), recipient.end());
    return command;
}

Bytes advertise()
{
    return {0x07, 0};
}

Bytes getBattery()
{
    return {0x14};
}

Bytes sendChannel(std::uint8_t index, std::string_view text, std::uint32_t timestamp, std::size_t senderNameBytes)
{
    if (!validText(text, channelLimit(senderNameBytes))) {
        return {};
    }
    Bytes bytes = {0x03, PlainText, index};
    appendTimestamp(bytes, timestamp);
    bytes.insert(bytes.end(), text.begin(), text.end());
    return bytes;
}

std::optional<std::uint8_t> parseChannelCount(std::span<const std::uint8_t> payload)
{
    if (!hasType(payload, response::DeviceInfo, 4)) {
        return std::nullopt;
    }
    return payload[3];
}

std::string parseFirmware(std::span<const std::uint8_t> payload)
{
    return hasType(payload, response::DeviceInfo, 61)
        ? readText(payload.subspan(60, std::min<std::size_t>(20, payload.size() - 60)))
        : std::string();
}

std::optional<SelfInfo> parseSelf(std::span<const std::uint8_t> payload)
{
    constexpr std::size_t nameOffset = 58;
    if (!hasType(payload, response::SelfInfo, nameOffset)) {
        return std::nullopt;
    }
    SelfInfo result;
    std::copy_n(payload.begin() + 4, result.key.size(), result.key.begin());
    result.name = readText(payload.subspan(nameOffset));
    result.powerDbm = static_cast<std::int8_t>(payload[2]);
    result.maxPowerDbm = static_cast<std::int8_t>(payload[3]);
    result.latitude = static_cast<std::int32_t>(readU32(payload, 36));
    result.longitude = static_cast<std::int32_t>(readU32(payload, 40));
    result.multiAcks = payload[44];
    result.advertLocPolicy = payload[45];
    result.telemetryModes = payload[46];
    result.manualAddContacts = payload[47];
    result.frequencyKhz = readU32(payload, 48);
    result.bandwidthHz = readU32(payload, 52);
    result.spreadingFactor = payload[56];
    result.codingRate = payload[57];
    return result;
}

std::optional<Contact> parseContact(std::span<const std::uint8_t> payload)
{
    constexpr std::size_t recordSize = 148;
    if (!hasType(payload, response::Contact, recordSize) && !hasType(payload, push::NewContact, recordSize)) {
        return std::nullopt;
    }
    Contact result;
    std::copy_n(payload.begin() + 1, result.key.size(), result.key.begin());
    result.type = payload[33];
    result.flags = payload[34];
    result.name = readText(payload.subspan(100, 32));
    result.path = payload[35];
    std::copy_n(payload.begin() + 36, result.pathHashes.size(), result.pathHashes.begin());
    result.lastAdvert = readU32(payload, 132);
    result.latitude = static_cast<std::int32_t>(readU32(payload, 136));
    result.longitude = static_cast<std::int32_t>(readU32(payload, 140));
    return result;
}

std::optional<Key> parseDeletedKey(std::span<const std::uint8_t> payload)
{
    Key key;
    if (!hasType(payload, push::ContactDeleted, 1 + key.size())) {
        return std::nullopt;
    }
    std::copy_n(payload.begin() + 1, key.size(), key.begin());
    return key;
}

std::optional<Channel> parseChannel(std::span<const std::uint8_t> payload)
{
    if (!hasType(payload, response::ChannelInfo, 50)) {
        return std::nullopt;
    }
    Channel result;
    result.index = payload[1];
    result.name = readText(payload.subspan(2, 32));
    std::copy_n(payload.begin() + 34, result.key.size(), result.key.begin());
    result.configured = !result.name.empty() || std::any_of(result.key.begin(), result.key.end(), [](std::uint8_t byte) {
        return byte != 0;
    });
    return result;
}

std::optional<Incoming> parseIncoming(std::span<const std::uint8_t> payload)
{
    if (payload.empty()) {
        return std::nullopt;
    }
    const auto code = payload.front();
    const bool direct = code == response::ContactMessage || code == response::ContactMessageV3;
    const bool channel = code == response::ChannelMessage || code == response::ChannelMessageV3;
    if (!direct && !channel) {
        return std::nullopt;
    }
    const bool version3 = code == response::ContactMessageV3 || code == response::ChannelMessageV3;
    const std::size_t addressOffset = version3 ? 4 : 1;
    const std::size_t addressSize = channel ? 1 : Prefix{}.size();
    const std::size_t typeOffset = addressOffset + addressSize + 1; // After the path byte.
    std::size_t textOffset = typeOffset + 5; // Type and four-byte timestamp.
    if (payload.size() < textOffset) {
        return std::nullopt;
    }
    const auto type = payload[typeOffset];
    if (direct && type == SignedText) {
        textOffset += 4; // A room post carries a four-byte author prefix.
    } else if (type != PlainText && !(direct && type == 1)) {
        return std::nullopt;
    }
    if (payload.size() <= textOffset) {
        return std::nullopt;
    }
    Incoming result;
    result.channel = channel;
    result.textType = type;
    result.path = payload[typeOffset - 1];
    if (version3)
        result.snrQuarterDb = static_cast<std::int8_t>(payload[1]);
    if (channel) {
        result.channelIndex = payload[addressOffset];
    } else {
        std::copy_n(payload.begin() + addressOffset, addressSize, result.sender.begin());
        if (type == SignedText) {
            result.signer.emplace();
            std::copy_n(payload.begin() + textOffset - result.signer->size(),
                        result.signer->size(), result.signer->begin());
        }
    }
    result.timestamp = readU32(payload, typeOffset + 1);
    result.text = readText(payload.subspan(textOffset));
    return result;
}

std::optional<std::uint32_t> parseSentTag(std::span<const std::uint8_t> payload)
{
    if (!hasType(payload, response::Sent, 10)) {
        return std::nullopt;
    }
    return readU32(payload, 2);
}

std::optional<std::uint32_t> parseSentTimeout(std::span<const std::uint8_t> payload)
{
    if (!hasType(payload, response::Sent, 10)) {
        return std::nullopt;
    }
    return readU32(payload, 6);
}

std::optional<std::uint32_t> parseConfirmedTag(std::span<const std::uint8_t> payload)
{
    if (!hasType(payload, push::SendConfirmed, 9)) {
        return std::nullopt;
    }
    return readU32(payload, 1);
}

std::optional<BatteryInfo> parseBattery(std::span<const std::uint8_t> payload)
{
    if (!hasType(payload, response::Battery, 3))
        return std::nullopt;
    BatteryInfo result;
    result.millivolts = static_cast<unsigned>(payload[1]) | (static_cast<unsigned>(payload[2]) << 8);
    if (payload.size() >= 11) {
        result.usedKb = readU32(payload, 3);
        result.totalKb = readU32(payload, 7);
    }
    return result;
}

Prefix prefix(const Key& key)
{
    Prefix result;
    std::copy_n(key.begin(), result.size(), result.begin());
    return result;
}

std::size_t channelLimit(std::size_t senderNameBytes)
{
    constexpr std::size_t delimiters = 2;
    const auto spaceBeforeName = PacketTextLimit - delimiters;
    return senderNameBytes <= spaceBeforeName ? spaceBeforeName - senderNameBytes : 0;
}

}
