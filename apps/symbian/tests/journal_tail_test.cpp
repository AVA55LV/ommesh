#include "journal_tail.hpp"

#include <QtCore/QBuffer>

#include <cassert>

namespace {

QStringList eventsIn(const QByteArray& data) {
    QBuffer file;
    file.setData(data);
    const bool opened = file.open(QIODevice::ReadOnly);
    assert(opened);
    return Journal::latestEvents(file);
}

void assertNewestEvents(int count) {
    QByteArray data;
    for (int index = 1; index <= count; ++index) {
        data += "event ";
        data += QByteArray::number(index);
        data += index % 2 ? "\r\n" : "\n";
        data += "\r\n";
    }
    data += "interrupted \xf0\x9f";
    const QStringList events = eventsIn(data);
    assert(events.size() == qMin(400, count));
    for (int index = 0; index < events.size(); ++index) {
        const int number = count - events.size() + index + 1;
        assert(events.at(index) == QLatin1String("event ") + QString::number(number));
    }
}

} // namespace

int main() {
    assert(eventsIn(QByteArray()).isEmpty());
    assert(eventsIn("\r\n\n\r\ninterrupted").isEmpty());
    assert(eventsIn("oldest\n\r\nnewest\r\n") ==
           (QStringList() << QLatin1String("oldest") << QLatin1String("newest")));
    assert(eventsIn("oldest\ninvalid \xf0\x9f\nnewest\n") ==
           (QStringList() << QLatin1String("oldest") << QLatin1String("newest")));
    assertNewestEvents(1);
    assertNewestEvents(399);
    assertNewestEvents(400);
    assertNewestEvents(401);
    assertNewestEvents(950);

    QByteArray splitUtf8("head\r\n\xd1\x8f\r\n");
    splitUtf8 += QByteArray(4093, 'x'); // A chunk boundary splits the letter я.
    assert(eventsIn(splitUtf8) == (QStringList() << QLatin1String("head") << QString::fromUtf8("\xd1\x8f")));

    QByteArray oversizedLine("first\n");
    oversizedLine += QByteArray(10000, 'x');
    oversizedLine += "\nlast\n";
    assert(eventsIn(oversizedLine) == (QStringList() << QLatin1String("first") << QLatin1String("last")));
}
