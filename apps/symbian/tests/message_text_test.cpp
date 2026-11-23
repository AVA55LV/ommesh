#include "message_text.hpp"

#include <iostream>

int main() {
    const auto text = QString::fromUtf8("Встреча @[Связь] https://example.org/тест $$А+В$$");
    const MessageText::Preferences compact{true, true, false, false};
    const QString packed = MessageText::pack(text, compact);
    const auto expected = QString::fromUtf8("Bcтpeчa @[Связь] https://example.org/тест $$А+В$$");
    if (packed != expected || packed.toUtf8().size() >= text.toUtf8().size()) {
        std::cerr << "Packing must save bytes without changing mentions, URLs or formulas\n";
        return 1;
    }
    const MessageText::Preferences display{false, false, false, true};
    const auto mixed = QString::fromUtf8("Вcтрeчa USB 33 @[Связь] https://example.org/Вcтрeчa");
    const auto restored = QString::fromUtf8("Встреча USB 33 @[Связь] https://example.org/Вcтрeчa");
    if (MessageText::restore(mixed, display) != restored || MessageText::pack(text, {}) != text) {
        std::cerr << "Display conversion must leave identifiers and unmodified drafts intact\n";
        return 1;
    }
    std::cout << "Message text compatibility and byte count passed\n";
}
