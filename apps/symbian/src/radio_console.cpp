#include "radio_console.hpp"

#include <algorithm>
#include <utility>

namespace meshcore {
namespace {

constexpr std::uint8_t SendText = 0x02;
constexpr std::uint8_t SendLogin = 0x1a;
constexpr std::uint8_t RunCli = 0x42;
constexpr std::uint8_t CliReply = 0x1d;
constexpr std::uint8_t LoginSuccess = 0x85;
constexpr std::uint8_t LoginFail = 0x86;
constexpr std::uint8_t CliData = 1;

constexpr std::size_t PrefixSize = 6;
constexpr std::size_t TagSize = 3;
constexpr std::size_t PasswordLimit = 15;
constexpr std::size_t LocalLimit = MaxFrameSize - 1 - TagSize;
constexpr std::size_t RemoteLimit = 160 - TagSize;
constexpr std::size_t MaximumAdministrators = 32;
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

}

RadioConsole::RadioConsole(Link link)
    : link_(std::move(link)) {}

bool RadioConsole::busy() const {
    return pending_.has_value();
}

bool RadioConsole::authorized(const Key& key) const {
    return std::find(administrators_.begin(), administrators_.end(), key) != administrators_.end();
}

void RadioConsole::forgetAuthorization(const Key& key) {
    const auto previous = std::find(administrators_.begin(), administrators_.end(), key);
    if (previous != administrators_.end()) {
        administrators_.erase(previous);
    }
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
    if (code != response::Error && code != (local ? CliReply : response::Sent)) {
        return false;
    }
    link_.released();
    if (local) {
        if (code == response::Error) {
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
        finish({false, "Рация отклонила запрос к узлу"});
        return true;
    }
    if (request.operation == Login && !hasIdentity(frame, 2, request.key, 4)) {
        finish({false, "Получен ответ рации для другого узла"});
        return true;
    }

    const std::int64_t estimate = parseSentTimeout(frame).value_or(0);
    const std::int64_t minimum = frame[1] == 0 ? 6000 : 12000;
    const std::int64_t wait = std::clamp(estimate * 5 / 4 + 2100, minimum, std::int64_t(60000));
    request.remoteDeadline = link_.now() + wait;
    if (request.early) {
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
        if (frame[1] != 1) {
            return Result{false, "Узел не подтвердил права администратора"};
        }
        return Result{true, "Вход администратора подтверждён"};
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
    administrators_.clear();
    nextRadioCommand_ = 0;
    if (pending_) {
        finish({false, "Соединение закрыто"});
    }
}

void RadioConsole::finish(Result result) {
    const Request completed = std::move(*pending_);
    pending_.reset();
    if (completed.operation == Login && result.success) {
        if (administrators_.size() == MaximumAdministrators) {
            administrators_.erase(administrators_.begin());
        }
        administrators_.push_back(completed.key);
    }
    link_.result(completed.id, completed.operation, result.success, result.text);
}

}
