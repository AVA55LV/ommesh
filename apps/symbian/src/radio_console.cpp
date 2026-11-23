#include "radio_console.hpp"

#include <algorithm>
#include <utility>

namespace meshcore {
namespace {

constexpr std::uint8_t SendText = 0x02;
constexpr std::uint8_t SendLogin = 0x1a;
constexpr std::uint8_t SendLogout = 29;
constexpr std::uint8_t SendTrace = 36;
constexpr std::uint8_t SendTelemetryRequest = 39;
constexpr std::uint8_t SendBinaryRequest = 50;
constexpr std::uint8_t RunCli = 0x42;
constexpr std::uint8_t CliReply = 0x1d;
constexpr std::uint8_t LoginSuccess = 0x85;
constexpr std::uint8_t LoginFail = 0x86;
constexpr std::uint8_t TraceReply = 0x89;
constexpr std::uint8_t TelemetryReply = 0x8b;
constexpr std::uint8_t BinaryReply = 0x8c;
constexpr std::uint8_t CliData = 1;

constexpr std::size_t PrefixSize = 6;
constexpr std::size_t TagSize = 3;
constexpr std::size_t PasswordLimit = 15;
constexpr std::size_t LocalLimit = MaxFrameSize - 1 - TagSize;
constexpr std::size_t RemoteLimit = 160 - TagSize;
constexpr std::size_t MaximumSessions = 32;
constexpr std::size_t MaximumEarlyReplies = 4;
constexpr std::int64_t RadioCommandInterval = 1500;
constexpr std::string_view Hex = "0123456789abcdef";

bool hasIdentity(std::span<const std::uint8_t> frame, std::size_t offset,
                 const Key& key, std::size_t length) {
    return frame.size() >= offset + length &&
           std::equal(key.begin(), key.begin() + length, frame.begin() + offset);
}

std::string readLine(std::span<const std::uint8_t> text) {
    const auto end = std::find(text.begin(), text.end(), std::uint8_t(0));
    return std::string(text.begin(), end);
}

bool hexDigit(std::uint8_t value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

void wipe(Bytes& bytes) {
    volatile std::uint8_t* data = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        data[i] = 0;
    }
}

bool binaryOperation(int operation) {
    return operation == RadioConsole::Neighbours || operation == RadioConsole::Access;
}

std::uint32_t littleU32(std::span<const std::uint8_t> frame, std::size_t at) {
    return std::uint32_t(frame[at]) | (std::uint32_t(frame[at + 1]) << 8) |
           (std::uint32_t(frame[at + 2]) << 16) | (std::uint32_t(frame[at + 3]) << 24);
}

void appendU32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

}

RadioConsole::RadioConsole(Link link)
    : link_(std::move(link)) {}

bool RadioConsole::busy() const {
    return pending_.has_value();
}

bool RadioConsole::canStart(int operation) const {
    return !busy() && (operation == Local || operation == ResetRoute || operation == Logout ||
                      link_.now() >= nextRadioCommand_);
}

bool RadioConsole::signedIn(const Key& key) const {
    return std::any_of(sessions_.begin(), sessions_.end(), [&](const Session& session) {
        return session.key == key;
    });
}

bool RadioConsole::authorized(const Key& key) const {
    return std::any_of(sessions_.begin(), sessions_.end(), [&](const Session& session) {
        return session.key == key && session.administrator;
    });
}

void RadioConsole::forgetAuthorization(const Key& key) {
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [&](const Session& session) {
        return session.key == key;
    }), sessions_.end());
}

