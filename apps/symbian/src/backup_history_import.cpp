#include "backup_history_import.hpp"

#include "backup_json.hpp"
#include "client.hpp"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <algorithm>

namespace BackupHistoryImport {
namespace {

constexpr int HistoryLimit = 400;
constexpr int HistoryBytes = 2 * 1024 * 1024;
constexpr int MessageBytes = 8192;
constexpr int NameBytes = 256;
constexpr int ReactionLimit = 32;

const char LastRadio[] = "radio/last";
const char LastName[] = "radio/lastName";
const char PublicSecret[] = "8b3387e9c5cdea6ac9e5edbaa115cd72";

struct Reaction {
    QString emoji;
    QString actor;
    quint32 stamp = 0;
    bool own = false;
};

struct Row {
    QString target;
    QString title;
    QString author;
    QString text;
    QString delivery;
    QString route;
    QString signer;
    quint32 stamp = 0;
    qint32 relays = -1;
    qint32 hops = -1;
    bool out = false;
    bool unread = false;
    bool partial = false;
    QVector<Reaction> reactions;
};

struct Address {
    QString target;
    QString title;
};

QString ui(const char* value) { return QString::fromUtf8(value); }

bool fail(QString& error, const char* text) {
    error = ui(text);
    return false;
}

QString historyKey(const QString& radio) { return QLatin1String("radios/") + radio + QLatin1String("/history"); }

QString field(const QVariantMap& data, const char* key) {
    const QVariant value = data.value(QLatin1String(key));
    return value.type() == QVariant::String ? value.toString() : QString();
}

bool hasType(const QVariantMap& data, const char* key, QVariant::Type type) {
    return data.value(QLatin1String(key)).type() == type;
}

QString hex(const QString& value, int minimum, int maximum) {
    if (value.size() < minimum * 2 || value.size() > maximum * 2 || value.size() % 2)
        return QString();
    const QString lower = value.toLower();
    for (QChar character : lower) {
        const ushort c = character.unicode();
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f'))
            return QString();
    }
    return lower;
}

QString secretDigest(const QString& secret) {
    const QByteArray bytes = QByteArray::fromHex(secret.toLatin1());
    const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(digest.constData(), digest.size());
}

bool asciiDigit(QChar character) {
    return character >= QLatin1Char('0') && character <= QLatin1Char('9');
}

int numberAt(const QString& text, int position, int length) {
    int result = 0;
    for (int index = position; index < position + length; ++index) {
        const QChar digit = text.at(index);
        if (!asciiDigit(digit))
            return -1;
        result = result * 10 + digit.unicode() - '0';
    }
    return result;
}

bool stampFromIso(const QString& iso, quint32& stamp) {
    if (iso.size() < 20 || iso.size() > 64 || iso.at(4) != QLatin1Char('-') || iso.at(7) != QLatin1Char('-') ||
            iso.at(10) != QLatin1Char('T') || iso.at(13) != QLatin1Char(':') || iso.at(16) != QLatin1Char(':'))
        return false;
    const int year = numberAt(iso, 0, 4);
    if (year < 1970)
        return false;
    const QDate date(year, numberAt(iso, 5, 2), numberAt(iso, 8, 2));
    const QTime time(numberAt(iso, 11, 2), numberAt(iso, 14, 2), numberAt(iso, 17, 2));
    if (!date.isValid() || !time.isValid())
        return false;
    int zone = 19;
    if (iso.at(zone) == QLatin1Char('.')) {
        ++zone;
        const int start = zone;
        while (zone < iso.size() && asciiDigit(iso.at(zone)))
            ++zone;
        if (zone == start)
            return false;
    }
    if (zone == iso.size())
        return false;
    const QDateTime utc(date, time, Qt::UTC);
    qint64 seconds = utc.toMSecsSinceEpoch() / 1000;
    const QChar sign = iso.at(zone);
    if (sign == QLatin1Char('Z')) {
        if (iso.size() != zone + 1)
            return false;
    } else {
        if (iso.size() != zone + 6 || iso.at(zone + 3) != QLatin1Char(':') ||
                (sign != QLatin1Char('+') && sign != QLatin1Char('-')))
            return false;
        const int hours = numberAt(iso, zone + 1, 2);
        const int minutes = numberAt(iso, zone + 4, 2);
        if (hours < 0 || hours >= 24 || minutes < 0 || minutes >= 60)
            return false;
        seconds += (sign == QLatin1Char('+') ? -1 : 1) * (hours * 3600 + minutes * 60);
    }
    if (seconds < 0 || seconds >= 0xffffffffLL)
        return false;
    stamp = static_cast<quint32>(seconds);
    return true;
}

bool readString(QDataStream& stream, QString& text) {
    QIODevice* device = stream.device();
    const qint64 start = device->pos();
    quint32 bytes = 0;
    stream >> bytes;
    if (stream.status() != QDataStream::Ok ||
            (bytes != 0xffffffffU && (bytes % 2 || bytes > 65536 || bytes > device->bytesAvailable())) ||
            !device->seek(start))
        return false;
    stream >> text;
    return stream.status() == QDataStream::Ok;
}

bool readFlag(QDataStream& stream, bool& value) {
    quint8 bit = 0;
    stream >> bit;
    value = bit != 0;
    return stream.status() == QDataStream::Ok && bit <= 1;
}

bool readRow(QDataStream& stream, quint32 version, Row& row) {
    if (!readString(stream, row.target) || !readString(stream, row.title) ||
            !readString(stream, row.author) || !readString(stream, row.text))
        return false;
    stream >> row.stamp;
    if (!readFlag(stream, row.out) || !readString(stream, row.delivery) || !readFlag(stream, row.unread))
        return false;
    if (version >= 2) {
        stream >> row.relays >> row.hops;
        if (!readString(stream, row.route))
            return false;
    }
    if (version >= 3) {
        if (!readFlag(stream, row.partial))
            return false;
        quint16 count = 0;
        stream >> count;
        if (stream.status() != QDataStream::Ok || count > ReactionLimit)
            return false;
        row.reactions.reserve(count);
        for (quint16 index = 0; index < count; ++index) {
            Reaction reaction;
            if (!readString(stream, reaction.emoji) || !readString(stream, reaction.actor))
                return false;
            stream >> reaction.stamp;
            if (!readFlag(stream, reaction.own))
                return false;
            row.reactions.append(reaction);
        }
    }
    if (version >= 4 && !readString(stream, row.signer))
        return false;
    if (version == 1 && !row.out && row.author.isEmpty() && row.target.startsWith(QLatin1String("c/"))) {
        const int separator = row.text.indexOf(QLatin1String(": "));
        if (separator > 0 && separator + 2 < row.text.size()) {
            row.author = row.text.left(separator);
            row.text = row.text.mid(separator + 2);
        }
    }
    return true;
}

bool readRows(const QByteArray& data, QVector<Row>& rows) {
    if (data.isEmpty())
        return true;
    if (data.size() > HistoryBytes)
        return false;
    QDataStream stream(data);
    stream.setVersion(QDataStream::Qt_4_6);
    quint32 version = 0, count = 0;
    stream >> version >> count;
    if (stream.status() != QDataStream::Ok || version < 1 || version > 4 || count > HistoryLimit)
        return false;
    rows.reserve(static_cast<int>(count));
    for (quint32 index = 0; index < count; ++index) {
        Row row;
        if (!readRow(stream, version, row))
            return false;
        rows.append(row);
    }
    return stream.status() == QDataStream::Ok && stream.atEnd();
}

void writeRow(QDataStream& stream, const Row& row) {
    stream << row.target << row.title << row.author << row.text << row.stamp << row.out << row.delivery << row.unread
           << row.relays << row.hops << row.route << row.partial << quint16(row.reactions.size());
    for (const Reaction& reaction : row.reactions)
        stream << reaction.emoji << reaction.actor << reaction.stamp << reaction.own;
    stream << row.signer;
}

QByteArray writeRows(const QVector<Row>& rows) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_4_6);
    stream << quint32(4) << quint32(rows.size());
    for (const Row& row : rows)
        writeRow(stream, row);
    return stream.status() == QDataStream::Ok && bytes.size() <= HistoryBytes ? bytes : QByteArray();
}

