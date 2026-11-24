#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QIODevice>
#include <QtCore/QStringList>

namespace Journal {

inline QStringList latestEvents(QIODevice& log) {
    constexpr int maximumEvents = 400;
    constexpr qint64 chunkSize = 4096;
    // StartupTrace writes a stage of at most 96 bytes followed by CRLF.
    constexpr int maximumLineBytes = 96 + 1;
    QStringList events;
    QByteArray lineSuffix;
    bool sawLineEnd = false;
    bool oversizedSuffix = false;

    const auto addEvent = [&events](const char* bytes, int size) {
        if (size && bytes[size - 1] == '\r')
            --size;
        if (!size)
            return;
        const QString event = QString::fromUtf8(bytes, size);
        // A record may have been damaged partway through a UTF-8 character.
        if (event.toUtf8() == QByteArray::fromRawData(bytes, size))
            events.prepend(event);
    };

    qint64 offset = log.size();
    while (offset > 0 && events.size() < maximumEvents) {
        const qint64 count = qMin(offset, chunkSize);
        offset -= count;
        if (!log.seek(offset))
            return events;
        QByteArray chunk = log.read(count);
        if (chunk.size() != count)
            return events;

        chunk.append(lineSuffix);
        int end = chunk.size();
        while (end > 0 && events.size() < maximumEvents) {
            const int newline = chunk.lastIndexOf('\n', end - 1);
            if (newline < 0)
                break;
            const int length = end - newline - 1;
            if (sawLineEnd && !oversizedSuffix && length <= maximumLineBytes)
                addEvent(chunk.constData() + newline + 1, length);
            sawLineEnd = true;
            oversizedSuffix = false;
            end = newline;
        }
        if (sawLineEnd) {
            oversizedSuffix = oversizedSuffix || end > maximumLineBytes;
            lineSuffix = oversizedSuffix ? QByteArray() : chunk.left(end);
        }
    }

    if (sawLineEnd && !oversizedSuffix && offset == 0 && events.size() < maximumEvents)
        addEvent(lineSuffix.constData(), lineSuffix.size());
    return events;
}

} // namespace Journal
