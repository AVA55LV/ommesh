#ifndef OMMESH_BELLE_RADIO_CONSOLE_HPP
#define OMMESH_BELLE_RADIO_CONSOLE_HPP

#include "meshcore.hpp"

#include <functional>

namespace meshcore {

class RadioConsole {
public:
    enum Operation {
        Local = 1, Login = 2, Remote = 3, Neighbours = 4, Access = 5,
        ResetRoute = 6, Telemetry = 7, Logout = 8, Trace = 9
    };

    struct Link {
        std::function<bool(const Bytes&)> send;
        // Releases the synchronous request slot. An over-the-air reply may still be pending.
        std::function<void()> released;
        std::function<void(int, int, bool, const std::string&)> result;
        std::function<std::int64_t()> now;
        std::function<std::uint32_t()> unixSeconds;
        std::function<void(int, int, bool, const Bytes&, const std::string&)> managementResult;
    };

    explicit RadioConsole(Link link);
    void start(int requestId, int operation, const Key* fullKey, std::string_view input);
    void startManagement(int requestId, int operation, const Key& key, int order, int offset,
                         std::span<const std::uint8_t> tracePath = {}, std::uint8_t traceFlags = 0);
    bool accept(std::span<const std::uint8_t> frame);
    void tick();
    void close();
    bool busy() const;
    bool canStart(int operation) const;
    bool signedIn(const Key& fullKey) const;
    bool authorized(const Key& fullKey) const;

private:
    struct Result {
        bool success;
        std::string text;
        Bytes data{};
        bool administrator = false;
    };

    struct Request {
        int id;
        int operation;
        Key key;
        std::array<std::uint8_t, 3> tag;
        std::int64_t remoteDeadline = 0;
        std::optional<Result> early{};
        std::optional<std::uint32_t> binaryTag{};
        std::vector<Bytes> earlyBinary{};
        std::uint32_t traceTag = 0;
        std::uint8_t traceFlags = 0;
        Bytes tracePath{};
        std::int64_t started = 0;
    };
    struct Session { Key key; bool administrator; };

    bool acceptCommandReply(std::span<const std::uint8_t> frame);
    std::optional<Result> overTheAirReply(std::span<const std::uint8_t> frame) const;
    void sendPending(Bytes frame);
    void finish(Result result);
    void forgetAuthorization(const Key& key);

    Link link_;
    std::optional<Request> pending_;
    std::vector<Session> sessions_;
    std::optional<std::uint8_t> nextTag_;
    std::uint32_t nextTraceTag_ = 0;
    std::int64_t nextRadioCommand_ = 0;
    std::uint32_t lastTimestamp_ = 0;
};

}

#endif