QByteArray identity(const Row& row) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << row.target << row.stamp << row.out << row.text << row.signer;
    if (!row.out && row.target.startsWith(QLatin1String("c/")))
        stream << row.author;
    return bytes;
}

QMap<QString, Address> channelAddresses(const QVector<Row>& rows, const QVector<RadioChannel>& channels) {
    QMap<QString, Address> addresses;
    for (const Row& row : rows) {
        if (!row.target.startsWith(QLatin1String("c/")) || row.target.count(QLatin1Char('/')) != 2)
            continue;
        bool slotOk = false;
        const QString slotText = row.target.section(QLatin1Char('/'), 1, 1);
        const int slot = slotText.toInt(&slotOk);
        const QString digest = hex(row.target.section(QLatin1Char('/'), 2, 2), 20, 20);
        if (slotOk && slot >= 0 && slot <= 255 && slotText == QString::number(slot) && !digest.isEmpty())
            addresses.insert(digest, {row.target, row.title});
    }
    QSet<QString> mapped;
    QSet<QString> publicAliases;
    for (const RadioChannel& channel : channels) {
        const QString secret = hex(channel.key, 16, 16);
        if (secret.isEmpty() || channel.slot < 0 || channel.slot > 255)
            continue;
        const QString digest = secretDigest(secret);
        const bool publicName = channel.title.compare(QLatin1String("Public"), Qt::CaseInsensitive) == 0;
        if (!mapped.contains(digest) || (publicName && !publicAliases.contains(digest))) {
            Address address{QLatin1String("c/") + QString::number(channel.slot) + QLatin1Char('/') + digest, channel.title};
            addresses.insert(digest, address);
            mapped.insert(digest);
            if (publicName)
                publicAliases.insert(digest);
        }
    }
    if (channels.isEmpty()) {
        const QString digest = secretDigest(QLatin1String(PublicSecret));
        if (!addresses.contains(digest))
            addresses.insert(digest, {QLatin1String("c/0/") + digest, QLatin1String("Public")});
    }
    return addresses;
}

