#include "meshcore.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>

namespace {

using meshcore::Bytes;
using meshcore::Prefix;

void testFraming()
{
    const Bytes payload{0x16, 3};
    assert((meshcore::frame(payload) == Bytes{'<', 2, 0, 0x16, 3}));
    assert(meshcore::frame({}).empty());
    assert(meshcore::frame(Bytes(meshcore::MaxFrameSize + 1)).empty());

    const Bytes stream{'x', '>', 2, 0, '>', 0x0a, '>', 1, 0, 0x83};
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        meshcore::Decoder decoder;
        const std::span<const std::uint8_t> bytes(stream);
        auto first = decoder.feed(bytes.first(split), 100);
        const auto second = decoder.feed(bytes.subspan(split), 101);
        first.insert(first.end(), second.begin(), second.end());
        assert((first == std::vector<Bytes>{{'>', 0x0a}, {0x83}}));
    }

    meshcore::Decoder decoder;
    const Bytes invalidHeaders{'>', 0, 0, '>', 177, 0, '>', '>', 1, 0, 0x83, '>', 200, '>', 1, 0, 0x0a};
    assert((decoder.feed(invalidHeaders, 0) == std::vector<Bytes>{{0x83}, {0x0a}}));
    assert(decoder.feed(Bytes{'>', 20, 0, 0x11}, 100).empty());
    assert((decoder.feed(Bytes{'>', 1, 0, 0x83}, 1100) == std::vector<Bytes>{{0x83}}));
    assert(decoder.feed(Bytes{'>', 20, 0, 0x11}, 1200).empty());
    decoder.reset();
    assert((decoder.feed(Bytes{'>', 1, 0, 0x83}, 1201) == std::vector<Bytes>{{0x83}}));

    const Bytes maximum(meshcore::MaxFrameSize, 0xff);
    Bytes inbound = meshcore::frame(maximum);
    assert(inbound[1] == 176 && inbound[2] == 0);
    inbound.front() = '>';
    assert(decoder.feed(inbound, 1300) == std::vector<Bytes>{maximum});
}

void testCommands()
{
    assert((meshcore::deviceQuery() == Bytes{0x16, 0x03}));
    const auto start = meshcore::appStart();
    assert(start.size() > 8 && start.front() == 0x01);
    assert(std::all_of(start.begin() + 1, start.begin() + 8, [](std::uint8_t byte) { return byte == 0; }));
    assert(meshcore::getContacts() == Bytes{0x04});
    assert((meshcore::getChannel(29) == Bytes{0x1f, 29}));
    assert(meshcore::syncNext() == Bytes{0x0a});

    const Prefix recipient{1, 2, 3, 4, 5, 6};
    constexpr std::uint32_t timestamp = 0x87654321;
    constexpr auto text = "\xD0\x94\xD0\xB0"; // Да
    assert((meshcore::sendDirect(recipient, text, timestamp)
        == Bytes{0x02, 0, 0, 0x21, 0x43, 0x65, 0x87, 1, 2, 3, 4, 5, 6, 0xd0, 0x94, 0xd0, 0xb0}));
    assert((meshcore::sendChannel(42, text, timestamp, 8)
        == Bytes{0x03, 0, 42, 0x21, 0x43, 0x65, 0x87, 0xd0, 0x94, 0xd0, 0xb0}));

    assert(!meshcore::sendDirect(recipient, std::string(158, 'x'), timestamp).empty());
    assert(meshcore::sendDirect(recipient, std::string(159, 'x'), timestamp).empty());
    assert(meshcore::sendDirect(recipient, {}, timestamp).empty());
    assert(meshcore::sendDirect(recipient, std::string_view("a\0b", 3), timestamp).empty());
    assert(meshcore::channelLimit(8) == 150);
    assert(!meshcore::sendChannel(0, std::string(150, 'x'), timestamp, 8).empty());
    assert(meshcore::sendChannel(0, std::string(151, 'x'), timestamp, 8).empty());
    assert(meshcore::channelLimit(159) == 0);
}

