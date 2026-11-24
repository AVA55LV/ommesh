#ifndef NOKIA_MESHCORE_HPP
#define NOKIA_MESHCORE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace meshcore {

using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t, 32>;
using Prefix = std::array<std::uint8_t, 6>;

constexpr std::size_t MaxFrameSize = 176;
constexpr std::size_t DirectLimit = 158;

namespace response {
constexpr std::uint8_t Ok = 0x00;
constexpr std::uint8_t Error = 0x01;
constexpr std::uint8_t ContactsStart = 0x02;
constexpr std::uint8_t Contact = 0x03;
constexpr std::uint8_t ContactsEnd = 0x04;
constexpr std::uint8_t SelfInfo = 0x05;
constexpr std::uint8_t Sent = 0x06;
constexpr std::uint8_t ContactMessage = 0x07;
constexpr std::uint8_t ChannelMessage = 0x08;
constexpr std::uint8_t NoMoreMessages = 0x0a;
constexpr std::uint8_t Battery = 0x0c;
constexpr std::uint8_t DeviceInfo = 0x0d;
constexpr std::uint8_t ContactMessageV3 = 0x10;
constexpr std::uint8_t ChannelMessageV3 = 0x11;
constexpr std::uint8_t ChannelInfo = 0x12;
constexpr std::uint8_t ChannelData = 0x1b;
}

namespace push {
constexpr std::uint8_t PathUpdated = 0x81;
constexpr std::uint8_t SendConfirmed = 0x82;
constexpr std::uint8_t MessageWaiting = 0x83;
constexpr std::uint8_t RadioLog = 0x88;
constexpr std::uint8_t NewContact = 0x8a;
constexpr std::uint8_t ContactDeleted = 0x8f;
}

struct SelfInfo {
    Key key{};
    std::string name;
    int powerDbm = 0;
    int maxPowerDbm = 0;
    std::uint32_t frequencyKhz = 0;
    std::uint32_t bandwidthHz = 0;
    int spreadingFactor = 0;
    int codingRate = 0;
    std::int32_t latitude = 0;
    std::int32_t longitude = 0;
    std::uint8_t multiAcks = 0;
    std::uint8_t advertLocPolicy = 0;
    std::uint8_t telemetryModes = 0;
    std::uint8_t manualAddContacts = 0;
};

struct Contact {
    Key key{};
    std::uint8_t type = 0;
    std::string name;
    std::uint8_t path = 0xff;
    std::uint32_t lastAdvert = 0;
    std::int32_t latitude = 0;
    std::int32_t longitude = 0;
    std::array<std::uint8_t, 64> pathHashes{};
    std::uint8_t flags = 0;
};

struct Channel {
    std::uint8_t index = 0;
    std::string name;
    std::array<std::uint8_t, 16> key{};
    // An unused slot has both an empty name and an all-zero key.
    bool configured = false;
};

struct Incoming {
    bool channel = false;
    std::uint8_t channelIndex = 0;
    Prefix sender{};
    std::uint32_t timestamp = 0;
    std::string text;
    std::uint8_t path = 0xff;
    std::optional<int> snrQuarterDb;
    std::uint8_t textType = 0;
    std::optional<std::array<std::uint8_t, 4>> signer;
};

struct BatteryInfo {
    unsigned millivolts = 0;
    std::uint32_t usedKb = 0;
    std::uint32_t totalKb = 0;
};

class Decoder {
public:
    std::vector<Bytes> feed(std::span<const std::uint8_t> chunk, std::int64_t milliseconds);
    void reset();

private:
    enum class State { Marker, LengthLow, LengthHigh, Payload };
    State state_ = State::Marker;
    std::array<std::uint8_t, MaxFrameSize> payload_{};
    std::size_t expected_ = 0;
    std::size_t received_ = 0;
    std::int64_t lastChunk_ = 0;
};

Bytes frame(std::span<const std::uint8_t> payload);
Bytes deviceQuery();
Bytes appStart();
Bytes getContacts();
Bytes getChannel(std::uint8_t index);
Bytes setChannel(std::uint8_t index, std::string_view name, const std::array<std::uint8_t, 16>& key);
Bytes syncNext();
Bytes sendDirect(const Prefix& recipient, std::string_view text, std::uint32_t timestamp, std::uint8_t attempt = 0);
Bytes sendChannel(std::uint8_t index, std::string_view text, std::uint32_t timestamp, std::size_t senderNameBytes);
Bytes resetPath(const Key& recipient);
Bytes advertise();
Bytes getBattery();

std::optional<std::uint8_t> parseChannelCount(std::span<const std::uint8_t> payload);
std::string parseFirmware(std::span<const std::uint8_t> payload);
std::optional<SelfInfo> parseSelf(std::span<const std::uint8_t> payload);
std::optional<Contact> parseContact(std::span<const std::uint8_t> payload);
std::optional<Key> parseDeletedKey(std::span<const std::uint8_t> payload);
std::optional<Channel> parseChannel(std::span<const std::uint8_t> payload);
std::optional<Incoming> parseIncoming(std::span<const std::uint8_t> payload);
std::optional<std::uint32_t> parseSentTag(std::span<const std::uint8_t> payload);
std::optional<std::uint32_t> parseSentTimeout(std::span<const std::uint8_t> payload);
std::optional<std::uint32_t> parseConfirmedTag(std::span<const std::uint8_t> payload);
std::optional<BatteryInfo> parseBattery(std::span<const std::uint8_t> payload);

Prefix prefix(const Key& key);
std::size_t channelLimit(std::size_t senderNameBytes);

}

#endif