QString delivery(const QString& status, bool channel) {
    if (status == QLatin1String("delivered"))
        return ui("Доставлено");
    if (status == QLatin1String("failed") || status == QLatin1String("queued"))
        return ui("Не отправлено");
    if (channel && (status.isEmpty() || status == QLatin1String("sent")))
        return ui("Принято рацией");
    return ui("Нет подтверждения");
}

enum Item { Invalid, Unmapped, Valid };

Item importedRow(const QVariant& item, const QMap<QString, Address>& addresses,
                 const QMap<QString, QString>& contacts, const QSet<QString>& ambiguousPeers,
                 const QMap<QString, QString>& channelNames,
                 Row& row) {
    if (item.type() != QVariant::Map)
        return Invalid;
    const QVariantMap data = item.toMap();
    if (!hasType(data, "chat", QVariant::Map) || !hasType(data, "text", QVariant::String) ||
            !stampFromIso(field(data, "sentAt"), row.stamp))
        return Invalid;
    const QString direction = field(data, "direction");
    row.out = direction == QLatin1String("out");
    if (!row.out && direction != QLatin1String("in"))
        return Invalid;
    row.text = field(data, "text");
    if (row.text.isEmpty() || row.text.contains(QChar(0)) || row.text.toUtf8().size() > MessageBytes)
        return Unmapped;

    const QVariantMap chat = data.value(QLatin1String("chat")).toMap();
    const bool channel = chat.contains(QLatin1String("channel"));
    const bool contact = chat.contains(QLatin1String("contact"));
    const bool prefix = chat.contains(QLatin1String("prefix"));
    if (int(channel) + int(contact) + int(prefix) != 1)
        return Invalid;
    if (channel) {
        const QString secret = hex(field(chat, "channel"), 16, 16);
        if (secret.isEmpty())
            return Invalid;
        const Address address = addresses.value(secretDigest(secret));
        if (address.target.isEmpty())
            return Unmapped;
        row.target = address.target;
        row.title = address.title.isEmpty() ? channelNames.value(secret) : address.title;
    } else {
        const QString peer = contact ? hex(field(chat, "contact"), 32, 32) : hex(field(chat, "prefix"), 1, 32);
        if (peer.isEmpty())
            return Invalid;
        if (peer.size() < 12 || ambiguousPeers.contains(peer.left(12)))
            return Unmapped;
        row.target = QLatin1String("d/") + peer.left(12);
        row.title = contacts.value(peer.left(12));
        if (row.title.isEmpty())
            row.title = peer.left(12).toUpper();
    }

    if (row.out) {
        row.delivery = delivery(field(data, "status"), channel);
    } else {
        const QVariantMap from = data.value(QLatin1String("from")).toMap();
        row.author = field(from, "name");
        if (hasType(data, "roomPost", QVariant::Bool) && data.value(QLatin1String("roomPost")).toBool())
            row.signer = hex(field(from, "key"), 4, 32).left(8);
        if (row.author.isEmpty() && !channel)
            row.author = row.signer.isEmpty() ? row.title : contacts.value(row.signer, row.signer);
    }
    if (hasType(data, "hops", QVariant::Double)) {
        const double hops = data.value(QLatin1String("hops")).toDouble();
        if (hops >= 0 && hops <= 63 && std::floor(hops) == hops)
            row.hops = static_cast<int>(hops);
    }
    if (channel && row.out) {
        QSet<QString> relays;
        for (const QVariant& copy : data.value(QLatin1String("echoes")).toList()) {
            const QVariantList path = copy.toMap().value(QLatin1String("path")).toList();
            if (!path.isEmpty() && path.first().type() == QVariant::String) {
                const QString hop = hex(path.first().toString(), 1, 4);
                if (!hop.isEmpty())
                    relays.insert(hop);
            }
        }
        if (!relays.isEmpty())
            row.relays = relays.size();
    }
    if (row.title.toUtf8().size() > NameBytes || row.author.toUtf8().size() > NameBytes)
        return Unmapped;
    return Valid;
}