void RadioConsole::start(int requestId, int operation, const Key* key, std::string_view input) {
    const auto fail = [&](const std::string& text) { link_.result(requestId, operation, false, text); };
    if (operation != Local && operation != Login && operation != Remote) {
        fail("Неизвестная операция консоли");
        return;
    }
    if (operation != Local && key == nullptr) {
        fail("Откройте консоль выбранного узла");
        return;
    }
    if (busy()) {
        fail("Дождитесь ответа на предыдущий запрос");
        return;
    }
    if (input.find_first_of(std::string_view("\0\r\n", 3)) != std::string_view::npos) {
        fail("Введите одну строку");
        return;
    }
    const bool login = operation == Login;
    const bool local = operation == Local;
    if (!login) {
        constexpr std::string_view spacing = " \t\v\f";
        const auto first = input.find_first_not_of(spacing);
        if (first == std::string_view::npos) {
            fail("Введите команду");
            return;
        }
        // These remote commands distinguish an empty argument from no argument.
        const bool explicitEmptyArgument = operation == Remote &&
            (input == "neighbor.remove " || input == "set owner.info " || input == "set guest.password ");
        if (!explicitEmptyArgument)
            input = input.substr(first, input.find_last_not_of(spacing) + 1 - first);
    }
    if (operation == Remote && input.size() < 2) {
        fail("Команда слишком короткая");
        return;
    }
    const std::size_t limit = login ? PasswordLimit : (local ? LocalLimit : RemoteLimit);
    if (input.size() > limit) {
        fail("Допустимо до " + std::to_string(limit) + " байт UTF-8");
        return;
    }
    if (operation == Remote && !authorized(*key)) {
        fail("Войдите как администратор узла");
        return;
    }
    const std::int64_t now = link_.now();
    if (!local && now < nextRadioCommand_) {
        fail("Подождите перед следующим запросом к узлу");
        return;
    }

    Key identity{};
    if (!local) {
        identity = *key;
        nextRadioCommand_ = now + RadioCommandInterval;
    }
    if (login) {
        forgetAuthorization(identity);
    }
    const auto candidate = nextTag_.value_or(static_cast<std::uint8_t>(link_.unixSeconds() ^ now));
    nextTag_ = static_cast<std::uint8_t>(candidate + 1);
    pending_ = Request{requestId, operation, identity,
        {static_cast<std::uint8_t>(Hex[candidate >> 4]),
         static_cast<std::uint8_t>(Hex[candidate & 15]), '|'}, 0, std::nullopt};

    Bytes frame;
    if (login) {
        frame.reserve(1 + identity.size() + input.size());
        frame.push_back(SendLogin);
        frame.insert(frame.end(), identity.begin(), identity.end());
    } else if (local) {
        frame.reserve(1 + TagSize + input.size());
        frame.push_back(RunCli);
    } else {
        frame.reserve(7 + PrefixSize + TagSize + input.size());
        frame = {SendText, CliData, 0};
        lastTimestamp_ = std::max(link_.unixSeconds(), lastTimestamp_ + 1);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            frame.push_back(static_cast<std::uint8_t>(lastTimestamp_ >> shift));
        }
        frame.insert(frame.end(), identity.begin(), identity.begin() + PrefixSize);
    }
    if (!login) {
        frame.insert(frame.end(), pending_->tag.begin(), pending_->tag.end());
    }
    frame.insert(frame.end(), input.begin(), input.end());
    sendPending(std::move(frame));
}

void RadioConsole::startManagement(int requestId, int operation, const Key& key, int order, int offset,
                                   std::span<const std::uint8_t> tracePath, std::uint8_t traceFlags) {
    const auto fail = [&](const std::string& text) {
        if (link_.managementResult)
            link_.managementResult(requestId, operation, false, {}, text);
    };
    if (!canStart(operation)) {
        fail("Подождите перед следующим запросом к узлу");
        return;
    }
    Bytes frame;
    Request request{requestId, operation, key, {}};
    if (operation == ResetRoute) {
        frame = resetPath(key);
    } else if (operation == Logout) {
        frame = {SendLogout};
        frame.insert(frame.end(), key.begin(), key.end());
    } else if (operation == Telemetry) {
        frame = {SendTelemetryRequest, 0, 0, 0};
        frame.insert(frame.end(), key.begin(), key.end());
        nextRadioCommand_ = link_.now() + RadioCommandInterval;
    } else if (operation == Trace) {
        if (traceFlags > 2 || tracePath.empty() || (tracePath.size() & ((1u << traceFlags) - 1)) != 0 ||
            13 + tracePath.size() + (tracePath.size() >> traceFlags) > MaxFrameSize ||
            (tracePath.size() >> traceFlags) > 64) {
            fail("Маршрут слишком длинный для проверки");
            return;
        }
        const std::int64_t now = link_.now();
        if (!nextTraceTag_)
            nextTraceTag_ = link_.unixSeconds() ^ static_cast<std::uint32_t>(now);
        request.traceTag = nextTraceTag_++;
        request.traceFlags = traceFlags;
        request.tracePath.assign(tracePath.begin(), tracePath.end());
        request.started = now;
        frame = {SendTrace};
        appendU32(frame, request.traceTag);
        appendU32(frame, 0);
        frame.push_back(traceFlags);
        frame.insert(frame.end(), tracePath.begin(), tracePath.end());
        nextRadioCommand_ = now + RadioCommandInterval;
    } else if (binaryOperation(operation)) {
        if (operation == Neighbours && !signedIn(key)) {
            fail("Войдите на узел, чтобы прочитать соседей");
            return;
        }
        if (operation == Access && !authorized(key)) {
            fail("Войдите как администратор узла");
            return;
        }
        frame = {SendBinaryRequest};
        frame.insert(frame.end(), key.begin(), key.end());
        switch (operation) {
        case Neighbours: {
            if (order < 0 || order > 3 || offset < 0 || offset > 65535) {
                fail("Неверная страница соседей");
                return;
            }
            frame.insert(frame.end(), {0x06, 0, 10, static_cast<std::uint8_t>(offset),
                         static_cast<std::uint8_t>(offset >> 8), static_cast<std::uint8_t>(order), PrefixSize});
            // Vary otherwise identical queries so the mesh does not treat them as duplicates.
            const std::uint32_t nonce = link_.unixSeconds() ^ static_cast<std::uint32_t>(link_.now());
            appendU32(frame, nonce);
            break;
        }
        case Access:
            frame.insert(frame.end(), {0x05, 0, 0});
            break;
        }
        nextRadioCommand_ = link_.now() + RadioCommandInterval;
    } else {
        fail("Неизвестный запрос к узлу");
        return;
    }
    pending_ = std::move(request);
    sendPending(std::move(frame));
}

