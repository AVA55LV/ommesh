#include "link_preview.hpp"

#include "native_tile_https.hpp"

#include <QtCore/QAtomicInt>
#include <QtCore/QBuffer>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QRegExp>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryFile>
#include <QtCore/QThread>
#include <QtCore/QVariant>
#include <QtCore/QCryptographicHash>
#include <QtGui/QDesktopServices>
#include <QtGui/QImageReader>
#include <QtNetwork/QHostAddress>
#ifndef Q_OS_SYMBIAN
#include <QtCore/QEventLoop>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#endif

namespace LinkPreview {
namespace {

const int maximumReply = 256 * 1024;
const int maximumLinks = 12;
const int maximumMemoryCards = 24;
const int maximumCacheFiles = 48;
const qint64 hour = 3600000;
const qint64 day = 24 * hour;

qint64 now() {
    return QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
}

bool publicHost(QString host) {
    if (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    QHostAddress address;
    if (address.setAddress(host)) {
        if (address.protocol() == QAbstractSocket::IPv6Protocol) {
            const Q_IPV6ADDR bytes = address.toIPv6Address();
            return (bytes[0] & 0xe0) == 0x20 &&
                !(bytes[0] == 0x20 && bytes[1] == 0x02) &&
                !(bytes[0] == 0x20 && bytes[1] == 0x01 &&
                  ((bytes[2] == 0 && bytes[3] == 0) || (bytes[2] == 0x0d && bytes[3] == 0xb8)));
        }
        const quint32 bytes = address.toIPv4Address();
        const int first = (bytes >> 24) & 255;
        const int second = (bytes >> 16) & 255;
        const int third = (bytes >> 8) & 255;
        return first != 0 && first != 10 && first != 127 && first < 224 &&
            !(first == 100 && second >= 64 && second <= 127) &&
            !(first == 169 && second == 254) &&
            !(first == 172 && second >= 16 && second <= 31) &&
            !(first == 192 && (second == 168 || (second == 0 && (third == 0 || third == 2)))) &&
            !(first == 198 && (second == 18 || second == 19 || (second == 51 && third == 100))) &&
            !(first == 203 && second == 0 && third == 113);
    }
    if (!host.contains(QLatin1Char('.')))
        return false;
    // Ambiguous numeric forms can be interpreted as IP addresses by HTTP clients.
    const QString last = host.mid(host.lastIndexOf(QLatin1Char('.')) + 1);
    if (last.isEmpty() || last.at(0).isDigit())
        return false;
    const QString lower = host.toLower();
    const char* localNames[] = {"localhost", "local", "lan", "home", "internal", "intranet",
                                "corp", "private", "localdomain", "home.arpa", "test"};
    for (const char* name : localNames) {
        const QString local = QLatin1String(name);
        if (lower == local || lower.endsWith(QLatin1Char('.') + local))
            return false;
    }
    return true;
}

bool previewable(const QUrl& url) {
    const int port = url.port();
    return url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) &&
        url.userInfo().isEmpty() && (port < 0 || port == 80 || port == 443) &&
        publicHost(url.host()) && url.toEncoded().size() <= 2048;
}

// Qt 4 has no JSON decoder. Parse the bounded response as data, including
// proper escapes, without evaluating it as code or feeding it into rich text.
class JsonReader {
public:
    explicit JsonReader(const QByteArray& bytes) : text_(QString::fromUtf8(bytes.constData(), bytes.size())) {}

    QVariantMap object() {
        QVariant result;
        if (!value(result, 0))
            return QVariantMap();
        spaces();
        return position_ == text_.size() ? result.toMap() : QVariantMap();
    }

private:
    void spaces() {
        while (position_ < text_.size() && text_.at(position_).isSpace())
            ++position_;
    }

    QChar peek() const {
        return position_ < text_.size() ? text_.at(position_) : QChar();
    }

    bool consume(char character) {
        spaces();
        if (peek() != QLatin1Char(character))
            return false;
        ++position_;
        return true;
    }