bool optionalList(const QVariantMap& object, const char* name, QVariantList& values) {
    const QLatin1String key(name);
    if (!object.contains(key))
        return true;
    const QVariant value = object.value(key);
    if (value.type() != QVariant::List)
        return false;
    values = value.toList();
    return true;
}

bool targetNames(const QVariantList& contacts, const QVariantList& channels, const QVariantList& messages,
                 QMap<QString, QString>& peers, QSet<QString>& ambiguousPeers, QMap<QString, QString>& names) {
    QMap<QString, QString> peerKeys;
    const auto remember = [&](const QString& key) {
        const QString prefix = key.left(12);
        if (peerKeys.contains(prefix) && peerKeys.value(prefix) != key)
            ambiguousPeers.insert(prefix);
        peerKeys.insert(prefix, key);
    };
    for (const QVariant& value : contacts) {
        if (value.type() != QVariant::Map)
            return false;
        const QVariantMap record = value.toMap();
        const QString key = hex(field(record, "key"), 32, 32);
        const QString name = field(record, "name");
        if (key.isEmpty())
            return false;
        remember(key);
        if (!name.isEmpty() && name.toUtf8().size() <= NameBytes)
            peers.insert(key.left(12), name);
    }
    for (const QVariant& value : messages) {
        const QVariantMap chat = value.toMap().value(QLatin1String("chat")).toMap();
        const QString key = hex(field(chat, "contact"), 32, 32);
        if (!key.isEmpty())
            remember(key);
    }
    for (const QString& key : ambiguousPeers)
        peers.remove(key);
    for (const QVariant& value : channels) {
        if (value.type() != QVariant::Map)
            return false;
        const QVariantMap record = value.toMap();
        const QString secret = hex(field(record, "secret"), 16, 16);
        const QString name = field(record, "name");
        if (secret.isEmpty() || !hasType(record, "name", QVariant::String))
            return false;
        if (name.toUtf8().size() <= NameBytes)
            names.insert(secret, name);
    }
    return true;
}

void mergeChronologically(const QVector<Row>& old, const QVector<Row>& added, QVector<Row>& merged) {
    merged.reserve(old.size() + added.size());
    int next = 0;
    for (const Row& local : old) {
        while (next < added.size() && added.at(next).stamp < local.stamp)
            merged.append(added.at(next++));
        merged.append(local);
    }
    while (next < added.size())
        merged.append(added.at(next++));
}

}  // namespace

bool preview(const QString& path, QSettings& settings, const QVector<RadioChannel>& channels,
             Plan& plan, QString& error) {
    plan = Plan();
    QFile file(path);
    if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly))
        return fail(error, "Не удалось открыть файл истории.");
    if (file.size() > BackupJson::MaximumBytes)
        return fail(error, "Файл истории превышает 2 МБ.");
    const QByteArray bytes = file.read(BackupJson::MaximumBytes + 1);
    if (file.error() != QFile::NoError)
        return fail(error, "Не удалось прочитать файл истории.");
    return preview(bytes, settings, channels, plan, error);
}