void RadioConsole::sendPending(Bytes frame) {
    const int requestId = pending_->id;
    const int operation = pending_->operation;
    const bool sent = link_.send(frame);
    wipe(frame);
    if (!sent && pending_ && pending_->id == requestId && pending_->operation == operation) {
        finish({false, "Не удалось отправить команду рации"});
    }
}

bool RadioConsole::accept(std::span<const std::uint8_t> frame) {
    if (!pending_ || frame.empty()) {
        return false;
    }
    if (pending_->remoteDeadline == 0 && acceptCommandReply(frame)) {
        return true;
    }
    if (binaryOperation(pending_->operation) && frame.front() == BinaryReply && frame.size() >= 6) {
        if (!pending_->binaryTag) {
            if (pending_->earlyBinary.size() == MaximumEarlyReplies)
                pending_->earlyBinary.erase(pending_->earlyBinary.begin());
            pending_->earlyBinary.emplace_back(frame.begin(), frame.end());
        } else if (*pending_->binaryTag == littleU32(frame, 2)) {
            finish({true, {}, Bytes(frame.begin() + 6, frame.end())});
        } else {
            return false;
        }
        return true;
    }
    auto result = overTheAirReply(frame);
    if (!result.has_value()) {
        return false;
    }
    // A push can precede the radio's reply to the corresponding send command.
    if (pending_->remoteDeadline == 0) {
        if (!pending_->early) {
            pending_->early = std::move(result);
        }
    } else {
        finish(std::move(*result));
    }
    return true;
}

bool RadioConsole::acceptCommandReply(std::span<const std::uint8_t> frame) {
    Request& request = *pending_;
    const bool local = request.operation == Local;
    const std::uint8_t code = frame.front();
    const bool localManagement = request.operation == ResetRoute || request.operation == Logout;
    const bool success = code == (local ? CliReply : localManagement ? response::Ok : response::Sent);
    if (code != response::Error && code != 0x0f && !success) {
        return false;
    }
    link_.released();
    if (localManagement) {
        if (request.operation == ResetRoute)
            finish({success, success ? "Маршрут сброшен" : "Рация не сбросила маршрут"});
        else
            finish({success, success ? "Сеанс завершён" : "Рация не завершила сеанс"});
        return true;
    }
    if (local) {
        if (!success) {
            const bool unsupported = frame.size() == 1 || frame[1] == 1;
            finish({false, unsupported ? "Прошивка рации не поддерживает локальную консоль"
                                       : "Рация отклонила команду"});
        } else {
            auto text = frame.subspan(1);
            if (text.size() >= TagSize &&
                std::equal(request.tag.begin(), request.tag.end(), text.begin())) {
                text = text.subspan(TagSize);
            } else if (text.size() >= TagSize && text[2] == '|' &&
                       hexDigit(text[0]) && hexDigit(text[1])) {
                finish({false, "Получен ответ на другую команду"});
                return true;
            }
            finish({true, readLine(text)});
        }
        return true;
    }
    if (code != response::Sent || frame.size() < 10) {
        const bool unsupported = request.operation >= Neighbours &&
                                 code == response::Error && (frame.size() == 1 || frame[1] == 1);
        finish({false, unsupported ? "Прошивка рации не поддерживает этот запрос"
                                   : "Рация отклонила запрос к узлу"});
        return true;
    }
    if (request.operation == Login && !hasIdentity(frame, 2, request.key, 4)) {
        finish({false, "Получен ответ рации для другого узла"});
        return true;
    }
    if (request.operation == Trace && parseSentTag(frame) != request.traceTag) {
        finish({false, "Получен ответ рации для другой проверки"});
        return true;
    }

    const std::int64_t estimate = parseSentTimeout(frame).value_or(0);
    const std::int64_t minimum = frame[1] == 0 ? 6000 : 12000;
    const std::int64_t wait = std::clamp(estimate * 5 / 4 + 2100, minimum, std::int64_t(60000));
    request.remoteDeadline = link_.now() + wait;
    if (binaryOperation(request.operation)) {
        request.binaryTag = parseSentTag(frame);
        auto early = std::move(request.earlyBinary);
        for (const auto& reply : early) {
            if (littleU32(reply, 2) == *request.binaryTag) {
                finish({true, {}, Bytes(reply.begin() + 6, reply.end())});
                break;
            }
        }
    } else if (request.early) {
        finish(std::move(*request.early));
    }
    return true;
}