void testInventory()
{
    assert(meshcore::parseChannelCount(Bytes{0x0d, 13, 16, 4}) == 4);
    assert(!meshcore::parseChannelCount(Bytes{0x0d, 13, 16}));
    const meshcore::Key key{1, 2, 3, 4, 5, 6, 7};
    assert((meshcore::prefix(key) == Prefix{1, 2, 3, 4, 5, 6}));

    Bytes self(58, 0);
    self[0] = 0x05;
    std::copy(key.begin(), key.end(), self.begin() + 4);
    self.insert(self.end(), {'N', 'o', 'd', 'e'});
    const auto radio = meshcore::parseSelf(self);
    assert(radio && radio->key == key && radio->name == "Node");
    assert(!meshcore::parseSelf(std::span(self).first(57)));

    Bytes contact(148, 0);
    contact[0] = 0x03;
    contact[33] = 2;
    std::copy(key.begin(), key.end(), contact.begin() + 1);
    contact[100] = 'A';
    contact[102] = '?';
    const auto entry = meshcore::parseContact(contact);
    assert(entry && entry->type == 2 && entry->key == key && entry->name == "A");
    assert(!meshcore::parseContact(std::span(contact).first(147)));
    contact[0] = 0x8a;
    assert(meshcore::parseContact(contact));
    contact[0] = 0x8f;
    contact.resize(33);
    assert(meshcore::parseDeletedKey(contact) == key);

    Bytes channel(50, 0);
    channel[0] = 0x12;
    channel[1] = 7;
    const auto empty = meshcore::parseChannel(channel);
    assert(empty && !empty->configured && empty->index == 7);
    channel[49] = 42;
    const auto privateChannel = meshcore::parseChannel(channel);
    assert(privateChannel && privateChannel->configured && privateChannel->key.back() == 42);
    channel[2] = 'B';
    channel[49] = 0;
    const auto named = meshcore::parseChannel(channel);
    assert(named && named->configured && named->name == "B");
    assert(!meshcore::parseChannel(std::span(channel).first(49)));
}

void testMessages()
{
    const Bytes direct{0x10, 163, 0, 0, 1, 2, 3, 4, 5, 6, 0xff, 0, 0x21, 0x43, 0x65, 0x87, 0xd0, 0x94};
    const auto message = meshcore::parseIncoming(direct);
    const Prefix sender{1, 2, 3, 4, 5, 6};
    assert(message && !message->channel && message->sender == sender);
    assert(!message->signer);
    assert(message->timestamp == 0x87654321 && message->text == "\xD0\x94");
    for (std::size_t count = 0; count < 17; ++count) {
        assert(!meshcore::parseIncoming(std::span(direct).first(count)));
    }

    Bytes signedMessage{0x07, 1, 2, 3, 4, 5, 6, 0xff, 2, 0x21, 0x43, 0x65, 0x87, 9, 8, 7, 6, 'h', 'i'};
    const auto roomPost = meshcore::parseIncoming(signedMessage);
    assert(roomPost && roomPost->sender == sender && roomPost->text == "hi");
    assert((roomPost->signer == std::array<std::uint8_t, 4>{9, 8, 7, 6}));
    assert(!meshcore::parseIncoming(std::span(signedMessage).first(16)));
    signedMessage[8] = 3;
    assert(!meshcore::parseIncoming(signedMessage));

    const Bytes group{0x08, 42, 0, 0, 0x21, 0x43, 0x65, 0x87, 'n', ':', ' ', 'h', 'i'};
    const auto groupMessage = meshcore::parseIncoming(group);
    assert(groupMessage && groupMessage->channel && groupMessage->channelIndex == 42);
    assert(groupMessage->text == "n: hi" && groupMessage->timestamp == 0x87654321);
    Bytes groupV3{0x11, 0, 0, 0};
    groupV3.insert(groupV3.end(), group.begin() + 1, group.end());
    const auto groupMessageV3 = meshcore::parseIncoming(groupV3);
    assert(groupMessageV3 && groupMessageV3->text == groupMessage->text);
    assert(!meshcore::parseIncoming(Bytes{0x0a}));

    const Bytes sent{0x06, 1, 0x21, 0x43, 0x65, 0x87, 0x88, 0x13, 0, 0};
    const Bytes confirmed{0x82, 0x21, 0x43, 0x65, 0x87, 0x88, 0x13, 0, 0};
    assert(meshcore::parseSentTag(sent) == 0x87654321);
    assert(meshcore::parseSentTimeout(sent) == 5000);
    assert(meshcore::parseConfirmedTag(confirmed) == 0x87654321);
    assert(!meshcore::parseSentTag(confirmed) && !meshcore::parseConfirmedTag(sent));
}

}

int main()
{
    testFraming();
    testCommands();
    testInventory();
    testMessages();
    std::puts("MeshCore protocol tests passed");
}