bool preview(const QByteArray& bytes, QSettings& settings, const QVector<RadioChannel>& channels,
             Plan& plan, QString& error) {
    plan = Plan();
    error.clear();
    if (bytes.size() > BackupJson::MaximumBytes)
        return fail(error, "Файл истории превышает 2 МБ.");
    if (bytes.startsWith(QByteArray("SQLite format 3\0", 16)))
        return fail(error, "Экспорт MeshCore в SQLite пока не поддерживается. Выберите историю Ommesh в JSON.");
    QVariantMap object;
    if (!BackupJson::readObject(bytes, object))
        return fail(error, "Файл истории повреждён или имеет неподдерживаемый формат.");
    if (field(object, "format") == QLatin1String("ommesh-backup"))
        object = object.value(QLatin1String("history")).toMap();
    if (field(object, "format") != QLatin1String("ommesh-history") ||
            !hasType(object, "version", QVariant::Double) || object.value(QLatin1String("version")).toDouble() != 1)
        return fail(error, "Выберите файл истории Ommesh версии 1.");
    const QVariantMap radio = object.value(QLatin1String("radio")).toMap();
    Plan result;
    result.radioKey = hex(field(radio, "key"), 32, 32);
    result.radioName = field(radio, "name");
    if (result.radioKey.isEmpty() || result.radioName.toUtf8().size() > NameBytes)
        return fail(error, "В файле истории нет допустимого ключа ноды.");

    settings.sync();
    if (settings.status() != QSettings::NoError)
        return fail(error, "Не удалось прочитать локальную историю.");
    result.selectedBefore = settings.value(QLatin1String(LastRadio)).toString();
    if (!result.selectedBefore.isEmpty() && result.selectedBefore.compare(result.radioKey, Qt::CaseInsensitive) != 0)
        return fail(error, "Копия принадлежит другой ноде.");
    result.before = settings.value(historyKey(result.radioKey)).toByteArray();
    QVector<Row> existing;
    if (!readRows(result.before, existing))
        return fail(error, "Локальная история повреждена. Слияние остановлено.");

    QVariantList messages, contacts, archiveChannels;
    QMap<QString, QString> peerNames, channelNames;
    QSet<QString> ambiguousPeers;
    if (!optionalList(object, "messages", messages) || !optionalList(object, "contacts", contacts) ||
            !optionalList(object, "channels", archiveChannels) ||
            !targetNames(contacts, archiveChannels, messages, peerNames, ambiguousPeers, channelNames))
        return fail(error, "В файле истории недопустимый список сообщений, контактов или каналов.");
    for (const Row& row : existing)
        if (row.target.startsWith(QLatin1String("d/")) && !row.title.isEmpty())
            peerNames.insert(row.target.mid(2), row.title);
    const QMap<QString, Address> addresses = channelAddresses(existing, channels);
    QSet<QByteArray> known;
    for (const Row& row : existing)
        known.insert(identity(row));
    QVector<Row> added;
    for (const QVariant& message : messages) {
        Row row;
        const Item item = importedRow(message, addresses, peerNames, ambiguousPeers, channelNames, row);
        if (item == Invalid)
            return fail(error, "В файле истории есть недопустимое сообщение.");
        if (item == Unmapped) {
            ++result.skipped;
            continue;
        }
        const QByteArray key = identity(row);
        if (known.contains(key)) {
            ++result.duplicates;
            continue;
        }
        known.insert(key);
        added.append(row);
    }
    std::stable_sort(added.begin(), added.end(), [](const Row& first, const Row& second) {
        return first.stamp < second.stamp;
    });
    result.omitted = qMax(0, existing.size() + added.size() - HistoryLimit);
    added.remove(0, result.omitted);
    result.added = added.size();
    if (result.added) {
        QVector<Row> merged;
        mergeChronologically(existing, added, merged);
        result.after = writeRows(merged);
        if (result.after.isEmpty())
            return fail(error, "История после слияния превышает допустимый размер.");
    } else {
        result.after = result.before;
    }
    plan = result;
    return true;
}

bool apply(const Plan& plan, QSettings& settings, QString& error) {
    error.clear();
    if (plan.added <= 0 || plan.after.isEmpty() || hex(plan.radioKey, 32, 32) != plan.radioKey)
        return fail(error, "В копии нет новых сообщений для добавления.");
    settings.sync();
    if (settings.status() != QSettings::NoError || !settings.isWritable())
        return fail(error, "История недоступна для записи.");
    const QString key = historyKey(plan.radioKey);
    const QVariant original = settings.value(key);
    const QVariant previousRadio = settings.value(QLatin1String(LastRadio));
    if (original.toByteArray() != plan.before || previousRadio.toString() != plan.selectedBefore)
        return fail(error, "Локальная история или выбранная нода изменились. Откройте копию заново.");
    const QVariant previousName = settings.value(QLatin1String(LastName));
    settings.setValue(key, plan.after);
    if (plan.selectedBefore.isEmpty()) {
        settings.setValue(QLatin1String(LastRadio), plan.radioKey);
        settings.setValue(QLatin1String(LastName), plan.radioName);
    }
    settings.sync();
    if (settings.status() == QSettings::NoError)
        return true;
    const auto rollback = [&settings](const QString& setting, const QVariant& previous) {
        if (previous.isValid())
            settings.setValue(setting, previous);
        else
            settings.remove(setting);
    };
    rollback(key, original);
    if (plan.selectedBefore.isEmpty()) {
        rollback(QLatin1String(LastRadio), previousRadio);
        rollback(QLatin1String(LastName), previousName);
    }
    settings.sync();
    return fail(error, "Не удалось сохранить историю из файла.");
}

}  // namespace BackupHistoryImport