std::optional<RadioConsole::Result> RadioConsole::overTheAirReply(
    std::span<const std::uint8_t> frame) const {
    const Request& request = *pending_;
    const std::uint8_t code = frame.front();
    if (request.operation == Login) {
        if ((code != LoginSuccess && code != LoginFail) ||
            !hasIdentity(frame, 2, request.key, PrefixSize)) {
            return std::nullopt;
        }
        if (code == LoginFail) {
            return Result{false, "Узел отклонил вход"};
        }
        const bool administrator = frame.size() >= 13 ? (frame[12] & 3) == 3 : frame[1] == 1;
        return Result{true, administrator ? "Вход администратора подтверждён" : "Вход подтверждён",
                      {}, administrator};
    }
    if (request.operation == Telemetry && code == TelemetryReply &&
        hasIdentity(frame, 2, request.key, PrefixSize)) {
        return Result{true, {}, Bytes(frame.begin() + 8, frame.end())};
    }
    if (request.operation == Trace && code == TraceReply) {
        const std::size_t length = request.tracePath.size();
        const std::size_t samples = (length >> request.traceFlags) + 1;
        constexpr std::size_t header = 12;
        if (frame.size() < header + length + samples || frame[2] != length ||
            frame[3] != request.traceFlags || littleU32(frame, 4) != request.traceTag ||
            !std::equal(request.tracePath.begin(), request.tracePath.end(), frame.begin() + header))
            return std::nullopt;
        Bytes data;
        appendU32(data, static_cast<std::uint32_t>(std::max<std::int64_t>(0, link_.now() - request.started)));
        data.insert(data.end(), frame.begin() + header + length, frame.begin() + header + length + samples);
        return Result{true, {}, std::move(data)};
    }
    if (request.operation != Remote ||
        (code != response::ContactMessage && code != response::ContactMessageV3)) {
        return std::nullopt;
    }
    const std::size_t keyOffset = code == response::ContactMessageV3 ? 4 : 1;
    const std::size_t format = keyOffset + PrefixSize + 1;
    const std::size_t text = format + 5;
    if (frame.size() < text + TagSize || frame[format] != CliData ||
        !hasIdentity(frame, keyOffset, request.key, PrefixSize) ||
        !std::equal(request.tag.begin(), request.tag.end(), frame.begin() + text)) {
        return std::nullopt;
    }
    return Result{true, readLine(frame.subspan(text + TagSize))};
}

void RadioConsole::tick() {
    if (pending_ && pending_->remoteDeadline > 0 && pending_->remoteDeadline <= link_.now()) {
        finish({false, "Узел не ответил"});
    }
}

void RadioConsole::close() {
    sessions_.clear();
    nextRadioCommand_ = 0;
    if (pending_) {
        finish({false, "Соединение закрыто"});
    }
}

void RadioConsole::finish(Result result) {
    const Request completed = std::move(*pending_);
    pending_.reset();
    if (completed.operation == Login && result.success) {
        if (sessions_.size() == MaximumSessions) {
            sessions_.erase(sessions_.begin());
        }
        sessions_.push_back({completed.key, result.administrator});
    }
    if (completed.operation == Logout && result.success)
        forgetAuthorization(completed.key);
    if (completed.operation >= Neighbours) {
        if (link_.managementResult)
            link_.managementResult(completed.id, completed.operation, result.success, result.data, result.text);
    } else {
        link_.result(completed.id, completed.operation, result.success, result.text);
    }
}

}
