#include "radio_backup_archive.hpp"
#include "backup_json.hpp"
#include "radio_key_identity.hpp"

#include <QtCore/QRegExp>
#include <QtCore/QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace RadioBackupArchive {
namespace {

const char* const author = "OMMesh Belle";
const int contactLimit = 4096;

QString ui(const char* text) {
    return QString::fromUtf8(text);
}

bool fail(QString& error, const char* message) {
    error = ui(message);
    return false;
}

QString fromWire(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

std::string toWire(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QByteArray keyBytes(const meshcore::Key& key) {
    return QByteArray(reinterpret_cast<const char*>(key.data()), static_cast<int>(key.size()));
}

bool hexBytes(const QString& text, int size, QByteArray& out) {
    if (text.size() != 2 * size)
        return false;
    QByteArray digits(size * 2, '\0');
    for (int i = 0; i < text.size(); ++i) {
        const ushort c = text.at(i).unicode();
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
        digits[i] = static_cast<char>(c);
    }
    out = QByteArray::fromHex(digits);
    return out.size() == size;
}

bool arrayKey(const QString& text, meshcore::Key& key) {
    QByteArray bytes;
    if (!hexBytes(text, 32, bytes))
        return false;
    std::memcpy(key.data(), bytes.constData(), key.size());
    return true;
}

QString hexText(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toHex().constData(), bytes.size() * 2);
}

bool whole(const QVariant& value, int minimum, int maximum, int& number) {
    if (value.type() != QVariant::Double && value.type() != QVariant::Int &&
            value.type() != QVariant::UInt && value.type() != QVariant::LongLong)
        return false;
    const double raw = value.toDouble();
    if (!std::isfinite(raw) || raw < minimum || raw > maximum)
        return false;
    number = static_cast<int>(raw);
    return number == raw;
}

bool number(const QVariant& value, double minimum, double maximum, double& number) {
    if (value.type() != QVariant::Double && value.type() != QVariant::Int)
        return false;
    number = value.toDouble();
    return std::isfinite(number) && number >= minimum && number <= maximum;
}

std::int32_t microdegrees(double value) {
    const double scaled = value * 1000000;
    return static_cast<std::int32_t>(scaled + (scaled < 0 ? -0.5 : 0.5));
}

bool wireName(const QVariant& value, int maximum, QString& out) {
    if (value.type() != QVariant::String)
        return false;
    out = value.toString();
    const QByteArray bytes = out.toUtf8();
    return bytes.size() <= maximum && !bytes.contains('\0');
}

QString isoUtc(const QDateTime& time) {
    return time.toUTC().toString(QLatin1String("yyyy-MM-dd'T'HH:mm:ss'Z'"));
}

bool isoDate(const QVariant& value, QDateTime& date) {
    if (value.type() != QVariant::String)
        return false;
    const QString stamp = value.toString();
    QRegExp iso(QLatin1String("([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2})(?:\\.[0-9]{1,9})?(Z|[+-][0-9]{2}:[0-9]{2})"));
    if (!iso.exactMatch(stamp))
        return false;
    date = QDateTime::fromString(iso.cap(1), Qt::ISODate);
    if (!date.isValid())
        return false;
    date.setTimeSpec(Qt::UTC);
    const QString zone = iso.cap(2);
    if (zone != QLatin1String("Z")) {
        const int hours = zone.mid(1, 2).toInt();
        const int minutes = zone.mid(4, 2).toInt();
        if (hours > 23 || minutes > 59)
            return false;
        const int offset = hours * 3600 + minutes * 60;
        date = date.addSecs(zone.at(0) == QLatin1Char('+') ? -offset : offset);
    }
    return true;
}

bool versionOne(const QVariantMap& map) {
    int version = 0;
    return whole(map.value(QLatin1String("version")), 1, 1, version);
}

bool positiveKey(const meshcore::Key& key) {
    return std::any_of(key.begin(), key.end(), [](std::uint8_t byte) { return byte != 0; });
}

bool validRadioParams(const meshcore::SelfInfo& self) {
    return self.frequencyKhz >= 100000 && self.frequencyKhz <= 3000000 &&
           self.bandwidthHz >= 1000 && self.bandwidthHz <= 1000000 &&
           self.spreadingFactor >= 5 && self.spreadingFactor <= 12 &&
           self.codingRate >= 5 && self.codingRate <= 8;
}

QVariantMap snapshotSettings(const RadioBackupSnapshot& source) {
    const auto& self = source.identity;
    QVariantMap settings;
    if (validRadioParams(self)) {
        QVariantMap radio;
        radio.insert(QLatin1String("frequencyKhz"), static_cast<int>(self.frequencyKhz));
        radio.insert(QLatin1String("bandwidthHz"), static_cast<int>(self.bandwidthHz));
        radio.insert(QLatin1String("spreadingFactor"), self.spreadingFactor);
        radio.insert(QLatin1String("codingRate"), self.codingRate);
        if (source.repeatEnabled)
            radio.insert(QLatin1String("repeat"), *source.repeatEnabled);
        settings.insert(QLatin1String("radio"), radio);
    }
    if (self.powerDbm >= -9 && self.powerDbm <= 40)
        settings.insert(QLatin1String("txPower"), self.powerDbm);
    QVariantMap location;
    location.insert(QLatin1String("lat"), self.latitude / 1000000.0);
    location.insert(QLatin1String("lon"), self.longitude / 1000000.0);
    settings.insert(QLatin1String("location"), location);

    QVariantMap other;
    other.insert(QLatin1String("manualAddContacts"), int(self.manualAddContacts));
    other.insert(QLatin1String("telemetryModeBase"), int(self.telemetryModes & 3));
    other.insert(QLatin1String("telemetryModeLocation"), int((self.telemetryModes >> 2) & 3));
    other.insert(QLatin1String("telemetryModeEnvironment"), int((self.telemetryModes >> 4) & 3));
    other.insert(QLatin1String("advertLocPolicy"), int(self.advertLocPolicy));
    other.insert(QLatin1String("multiAcks"), int(self.multiAcks));
    settings.insert(QLatin1String("other"), other);
    if (source.pathHashMode >= 0 && source.pathHashMode <= 2)
        settings.insert(QLatin1String("pathHashMode"), source.pathHashMode);
    return settings;
}

// The stored shape is also verified immediately before constructing the byte writes.
bool readSettings(const QVariantMap& settings, RadioBackupSnapshot& source) {
    auto& self = source.identity;
    if (settings.contains(QLatin1String("radio"))) {
        const QVariantMap radio = settings.value(QLatin1String("radio")).toMap();
        int frequency = 0;
        int bandwidth = 0;
        if (!whole(radio.value(QLatin1String("frequencyKhz")), 100000, 3000000, frequency) ||
            !whole(radio.value(QLatin1String("bandwidthHz")), 1000, 1000000, bandwidth) ||
            !whole(radio.value(QLatin1String("spreadingFactor")), 5, 12, self.spreadingFactor) ||
            !whole(radio.value(QLatin1String("codingRate")), 5, 8, self.codingRate))
            return false;
        self.frequencyKhz = static_cast<std::uint32_t>(frequency);
        self.bandwidthHz = static_cast<std::uint32_t>(bandwidth);
        if (radio.contains(QLatin1String("repeat"))) {
            const QVariant repeat = radio.value(QLatin1String("repeat"));
            if (repeat.type() != QVariant::Bool)
                return false;
            source.repeatEnabled = repeat.toBool();
        }
    }
    if (settings.contains(QLatin1String("txPower")) &&
        !whole(settings.value(QLatin1String("txPower")), -9, 40, self.powerDbm))
        return false;
    if (settings.contains(QLatin1String("location"))) {
        const QVariantMap location = settings.value(QLatin1String("location")).toMap();
        double lat = 0;
        double lon = 0;
        if (!number(location.value(QLatin1String("lat")), -90, 90, lat) ||
            !number(location.value(QLatin1String("lon")), -180, 180, lon))
            return false;
        self.latitude = microdegrees(lat);
        self.longitude = microdegrees(lon);
    }
    if (settings.contains(QLatin1String("other"))) {
        const QVariantMap fields = settings.value(QLatin1String("other")).toMap();
        const char* const names[] = {"manualAddContacts", "telemetryModeBase", "telemetryModeLocation",
                                    "telemetryModeEnvironment", "advertLocPolicy", "multiAcks"};
        int values[6] = {};
        for (int i = 0; i < 6; ++i) {
            if (!whole(fields.value(QLatin1String(names[i])), 0, 255, values[i]))
                return false;
        }
        self.manualAddContacts = static_cast<std::uint8_t>(values[0]);
        self.telemetryModes = static_cast<std::uint8_t>(((values[3] & 3) << 4) |
                                                        ((values[2] & 3) << 2) | (values[1] & 3));
        self.advertLocPolicy = static_cast<std::uint8_t>(values[4]);
        self.multiAcks = static_cast<std::uint8_t>(values[5]);
    }
    if (settings.contains(QLatin1String("pathHashMode")) &&
        !whole(settings.value(QLatin1String("pathHashMode")), 0, 2, source.pathHashMode))
        return false;
    if (settings.contains(QLatin1String("autoAdd"))) {
        const QVariantMap additions = settings.value(QLatin1String("autoAdd")).toMap();
        int config = 0;
        int hops = 0;
        if (!whole(additions.value(QLatin1String("config")), 0, 255, config) ||
            !whole(additions.value(QLatin1String("maxHops")), 0, 255, hops))
            return false;
    }
    return true;
}

bool readChannels(const QVariant& value, QVector<RadioChannel>& channels) {
    if (!value.isValid())
        return true;
    if (value.type() != QVariant::List || value.toList().size() > 256)
        return false;
    const QVariantList items = value.toList();
    QSet<int> usedSlots;
    for (int i = 0; i < items.size(); ++i) {
        const QVariantMap item = items.at(i).toMap();
        RadioChannel channel{};
        QByteArray secret;
        if (!whole(item.value(QLatin1String("slot")), 0, 255, channel.slot) ||
            usedSlots.contains(channel.slot) ||
            !wireName(item.value(QLatin1String("name")), 31, channel.title) ||
            !hexBytes(item.value(QLatin1String("secret")).toString(), 16, secret))
            return false;
        usedSlots.insert(channel.slot);
        if (channel.title.isEmpty() && secret == QByteArray(16, '\0'))
            continue;
        channel.key = hexText(secret);
        channels.append(channel);
    }
    return true;
}

const char* const contactKinds[] = {"", "chat", "repeater", "room", "sensor"};

bool readContactRoute(const QVariant& value, meshcore::Contact& contact) {
    if (!value.isValid())
        return true;
    if (value.type() != QVariant::List)
        return false;
    const QVariantList route = value.toList();
    if (route.isEmpty()) {
        contact.path = 0;
        return true;
    }
    const int width = route.first().toString().size() / 2;
    if (width < 1 || width > 4 || route.size() > 63 || route.size() * width > 64)
        return false;
    for (int i = 0; i < route.size(); ++i) {
        QByteArray hash;
        if (!hexBytes(route.at(i).toString(), width, hash))
            return false;
        if (width <= 3)
            std::memcpy(contact.pathHashes.data() + i * width, hash.constData(), hash.size());
    }
    if (width <= 3)
        contact.path = static_cast<std::uint8_t>((width - 1) * 64 + route.size());
    return true;
}

bool readContacts(const QVariant& value, std::vector<meshcore::Contact>& contacts) {
    if (!value.isValid())
        return true;
    if (value.type() != QVariant::List || value.toList().size() > contactLimit)
        return false;
    const QVariantList items = value.toList();
    QSet<QString> keys;
    for (int i = 0; i < items.size(); ++i) {
        if (items.at(i).type() != QVariant::Map)
            return false;
        const QVariantMap item = items.at(i).toMap();
        const QString availability = item.value(QLatin1String("on"), QLatin1String("radio")).toString();
        if (availability == QLatin1String("removed") || availability == QLatin1String("heard"))
            continue;
        if (availability != QLatin1String("radio"))
            return false;
        meshcore::Contact contact;
        contact.type = 1;
        if (!arrayKey(item.value(QLatin1String("key")).toString(), contact.key) || !positiveKey(contact.key))
            return false;
        const QString key = hexText(keyBytes(contact.key));
        if (keys.contains(key))
            return false;
        keys.insert(key);
        if (item.contains(QLatin1String("name"))) {
            QString name;
            if (!wireName(item.value(QLatin1String("name")), 32, name))
                return false;
            contact.name = toWire(name);
        }
        if (item.contains(QLatin1String("kind"))) {
            const QString value = item.value(QLatin1String("kind")).toString();
            for (int kind = 1; kind < 5; ++kind) {
                if (value == QLatin1String(contactKinds[kind])) {
                    contact.type = static_cast<std::uint8_t>(kind);
                    break;
                }
                if (kind == 4)
                    return false;
            }
        }
        int flags = 0;
        if (item.contains(QLatin1String("flags")) &&
            !whole(item.value(QLatin1String("flags")), 0, 255, flags))
            return false;
        contact.flags = static_cast<std::uint8_t>(flags);
        double lat = 0;
        double lon = 0;
        if ((item.contains(QLatin1String("lat")) && !number(item.value(QLatin1String("lat")), -90, 90, lat)) ||
            (item.contains(QLatin1String("lon")) && !number(item.value(QLatin1String("lon")), -180, 180, lon)))
            return false;
        contact.latitude = microdegrees(lat);
        contact.longitude = microdegrees(lon);
        if (item.contains(QLatin1String("lastAdvert"))) {
            QDateTime date;
            if (!isoDate(item.value(QLatin1String("lastAdvert")), date))
                return false;
            contact.lastAdvert = date.toTime_t();
        }
        if (!readContactRoute(item.value(QLatin1String("route")), contact))
            return false;
        contacts.push_back(contact);
    }
    return true;
}

QVariantMap contactJson(const meshcore::Contact& contact) {
    QVariantMap result;
    result.insert(QLatin1String("key"), hexText(keyBytes(contact.key)));
    result.insert(QLatin1String("name"), fromWire(contact.name));
    const int kind = contact.type >= 1 && contact.type <= 4 ? contact.type : 1;
    result.insert(QLatin1String("kind"), QLatin1String(contactKinds[kind]));
    result.insert(QLatin1String("on"), QLatin1String("radio"));
    result.insert(QLatin1String("flags"), int(contact.flags));
    result.insert(QLatin1String("lat"), contact.latitude / 1000000.0);
    result.insert(QLatin1String("lon"), contact.longitude / 1000000.0);
    if (contact.lastAdvert)
        result.insert(QLatin1String("lastAdvert"), isoUtc(QDateTime::fromTime_t(contact.lastAdvert)));
    const int hops = contact.path & 63;
    const int width = (contact.path >> 6) + 1;
    if (contact.path == 0xff || width > 3 || hops * width > int(contact.pathHashes.size())) {
        result.insert(QLatin1String("route"), QVariant());
    } else {
        QVariantList route;
        for (int i = 0; i < hops; ++i) {
            route.append(hexText(QByteArray(reinterpret_cast<const char*>(contact.pathHashes.data()) +
                                            i * width, width)));
        }
        result.insert(QLatin1String("route"), route);
    }
    return result;
}

QVariantMap keyObject(const QString& name, const QString& publicKey, const QByteArray& privateKey) {
    QVariantMap data;
    data.insert(QLatin1String("name"), name);
    data.insert(QLatin1String("publicKey"), publicKey);
    data.insert(QLatin1String("privateKey"), hexText(privateKey));
    return data;
}

bool decodeKeyObject(const QVariantMap& object, const QString& claimed, KeyFile& result) {
    if (!wireName(object.value(QLatin1String("name"), QString()), 31, result.name) ||
        !hexBytes(object.value(QLatin1String("privateKey")).toString(), 64, result.privateKey))
        return false;
    const QByteArray publicKey = deriveRadioPublicKey(result.privateKey);
    if (publicKey.isEmpty())
        return false;
    result.publicKey = hexText(publicKey);
    const QVariant embedded = object.value(QLatin1String("publicKey"));
    return (!embedded.isValid() || (embedded.type() == QVariant::String &&
               embedded.toString().toLower() == result.publicKey)) &&
           (claimed.isEmpty() || claimed.toLower() == result.publicKey);
}

void word(meshcore::Bytes& bytes, std::uint32_t value) {
    for (int n = 0; n < 4; ++n)
        bytes.push_back(static_cast<std::uint8_t>(value >> (n * 8)));
}

void appendName(RadioBackupRestore& plan, const std::string& from, const std::string& to) {
    if (from.empty() || from == to)
        return;
    meshcore::Bytes bytes{8};
    bytes.insert(bytes.end(), from.begin(), from.end());
    plan.settings.push_back(std::move(bytes));
}

void buildSettings(const QVariantMap& settings, const RadioBackupSnapshot& source,
                   const RadioBackupSnapshot& target, RadioBackupRestore& plan) {
    const auto& self = source.identity;
    const auto& held = target.identity;
    if (settings.contains(QLatin1String("radio")) && self.frequencyKhz >= 150000 &&
        self.frequencyKhz <= 2500000 && self.bandwidthHz >= 7000 && self.bandwidthHz <= 500000) {
        const bool tuned = self.frequencyKhz == held.frequencyKhz && self.bandwidthHz == held.bandwidthHz &&
                           self.spreadingFactor == held.spreadingFactor && self.codingRate == held.codingRate;
        const std::optional<bool> repeat = target.repeatEnabled.has_value()
            ? source.repeatEnabled.value_or(*target.repeatEnabled) : std::optional<bool>();
        if (!tuned || (repeat && repeat != target.repeatEnabled)) {
            meshcore::Bytes bytes{11};
            word(bytes, self.frequencyKhz);
            word(bytes, self.bandwidthHz);
            bytes.push_back(static_cast<std::uint8_t>(self.spreadingFactor));
            bytes.push_back(static_cast<std::uint8_t>(self.codingRate));
            if (repeat)
                bytes.push_back(*repeat ? 1 : 0);
            plan.settings.push_back(std::move(bytes));
        }
    }
    if (settings.contains(QLatin1String("txPower"))) {
        const int power = held.maxPowerDbm > 0 ? std::min(self.powerDbm, held.maxPowerDbm) : self.powerDbm;
        if (power != held.powerDbm)
            plan.settings.push_back({12, static_cast<std::uint8_t>(power)});
    }
    if (settings.contains(QLatin1String("location")) &&
        (self.latitude != held.latitude || self.longitude != held.longitude)) {
        meshcore::Bytes bytes{14};
        word(bytes, static_cast<std::uint32_t>(self.latitude));
        word(bytes, static_cast<std::uint32_t>(self.longitude));
        plan.settings.push_back(std::move(bytes));
    }
    if (settings.contains(QLatin1String("other")) &&
        (self.manualAddContacts != held.manualAddContacts ||
         self.telemetryModes != (held.telemetryModes & 0x3f) ||
         self.advertLocPolicy != held.advertLocPolicy || self.multiAcks != held.multiAcks)) {
        plan.settings.push_back({38, self.manualAddContacts, self.telemetryModes,
                                 self.advertLocPolicy, self.multiAcks});
    }
    if (settings.contains(QLatin1String("autoAdd"))) {
        const QVariantMap additions = settings.value(QLatin1String("autoAdd")).toMap();
        plan.settings.push_back({58, static_cast<std::uint8_t>(additions.value(QLatin1String("config")).toInt()),
                                 static_cast<std::uint8_t>(qMin(64, additions.value(QLatin1String("maxHops")).toInt()))});
    }
    if (source.pathHashMode >= 0 && target.pathHashMode >= 0 && source.pathHashMode != target.pathHashMode)
        plan.settings.push_back({61, 0, static_cast<std::uint8_t>(source.pathHashMode)});
}

} // namespace

QByteArray encode(const RadioBackupSnapshot& source, const QByteArray& privateKey,
                  const QByteArray& belleHistory, QString& error) {
    error.clear();
    QByteArray publicBytes;
    QString name;
    if (!hexBytes(source.publicKey, 32, publicBytes) || publicBytes == QByteArray(32, '\0') ||
        !wireName(fromWire(source.identity.name), 31, name)) {
        fail(error, "Рация ещё не сообщила корректное имя и открытый ключ.");
        return QByteArray();
    }
    if (!privateKey.isEmpty() && !radioKeyMatchesPublic(privateKey, publicBytes)) {
        fail(error, "Приватный ключ не совпадает с открытым ключом рации.");
        return QByteArray();
    }
    if (belleHistory.size() > MaxFileBytes * 3 / 4 || source.contacts.size() > contactLimit ||
        source.channels.size() > 256) {
        fail(error, "Резервная копия рации превышает допустимый размер.");
        return QByteArray();
    }
    const QVariantMap settings = snapshotSettings(source);
    RadioBackupSnapshot checked;
    if (!readSettings(settings, checked)) {
        fail(error, "Настройки рации не подходят для резервной копии.");
        return QByteArray();
    }

    QVariantList channels;
    QVariantList historyChannels;
    for (int i = 0; i < source.channels.size(); ++i) {
        const RadioChannel& channel = source.channels.at(i);
        QVariantMap record;
        record.insert(QLatin1String("name"), channel.title);
        record.insert(QLatin1String("secret"), channel.key.toLower());
        historyChannels.append(record);
        record.insert(QLatin1String("slot"), channel.slot);
        channels.append(record);
    }
    QVariantList contacts;
    for (const auto& contact : source.contacts)
        contacts.append(contactJson(contact));
    if (!readChannels(channels, checked.channels) || !readContacts(contacts, checked.contacts)) {
        fail(error, "В рации есть контакт или канал с неподдерживаемыми данными.");
        return QByteArray();
    }
    const QString publicKey = hexText(publicBytes);
    const QString saved = isoUtc(QDateTime::currentDateTime());
    QVariantMap historyRadio;
    historyRadio.insert(QLatin1String("key"), publicKey);
    historyRadio.insert(QLatin1String("name"), name);
    QVariantMap history;
    history.insert(QLatin1String("format"), QLatin1String("ommesh-history"));
    history.insert(QLatin1String("version"), 1);
    history.insert(QLatin1String("exportedAt"), saved);
    history.insert(QLatin1String("app"), QLatin1String(author));
    history.insert(QLatin1String("radio"), historyRadio);
    history.insert(QLatin1String("channels"), historyChannels);
    history.insert(QLatin1String("contacts"), contacts);
    history.insert(QLatin1String("messages"), QVariantList());

    QVariantMap radio;
    radio.insert(QLatin1String("name"), name);
    radio.insert(QLatin1String("publicKey"), publicKey);
    radio.insert(QLatin1String("settings"), settings);
    radio.insert(QLatin1String("channels"), channels);
    QVariantMap data;
    data.insert(QLatin1String("format"), QLatin1String("ommesh-backup"));
    data.insert(QLatin1String("version"), 1);
    data.insert(QLatin1String("saved"), saved);
    data.insert(QLatin1String("by"), QLatin1String(author));
    data.insert(QLatin1String("key"), privateKey.isEmpty()
                                    ? QVariant() : QVariant(keyObject(name, publicKey, privateKey)));
    data.insert(QLatin1String("radio"), radio);
    data.insert(QLatin1String("history"), history);
    QVariantMap app;
    app.insert(QLatin1String("settings"), QVariantMap());
    data.insert(QLatin1String("app"), app);
    if (!belleHistory.isEmpty()) {
        const QByteArray base64 = belleHistory.toBase64();
        data.insert(QLatin1String("belleHistory"), QString::fromLatin1(base64.constData(), base64.size()));
    }

    const QByteArray archive = BackupJson::writeObject(data);
    if (archive.isEmpty() || archive.size() > MaxFileBytes)
        fail(error, "Резервная копия рации превышает допустимый размер.");
    return error.isEmpty() ? archive : QByteArray();
}

bool decode(const QByteArray& json, Archive& out, QString& error) {
    error.clear();
    QVariantMap file;
    if (!BackupJson::readObject(json, file) ||
        file.value(QLatin1String("format")).toString() != QLatin1String("ommesh-backup"))
        return fail(error, "Файл не является резервной копией рации OMMesh.");
    if (!versionOne(file))
        return fail(error, "Версия резервной копии рации не поддерживается.");

    Archive archive;
    const QVariantMap radio = file.value(QLatin1String("radio")).toMap();
    QString name;
    if (!wireName(radio.value(QLatin1String("name")), 31, name) ||
        !arrayKey(radio.value(QLatin1String("publicKey")).toString(), archive.source.identity.key) ||
        !positiveKey(archive.source.identity.key) ||
        !isoDate(file.value(QLatin1String("saved")), archive.saved) ||
        file.value(QLatin1String("by")).type() != QVariant::String ||
        radio.value(QLatin1String("settings")).type() != QVariant::Map)
        return fail(error, "В резервной копии не удалось прочитать сведения о рации.");
    archive.source.identity.name = toWire(name);
    archive.source.publicKey = hexText(keyBytes(archive.source.identity.key));
    archive.settings = radio.value(QLatin1String("settings")).toMap();
    if (!readSettings(archive.settings, archive.source))
        return fail(error, "В резервной копии неверно записаны настройки рации.");
    if (!readChannels(radio.value(QLatin1String("channels")), archive.source.channels))
        return fail(error, "В резервной копии неверно записаны каналы рации.");

    const QVariant key = file.value(QLatin1String("key"));
    if (key.isValid()) {
        KeyFile identity;
        if (key.type() != QVariant::Map || !decodeKeyObject(key.toMap(), archive.source.publicKey, identity))
            return fail(error, "Приватный ключ в резервной копии не совпадает с открытым ключом рации.");
        archive.privateKey = identity.privateKey;
    }

    const QVariantMap history = file.value(QLatin1String("history")).toMap();
    const QString historyKey = history.value(QLatin1String("radio")).toMap().value(QLatin1String("key")).toString();
    if (history.value(QLatin1String("format")).toString() != QLatin1String("ommesh-history") ||
        !versionOne(history) || historyKey.toLower() != archive.source.publicKey)
        return fail(error, "История в резервной копии относится к другой рации или её формат не поддерживается.");
    if (!readContacts(history.value(QLatin1String("contacts")), archive.source.contacts))
        return fail(error, "В резервной копии неверно записаны контакты рации.");
    const QVariant messages = history.value(QLatin1String("messages"));
    if (messages.isValid() && messages.type() != QVariant::List)
        return fail(error, "В резервной копии неверно записаны сообщения.");
    archive.messageCount = messages.toList().size();
    if (archive.messageCount > 0) {
        archive.androidHistory = BackupJson::writeObject(history);
        if (archive.androidHistory.isEmpty())
            return fail(error, "История сообщений превышает допустимый размер.");
    }

    const QVariant extension = file.value(QLatin1String("belleHistory"));
    if (extension.isValid()) {
        if (extension.type() != QVariant::String)
            return fail(error, "В резервной копии повреждена история OMMesh Belle.");
        const QByteArray bytes = extension.toString().toLatin1();
        archive.belleHistory = QByteArray::fromBase64(bytes);
        if (archive.belleHistory.toBase64() != bytes)
            return fail(error, "В резервной копии повреждена история OMMesh Belle.");
    }
    out = std::move(archive);
    return true;
}

QByteArray encodeKey(const RadioBackupSnapshot& source, const QByteArray& privateKey, QString& error) {
    error.clear();
    QByteArray publicBytes;
    QString name;
    if (!wireName(fromWire(source.identity.name), 31, name) ||
        !hexBytes(source.publicKey, 32, publicBytes) ||
        !radioKeyMatchesPublic(privateKey, publicBytes)) {
        fail(error, "Не удалось проверить приватный ключ рации.");
        return QByteArray();
    }
    QVariantMap file = keyObject(name, hexText(publicBytes), privateKey);
    file.insert(QLatin1String("format"), QLatin1String("ommesh-key"));
    file.insert(QLatin1String("version"), 1);
    file.insert(QLatin1String("saved"), isoUtc(QDateTime::currentDateTime()));
    file.insert(QLatin1String("by"), QLatin1String(author));
    return BackupJson::writeObject(file);
}

bool decodeKey(const QByteArray& input, KeyFile& out, QString& error) {
    error.clear();
    if (input.isEmpty() || input.size() > MaxFileBytes)
        return fail(error, "Файл приватного ключа пуст или слишком большой.");
    const QByteArray trimmed = (input.startsWith("\xef\xbb\xbf") ? input.mid(3) : input).trimmed();
    KeyFile result;
    if (trimmed.startsWith('{')) {
        QVariantMap file;
        if (!BackupJson::readObject(trimmed, file) || !versionOne(file))
            return fail(error, "Формат файла приватного ключа не поддерживается.");
        const QString format = file.value(QLatin1String("format")).toString();
        const QVariant saved = file.value(QLatin1String("saved"));
        if (saved.isValid() && !isoDate(saved, result.saved))
            return fail(error, "Дата в файле приватного ключа недействительна.");
        if (format == QLatin1String("ommesh-backup")) {
            const QString claimed = file.value(QLatin1String("radio")).toMap().value(QLatin1String("publicKey")).toString();
            QByteArray publicBytes;
            if (!hexBytes(claimed, 32, publicBytes) ||
                !decodeKeyObject(file.value(QLatin1String("key")).toMap(), claimed, result))
                return fail(error, "В резервной копии нет подходящего приватного ключа.");
        } else if (format != QLatin1String("ommesh-key") || !decodeKeyObject(file, QString(), result)) {
            return fail(error, "Файл приватного ключа повреждён или ключ не соответствует рации.");
        }
    } else {
        if (trimmed.size() > 4096)
            return fail(error, "Текст приватного ключа слишком большой.");
        const QString raw = QString::fromUtf8(trimmed.constData(), trimmed.size());
        QString digits;
        for (int i = 0; i < raw.size(); ++i) {
            if (!raw.at(i).isSpace())
                digits += raw.at(i);
        }
        if (digits.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
            digits.remove(0, 2);
        if (!hexBytes(digits, 64, result.privateKey))
            return fail(error, "Нужно 128 шестнадцатеричных знаков приватного ключа MeshCore.");
        const QByteArray publicBytes = deriveRadioPublicKey(result.privateKey);
        if (publicBytes.isEmpty())
            return fail(error, "Это недействительный приватный ключ MeshCore.");
        result.publicKey = hexText(publicBytes);
    }
    out = std::move(result);
    return true;
}

bool planRestore(const Archive& archive, const RadioBackupSnapshot& target, unsigned parts,
                 RadioBackupRestore& out, QString& error) {
    error.clear();
    QByteArray targetKey;
    QByteArray sourceKey;
    if (!hexBytes(target.publicKey, 32, targetKey) || targetKey == QByteArray(32, '\0') ||
        !hexBytes(archive.source.publicKey, 32, sourceKey))
        return fail(error, "Нужно подключиться к рации и дождаться её открытого ключа.");

    RadioBackupRestore plan;
    plan.expectedTarget = hexText(targetKey);
    plan.sourcePublicKey = hexText(sourceKey);
    const bool same = targetKey == sourceKey;
    if ((parts & Identity) && archive.hasKey() && !same) {
        if (!radioKeyMatchesPublic(archive.privateKey, sourceKey))
            return fail(error, "Приватный ключ не совпадает с открытым ключом копии.");
        plan.privateKey = archive.privateKey;
    }
    if (parts & Settings) {
        RadioBackupSnapshot validated;
        if (!readSettings(archive.settings, validated))
            return fail(error, "Настройки рации в резервной копии недействительны.");
        buildSettings(archive.settings, validated, target, plan);
    }
    if (!plan.privateKey.isEmpty() || (same && (parts & Settings))) {
        QString name;
        if (!wireName(fromWire(archive.source.identity.name), 31, name))
            return fail(error, "Имя рации в резервной копии недействительно.");
        appendName(plan, toWire(name), target.identity.name);
    }
    if (parts & Channels)
        plan.channels = archive.source.channels;
    if (parts & Contacts) {
        plan.contacts = archive.source.contacts;
        std::stable_sort(plan.contacts.begin(), plan.contacts.end(), [](const auto& left, const auto& right) {
            const bool leftFavourite = left.flags & 1;
            const bool rightFavourite = right.flags & 1;
            if (leftFavourite != rightFavourite)
                return leftFavourite;
            return left.lastAdvert > right.lastAdvert;
        });
    }
    out = std::move(plan);
    return true;
}

bool planKeyRestore(const KeyFile& key, const RadioBackupSnapshot& target,
                    RadioBackupRestore& out, QString& error) {
    error.clear();
    QByteArray targetKey;
    QByteArray sourceKey;
    QString name;
    if (!hexBytes(target.publicKey, 32, targetKey) || targetKey == QByteArray(32, '\0'))
        return fail(error, "Нужно подключиться к рации и дождаться её открытого ключа.");
    if (!hexBytes(key.publicKey, 32, sourceKey) || !radioKeyMatchesPublic(key.privateKey, sourceKey) ||
        !wireName(key.name, 31, name))
        return fail(error, "Не удалось проверить приватный ключ и имя рации.");
    RadioBackupRestore plan;
    plan.expectedTarget = hexText(targetKey);
    plan.sourcePublicKey = hexText(sourceKey);
    if (sourceKey != targetKey)
        plan.privateKey = key.privateKey;
    appendName(plan, toWire(name), target.identity.name);
    out = std::move(plan);
    return true;
}

} // namespace RadioBackupArchive