    bool string(QString& result) {
        if (!consume('"'))
            return false;
        while (position_ < text_.size()) {
            const QChar character = text_.at(position_++);
            if (character == QLatin1Char('"'))
                return true;
            if (character.unicode() < 32)
                return false;
            if (character != QLatin1Char('\\')) {
                result += character;
                continue;
            }
            if (position_ == text_.size())
                return false;
            const QChar escaped = text_.at(position_++);
            switch (escaped.unicode()) {
            case '"': case '/': case '\\': result += escaped; break;
            case 'b': result += QLatin1Char('\b'); break;
            case 'f': result += QLatin1Char('\f'); break;
            case 'n': result += QLatin1Char('\n'); break;
            case 'r': result += QLatin1Char('\r'); break;
            case 't': result += QLatin1Char('\t'); break;
            case 'u': {
                if (text_.size() - position_ < 4)
                    return false;
                const QString code = text_.mid(position_, 4);
                for (int i = 0; i < code.size(); ++i)
                    if (!QString::fromLatin1("0123456789abcdefABCDEF").contains(code.at(i)))
                        return false;
                result += QChar(code.toUShort(nullptr, 16));
                position_ += 4;
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool value(QVariant& result, int depth) {
        spaces();
        if (depth > 12 || position_ == text_.size())
            return false;
        if (peek() == QLatin1Char('"')) {
            QString decoded;
            if (!string(decoded))
                return false;
            result = decoded;
            return true;
        }
        if (consume('{')) {
            QVariantMap fields;
            if (!consume('}')) {
                do {
                    QString key;
                    QVariant child;
                    if (fields.size() >= 64 || !string(key) || fields.contains(key) ||
                            !consume(':') || !value(child, depth + 1))
                        return false;
                    fields.insert(key, child);
                } while (consume(','));
                if (!consume('}'))
                    return false;
            }
            result = fields;
            return true;
        }
        if (consume('[')) {
            QVariantList children;
            if (!consume(']')) {
                do {
                    QVariant child;
                    if (children.size() >= 64 || !value(child, depth + 1))
                        return false;
                    children.append(child);
                } while (consume(','));
                if (!consume(']'))
                    return false;
            }
            result = children;
            return true;
        }
        const int start = position_;
        while (position_ < text_.size() && position_ - start < 64 && !peek().isSpace() &&
               !QString::fromLatin1(",]}").contains(peek()))
            ++position_;
        const QString raw = text_.mid(start, position_ - start);
        if (raw == QLatin1String("null"))
            return true;
        if (raw == QLatin1String("true") || raw == QLatin1String("false")) {
            result = raw == QLatin1String("true");
            return true;
        }
        const QRegExp number(QLatin1String("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?"));
        if (!number.exactMatch(raw))
            return false;
        bool valid = false;
        const double parsed = raw.toDouble(&valid);
        result = parsed;
        return valid;
    }

    QString text_;
    int position_ = 0;
};

QString label(const QVariant& value, int limit) {
    if (value.type() != QVariant::String)
        return QString();
    QString text = value.toString().left(limit);
    text.remove(QRegExp(QString::fromLatin1("[\\x0000-\\x001f\\x007f\\x202a-\\x202e\\x2066-\\x2069]")));
    return text.simplified();
}

QImage picture(const QVariantMap& image) {
    const QString type = image.value(QLatin1String("type")).toString();
    const QByteArray format = type == QLatin1String("image/jpeg") ? QByteArray("jpeg") :
                              type == QLatin1String("image/png") ? QByteArray("png") : QByteArray();
    if (format.isEmpty())
        return QImage();
    const QString encoded = image.value(QLatin1String("data")).toString();
    if (encoded.size() > maximumReply)
        return QImage();
    QByteArray data = QByteArray::fromBase64(encoded.toLatin1());
    QBuffer buffer(&data);
    if (!buffer.open(QIODevice::ReadOnly))
        return QImage();
    QImageReader reader(&buffer, format);
    reader.setAutoDetectImageFormat(false);
    QSize size = reader.size();
    if (!size.isValid() || size.width() > 1024 || size.height() > 1024)
        return QImage();
    if (size.width() > 256 || size.height() > 156) {
        size.scale(256, 156, Qt::KeepAspectRatio);
        reader.setScaledSize(size);
    }
    return reader.read();
}

QDir cacheDirectory() {
    const QString root = QDesktopServices::storageLocation(QDesktopServices::CacheLocation);
    if (root.isEmpty())
        return QDir();
    return QDir(root + QLatin1String("/link-previews"));
}

QString cacheName(const QUrl& url) {
    return QString::fromLatin1(QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha1).toHex()) +
        QLatin1String(".preview");
}

QByteArray cached(const QUrl& url) {
    const QDir dir = cacheDirectory();
    if (dir.isRelative())
        return QByteArray();
    QFile file(dir.filePath(cacheName(url)));
    const QFileInfo info(file);
    const qint64 age = now() - info.lastModified().toMSecsSinceEpoch();
    if (age < 0 || age >= day || info.size() > maximumReply || !file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.read(maximumReply);
}

void cache(const QUrl& url, const QByteArray& data) {
    QDir dir = cacheDirectory();
    if (dir.isRelative() || !QDir().mkpath(dir.path()))
        return;
    QTemporaryFile temporary(dir.filePath(QLatin1String(".preview-XXXXXX")));
    if (!temporary.open() || temporary.write(data) != data.size() || !temporary.flush())
        return;
    const QString destination = dir.filePath(cacheName(url));
    QFile::remove(destination);
    if (!temporary.rename(destination))
        return;
    temporary.setAutoRemove(false);
    const QStringList files = dir.entryList(QStringList(QLatin1String("*.preview")), QDir::Files, QDir::Time);
    for (int i = maximumCacheFiles; i < files.size(); ++i)
        dir.remove(files.at(i));
}

} // namespace

Mode preference() {
    const int stored = QSettings().value(QLatin1String("ui/linkPreviewMode"), Off).toInt();
    return stored == OnTap ? OnTap : stored == Automatic ? Automatic : Off;
}

QUrl previewAddress(QUrl url) {
    url.setFragment(QString());
    return url;
}

QList<Link> linksIn(const QString& text) {
    QRegExp pattern(QString::fromUtf8("(?:https?://|www\\.)[^\\s<>\"«»]+"), Qt::CaseInsensitive);
    QList<Link> links;
    for (int position = 0; links.size() < maximumLinks && (position = pattern.indexIn(text, position)) >= 0;) {
        QString token = pattern.cap(0);
        const int start = position;
        position += token.size();
        while (!token.isEmpty()) {
            const QChar last = token.at(token.size() - 1);
            const QString closing = QLatin1String(")]}");
            const int bracket = closing.indexOf(last);
            if (!QString::fromUtf8(".,;:!?'…").contains(last) &&
                    (bracket < 0 || token.count(QString::fromLatin1("([{").at(bracket)) >= token.count(last)))
                break;
            token.chop(1);
        }
        if (token.startsWith(QLatin1String("www."), Qt::CaseInsensitive)) {
            if (token.size() <= 4)
                continue;
        }
        const bool hasScheme = token.startsWith(QLatin1String("http"), Qt::CaseInsensitive);
        const QUrl address(hasScheme ? token : QLatin1String("https://") + token, QUrl::StrictMode);
        if (address.isValid() && !address.host().isEmpty() && address.userInfo().isEmpty())
            links.append(Link{start, token.size(), address, previewable(address)});
    }
    return links;
}

Card parseAnswer(const QByteArray& json) {
    Card result;
    result.state = Failed;
    if (json.isEmpty() || json.size() > maximumReply)
        return result;
    const QVariantMap fields = JsonReader(json).object();
    if (fields.contains(QLatin1String("refused")))
        return result;
    const QString kind = fields.value(QLatin1String("kind")).toString();
    if (kind == QLatin1String("none")) {
        result.state = Empty;
        return result;
    }
    if (kind != QLatin1String("page") && kind != QLatin1String("picture") && kind != QLatin1String("file"))
        return result;
    result.host = label(fields.value(QLatin1String("host")), 100);
    if (result.host.isEmpty())
        return result;
    result.title = label(fields.value(QLatin1String("title")), 160);
    result.description = label(fields.value(QLatin1String("description")), 220);
    const QVariantMap file = fields.value(QLatin1String("file")).toMap();
    if (result.title.isEmpty())
        result.title = label(file.value(QLatin1String("name")), 160);
    if (result.description.isEmpty()) {
        const qint64 size = file.value(QLatin1String("size")).toLongLong();
        if (size > 0)
            result.description = QString::fromUtf8("%1 КБ").arg(size / 1024 + (size % 1024 != 0));
    }
    result.image = picture(fields.value(QLatin1String("image")).toMap());
    result.state = Ready;
    return result;
}

class Worker final : public QThread {
public:
    explicit Worker(const QUrl& url) : address(url) {
        setStackSize(96 * 1024);
    }

    void cancel() { cancelled_.fetchAndStoreRelease(1); }
    bool cancelled() { return cancelled_.fetchAndAddAcquire(0) != 0; }

    const QUrl address;
    QByteArray data;

protected:
    void run() override {
        if (cancelled())
            return;
#ifdef Q_OS_SYMBIAN
        data = fetchOmmeshLinkPreview(address.toEncoded(), &cancelled_, maximumReply);
#else
        QNetworkAccessManager network;
        const QByteArray query = QUrl::toPercentEncoding(QString::fromLatin1(address.toEncoded()));
        const QUrl endpoint = QUrl::fromEncoded("https://preview.ommesh.ru/v1/preview?url=" + query);
        QNetworkRequest request(endpoint);
        request.setRawHeader("Accept", "application/json");
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        QNetworkReply* reply = network.get(request);
        reply->setReadBufferSize(maximumReply + 1);
        QTimer pulse;
        QEventLoop wait;
        connect(&pulse, SIGNAL(timeout()), &wait, SLOT(quit()));
        connect(reply, SIGNAL(readyRead()), &wait, SLOT(quit()));
        connect(reply, SIGNAL(finished()), &wait, SLOT(quit()));
        pulse.start(100);
        QElapsedTimer clock;
        clock.start();
        QByteArray body;
        while (!cancelled() && clock.elapsed() < 30000) {
            body += reply->read(maximumReply + 1 - body.size());
            if (body.size() > maximumReply)
                break;
            if (reply->isFinished()) {
                const QByteArray type = reply->rawHeader("Content-Type").split(';').first().trimmed().toLower();
                if (reply->error() == QNetworkReply::NoError && type == "application/json" &&
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200)
                    data = body;
                return;
            }
            wait.exec();
        }
        reply->abort();
#endif
    }

private:
    QAtomicInt cancelled_{0};
};

Store::Store(QObject* parent) : QObject(parent) {
    timer_.setSingleShot(true);
    connect(&timer_, SIGNAL(timeout()), this, SLOT(pump()));
}

Store::~Store() {
    if (!worker_)
        return;
    worker_->cancel();
    if (worker_->wait(50)) {
        delete worker_;
        return;
    }
    connect(worker_, SIGNAL(finished()), worker_, SLOT(deleteLater()), Qt::QueuedConnection);
    if (worker_->isFinished())
        worker_->deleteLater();
}

void Store::setMode(Mode mode) {
    if (mode_ == mode)
        return;
    mode_ = mode;
    if (mode_ == Off) {
        manual_.clear();
        visible_.clear();
        timer_.stop();
        if (worker_)
            worker_->cancel();
    } else {
        schedule();
    }
    emit changed();
}

void Store::setVisible(const QList<QUrl>& addresses) {
    if (visible_ == addresses)
        return;
    visible_ = addresses;
    if (mode_ == Automatic)
        schedule();
}

Card Store::card(const QUrl& address) const {
    if (worker_ && worker_->address == address)
        return Card{Loading, QString(), QString(), QString(), QImage()};
    const auto entry = cards_.constFind(address);
    return entry != cards_.constEnd() && entry.value().expires > now() ? entry.value().card : Card();
}

void Store::request(const QUrl& address) {
    if (mode_ == Off || !previewable(address) || manual_.contains(address) || manual_.size() >= maximumLinks)
        return;
    const State state = card(address).state;
    if (state == Missing || state == Failed) {
        manual_.append(address);
        schedule();
    }
}

void Store::schedule(int delay) {
    if (mode_ != Off && !worker_ && (!timer_.isActive() || delay == 0))
        timer_.start(delay);
}

void Store::remember(const QUrl& address, const Card& value, qint64 expires) {
    recent_.removeAll(address);
    recent_.append(address);
    cards_.insert(address, Stored{value, expires});
    while (recent_.size() > maximumMemoryCards) {
        int victim = 0;
        while (victim < recent_.size() - 1 && visible_.contains(recent_.at(victim)))
            ++victim;
        cards_.remove(recent_.takeAt(victim));
    }
}

void Store::pump() {
    if (mode_ == Off || worker_)
        return;
    const qint64 started = now();
    while (!autoTimes_.isEmpty() && started - autoTimes_.first() >= 60000)
        autoTimes_.removeFirst();
    while (true) {
        const bool manual = !manual_.isEmpty();
        QUrl address;
        if (manual) {
            address = manual_.takeFirst();
        } else if (mode_ == Automatic) {
            for (const QUrl& visible : visible_) {
                if (card(visible).state == Missing) {
                    address = visible;
                    break;
                }
            }
        }
        if (address.isEmpty())
            return;
        const QByteArray stored = cached(address);
        if (!stored.isEmpty()) {
            const Card value = parseAnswer(stored);
            if (value.state == Ready || value.state == Empty) {
                remember(address, value, started + hour);
                emit changed();
                continue;
            }
        }
        if (!manual && autoTimes_.size() >= 20) {
            schedule(qMax<qint64>(1, 60000 - (started - autoTimes_.first())));
            return;
        }
        if (!manual)
            autoTimes_.append(started);
        worker_ = new Worker(address);
        connect(worker_, SIGNAL(finished()), this, SLOT(completed()), Qt::QueuedConnection);
        worker_->start();
        emit changed();
        return;
    }
}

void Store::completed() {
    Worker* finished = worker_;
    worker_ = nullptr;
    if (!finished->cancelled() && mode_ != Off) {
        const Card value = parseAnswer(finished->data);
        const bool cacheable = value.state == Ready || value.state == Empty;
        if (cacheable)
            cache(finished->address, finished->data);
        remember(finished->address, value, now() + (cacheable ? day : 120000));
    }
    finished->deleteLater();
    emit changed();
    schedule();
}

} // namespace LinkPreview
