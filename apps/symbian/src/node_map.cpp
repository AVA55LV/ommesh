#include "node_map.hpp"
#include "appearance.hpp"
#include "client.hpp"
#include "local_tile_proxy.hpp"
#include "settings_icons.hpp"

#include <QtCore/QCache>
#include <QtCore/QCoreApplication>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QLocale>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QButtonGroup>
#include <QtGui/QDesktopServices>
#include <QtGui/QGridLayout>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QLabel>
#include <QtGui/QMenu>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPolygon>
#include <QtGui/QPushButton>
#include <QtGui/QToolButton>
#include <QtGui/QVBoxLayout>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkDiskCache>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

const QRgb Background = 0x151c22;
const QRgb Chrome = 0x1b242c;
const QRgb Paper = 0xeaf1f4;
const QRgb Muted = 0xa3b1b8;
const QRgb Accent = 0x62d5be;
const QRgb Repeater = 0xf2bc73;
const QRgb Room = 0xb5a7ea;
const QRgb Sensor = 0x80badc;
const QRgb AtlasBackground = 0x1c272a;
const QRgb AtlasColors[] = {0x385969, 0x507e9b, 0x61646a, 0x49584e, 0x887767, 0xb99a73,
                           0x80a796, 0x8e819c, 0x608da6};
const int RoadLevels[] = {11, 1, 11, 12, 10, 1};
const int AtlasFeatureKinds = sizeof(AtlasColors) / sizeof(AtlasColors[0]);
const int AtlasVertexLimit = 48000;
const int TileSide = 256;
const int MaximumTileBytes = 160 * 1024;
const int MaximumOnlineZoom = 17;
const int MaximumOfflineZoom = 19;
const int MinimumOnlineZoom = 1;
const int CameraDuration = 165;
const int CameraFrame = 26;
const int EmptyX = 73375000;
const int EmptyY = 55000000;
const double Pi = 3.14159265358979323846;
const double FullLongitude = 360000000.0;
const double MaxLatitude = 85.05112878;

QString ru(const char* text) { return QString::fromUtf8(text); }
QColor color(QRgb rgb) { return QColor::fromRgb(rgb); }

QString credit() { return ru("© OpenStreetMap contributors"); }

QPointF project(qint32 latitude, qint32 longitude) {
    const double degrees = std::max(-MaxLatitude, std::min(MaxLatitude, latitude / 1000000.0));
    const double radians = degrees * Pi / 180.0;
    const double x = (longitude + FullLongitude / 2) / FullLongitude;
    const double y = 0.5 - std::log(std::tan(Pi / 4 + radians / 2)) / (2 * Pi);
    return QPointF(x, std::max(0.0, std::min(1.0, y)));
}

double wrapped(double position) { return position - std::floor(position); }

double nearX(double x, double center) { return x + std::floor(center - x + 0.5); }

struct AtlasLine {
    int kind;
    QRectF bounds;
    QVector<QPointF> points;
};

struct AtlasPlace {
    int kind;
    QPointF position;
    QString name;
};

struct Atlas {
    QRectF bounds;
    QVector<AtlasLine> lines;
    QVector<AtlasPlace> places;
};

QRectF regionBounds(NodeMap::OfflineRegion region) {
    switch (region) {
        case NodeMap::OmskCity: return QRectF(project(55320000, 72700000), project(54680000, 74050000));
        case NodeMap::OmskOblast: return QRectF(project(59250000, 69000000), project(53300000, 77000000));
        case NodeMap::Siberia: return QRectF(project(83000000, 18000000), project(40000000, 180000000));
        case NodeMap::World: return QRectF(project(85000000, -180000000), project(-85000000, 180000000));
    }
    return QRectF();
}

const char* regionFile(NodeMap::OfflineRegion region) {
    switch (region) {
        case NodeMap::OmskCity: return "omsk.bin";
        case NodeMap::OmskOblast: return "omsk-oblast.bin";
        case NodeMap::Siberia: return "siberia.bin";
        case NodeMap::World: return "world.bin";
    }
    return "world.bin";
}

QString regionCaption(NodeMap::OfflineRegion region) {
    switch (region) {
        case NodeMap::OmskCity: return ru("Омск");
        case NodeMap::OmskOblast: return ru("Область");
        case NodeMap::Siberia: return ru("Сибирь");
        case NodeMap::World: return ru("Мир");
    }
    return QString();
}

QStringList atlasPaths(NodeMap::OfflineRegion region) {
    QStringList paths;
    const QString applicationPath = QCoreApplication::applicationDirPath();
    const QString leaf = QString::fromLatin1(regionFile(region));
#ifdef Q_OS_SYMBIAN
    const QString privatePath = QLatin1String("/private/e78af613/maps/") + leaf;
    if (applicationPath.size() >= 2 && applicationPath.at(1) == QLatin1Char(':')) {
        paths << (applicationPath.left(2) + privatePath);
    }
    const QFileInfoList drives = QDir::drives();
    for (int i = 0; i < drives.size(); ++i) {
        const QString root = drives.at(i).absoluteFilePath();
        if (root.size() >= 2 && root.at(1) == QLatin1Char(':')) {
            paths << (root.left(2) + privatePath);
        }
    }
    paths << (QString::fromLatin1("C:") + privatePath) << (QString::fromLatin1("E:") + privatePath)
          << (QString::fromLatin1("F:") + privatePath);
    paths.removeDuplicates();
#else
    const QByteArray selected = qgetenv("OMMESH_MAP_PATH");
    if (region == NodeMap::OmskCity && !selected.isEmpty()) {
        paths.prepend(QString::fromLocal8Bit(selected.constData(), selected.size()));
    }
    paths << (applicationPath + QLatin1String("/resources/maps/") + leaf)
          << (QLatin1String("resources/maps/") + leaf);
#endif
    return paths;
}

bool coordinate(qint64 latitude, qint64 longitude) {
    return latitude >= -90000000 && latitude <= 90000000
        && longitude >= -180000000 && longitude <= 180000000;
}

bool readAtlas(QIODevice& file, Atlas& output) {
    QDataStream input(&file);
    quint32 magic;
    qint32 west, east, south, north;
    quint16 count;
    input >> magic >> west >> east >> south >> north >> count;
    const bool legacy = magic == 0x4f4d4d31u;
    if (input.status() != QDataStream::Ok || (!legacy && magic != 0x4f4d4d32u) || count > 7000
        || west >= east || south >= north || !coordinate(south, west) || !coordinate(north, east)) {
        return false;
    }
    const int quantum = legacy ? 10 : 1000;
    int vertices = 0;
    Atlas atlas;
    atlas.bounds = QRectF(project(north, west), project(south, east));
    atlas.lines.reserve(count);
    while (count-- > 0) {
        quint8 kind;
        quint16 size;
        qint32 longitude, latitude;
        input >> kind >> size >> longitude >> latitude;
        qint64 x = longitude, y = latitude;
        vertices += size;
        if (input.status() != QDataStream::Ok || kind >= AtlasFeatureKinds || size < 2 || size > 512
            || vertices > AtlasVertexLimit) {
            return false;
        }
        AtlasLine line;
        line.kind = kind;
        line.points.reserve(size);
        qreal minX = 1, maxX = 0, minY = 1, maxY = 0;
        for (int i = 0; i < size; ++i) {
            if (i > 0) {
                qint16 dx, dy;
                input >> dx >> dy;
                if (input.status() != QDataStream::Ok) {
                    return false;
                }
                x += dx;
                y += dy;
            }
            if (!coordinate(y * quantum, x * quantum)) {
                return false;
            }
            const QPointF point = project(static_cast<qint32>(y * quantum), static_cast<qint32>(x * quantum));
            minX = qMin(minX, point.x());
            maxX = qMax(maxX, point.x());
            minY = qMin(minY, point.y());
            maxY = qMax(maxY, point.y());
            line.points.append(point);
        }
        line.bounds = QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
        atlas.lines.append(line);
    }
    input >> count;
    if (input.status() != QDataStream::Ok || count > 1024) {
        return false;
    }
    atlas.places.reserve(count);
    while (count-- > 0) {
        quint8 kind, length;
        qint32 longitude, latitude;
        input >> kind >> longitude >> latitude >> length;
        if (kind > 2 || length == 0 || length > 96
            || !coordinate(static_cast<qint64>(latitude) * quantum, static_cast<qint64>(longitude) * quantum)) {
            return false;
        }
        char text[96];
        if (input.readRawData(text, length) != length) {
            return false;
        }
        AtlasPlace place = {kind, project(latitude * quantum, longitude * quantum), QString::fromUtf8(text, length)};
        atlas.places.append(place);
    }
    if (input.status() != QDataStream::Ok) {
        return false;
    }
    output = atlas;
    return true;
}

struct Pin {
    QString id;
    QString name;
    int role = 0;
    QPointF position;

    bool operator==(const Pin& other) const {
        return id == other.id && name == other.name && role == other.role && position == other.position;
    }
};

bool nodePosition(const NetworkNode& node) {
    return (node.latitude != 0 || node.longitude != 0) &&
        coordinate(node.latitude, node.longitude);
}

enum MapControl { Layers, Fit, Center, Expand, ZoomIn, ZoomOut, MapControlCount };

class MapButton : public QToolButton {
public:
    MapButton(int control, const char* name, const char* caption, QWidget* parent)
        : QToolButton(parent), control_(control) {
        setObjectName(QLatin1String(name));
        describe(caption);
    }

    void describe(const char* caption) {
        const QString text = ru(caption);
        setText(text);
        setToolTip(text);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const Appearance::Palette palette = Appearance::paletteFor(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(hasFocus() || isChecked() ? palette.accent : palette.border));
        painter.setBrush(isDown() || isChecked() ? palette.selection : palette.chrome);
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);

        const QColor ink = isEnabled() ? palette.text : palette.muted;
        const int diameter = qBound(13, width() - 10, 20);
        const QPoint position = rect().center();
        if (control_ == Layers) {
            SettingsIcons::draw(painter, SettingsIcons::Map,
                QRect(position.x() - diameter / 2, position.y() - diameter / 2, diameter, diameter), ink);
            return;
        }
        painter.translate(position);
        const int edge = diameter / 2 - 2;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (control_ == Fit) {
            painter.drawRect(-edge, -edge + 2, 2 * edge, 2 * edge - 4);
            painter.drawEllipse(QPoint(0, 0), 2, 2);
        } else if (control_ == Center) {
            painter.drawEllipse(QPoint(0, 0), edge - 2, edge - 2);
            painter.drawLine(-edge, 0, -edge + 3, 0);
            painter.drawLine(edge, 0, edge - 3, 0);
            painter.drawLine(0, -edge, 0, -edge + 3);
            painter.drawLine(0, edge, 0, edge - 3);
        } else if (control_ == Expand) {
            const int side = isChecked() ? edge - 2 : edge;
            const int arm = qMin(side, 4);
            for (int x : {-1, 1}) {
                for (int y : {-1, 1}) {
                    const QPoint corner(side * x, side * y);
                    painter.drawLine(corner, QPoint((side - arm) * x, side * y));
                    painter.drawLine(corner, QPoint(side * x, (side - arm) * y));
                }
            }
        } else {
            painter.drawLine(-edge, 0, edge, 0);
            if (control_ == ZoomIn)
                painter.drawLine(0, -edge, 0, edge);
        }
    }

private:
    int control_;
};

struct TilePlacement {
    int zoom;
    int x;
    int y;
    QRect rect;

    QString path() const {
        return QString::fromLatin1("%1/%2/%3.png").arg(zoom).arg(x).arg(y);
    }
};

struct CachedImage {
    QImage image;
    QDateTime expires;
    bool transient;
};

QString cachePath() {
    QString location = QDesktopServices::storageLocation(QDesktopServices::CacheLocation);
    if (location.isEmpty()) {
        location = QDir::tempPath() + QString::fromLatin1("/OMMeshBelle");
    }
    return location + QString::fromLatin1("/tiles");
}

quint32 bigEndian(const QByteArray& data, int offset) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data.constData() + offset);
    return quint32(bytes[0]) << 24 | quint32(bytes[1]) << 16 | quint32(bytes[2]) << 8 | quint32(bytes[3]);
}

QImage decodeTile(const QByteArray& data) {
    if (data.size() < 33 || data.size() > MaximumTileBytes || bigEndian(data, 0) != 0x89504e47u
        || bigEndian(data, 4) != 0x0d0a1a0au || bigEndian(data, 8) != 13
        || bigEndian(data, 12) != 0x49484452u || bigEndian(data, 16) != TileSide
        || bigEndian(data, 20) != TileSide) {
        return QImage();
    }
    const QImage decoded = QImage::fromData(data, "PNG");
    return decoded.width() == TileSide && decoded.height() == TileSide ? decoded : QImage();
}

QByteArray headerValue(const QNetworkCacheMetaData::RawHeaderList& headers, const char* name) {
    for (int i = 0; i < headers.size(); ++i) {
        if (headers.at(i).first.toLower() == name) {
            return headers.at(i).second;
        }
    }
    return QByteArray();
}

QDateTime tileExpiry(const QNetworkCacheMetaData::RawHeaderList& headers, const QDateTime& now) {
    const QByteArray control = headerValue(headers, "cache-control").toLower();
    if (control.contains("no-cache") || control.contains("no-store")) {
        return now;
    }
    const QList<QByteArray> directives = control.split(',');
    for (int i = 0; i < directives.size(); ++i) {
        const QByteArray directive = directives.at(i).trimmed();
        const int equals = directive.indexOf('=');
        if (equals < 0 || directive.left(equals).trimmed() != "max-age") {
            continue;
        }
        QByteArray value = directive.mid(equals + 1).trimmed();
        if (value.size() > 1 && value.startsWith('"') && value.endsWith('"')) {
            value = value.mid(1, value.size() - 2);
        }
        bool valid = false;
        const qint64 seconds = value.toLongLong(&valid);
        if (valid) {
            const qint64 age = qMax<qint64>(0, headerValue(headers, "age").toLongLong());
            const qint64 maxAge = qBound<qint64>(0, seconds, 31536000);
            return now.addSecs(static_cast<int>(qMax<qint64>(0, maxAge - age)));
        }
    }
    const QByteArray expires = headerValue(headers, "expires");
    if (!expires.isEmpty()) {
        const QString field = QString::fromLatin1(expires.constData(), expires.size());
        const QString pattern = QString::fromLatin1("ddd, dd MMM yyyy HH:mm:ss 'GMT'");
        QDateTime parsed = QLocale::c().toDateTime(field, pattern);
        if (parsed.isValid()) {
            parsed.setTimeSpec(Qt::UTC);
            return parsed;
        }
    }
    return now.addDays(7);
}

bool intersects(const QRectF& lhs, const QRectF& rhs) {
    return lhs.left() <= rhs.right() && lhs.right() >= rhs.left()
        && lhs.top() <= rhs.bottom() && lhs.bottom() >= rhs.top();
}

const int BuildingZoom = 14;
const int HouseNumberZoom = 16;
const int BuildingSectorZoom = 15;
const int BuildingSectorsPerAxis = 1 << BuildingSectorZoom;
const int SectorSubdivisions = 4096;
const int MaximumSectorBytes = 256 * 1024;
const QRgb BuildingFill = 0x43545c;
const QRgb BuildingOutline = 0x657982;
const QRgb HouseNumberText = 0xffe1ab;

struct BuildingShape {
    QVector<QPolygon> rings;
    QRect bounds;
};

struct HouseNumber {
    QPoint anchor;
    QString value;
};

struct BuildingSector {
    QVector<BuildingShape> shapes;
    QVector<HouseNumber> numbers;
};

quint32 sectorKey(int x, int y) {
    return (static_cast<quint32>(x) << 16) | static_cast<quint32>(y);
}

QPoint sectorPosition(quint32 key) {
    return QPoint(key >> 16, key & 0xffff);
}

class BuildingLayer {
public:
    BuildingLayer() : file_(QLatin1String(":/maps/omsk-buildings.bin")), sectors_(3 * 1024 * 1024) {}

    int open() {
        if (tried_) {
            return index_.size();
        }
        tried_ = true;
        if (!file_.open(QIODevice::ReadOnly) || !readIndex()) {
            index_.clear();
            file_.close();
        }
        return index_.size();
    }

    bool tried() const { return tried_; }

    const BuildingSector* cached(quint32 key) const { return sectors_.object(key); }

    void releaseSectors() { sectors_.clear(); }

    bool load(quint32 key) {
        if (sectors_.contains(key) || rejected_.contains(key)) {
            return false;
        }
        const auto entry = index_.constFind(key);
        if (entry == index_.constEnd()) {
            return false;
        }
        if (!file_.seek(entry.value().offset)) {
            rejected_.insert(key);
            return false;
        }
        const QByteArray compressed = file_.read(entry.value().length);
        if (compressed.size() < 5 || bigEndian(compressed, 0) > MaximumSectorBytes) {
            rejected_.insert(key);
            return false;
        }
        const QByteArray raw = qUncompress(compressed);
        auto* tile = new BuildingSector;
        int cost = 0;
        if (!decode(raw, *tile, cost, compact_)) {
            delete tile;
            rejected_.insert(key);
            return false;
        }
        return sectors_.insert(key, tile, cost);
    }

private:
    struct Entry {
        quint32 offset;
        quint32 length;
    };

    bool readIndex() {
        QDataStream input(&file_);
        quint32 magic;
        quint8 zoom;
        quint16 count;
        input >> magic >> zoom >> count;
        compact_ = magic == 0x4f4d4832u;
        if (input.status() != QDataStream::Ok || (!compact_ && magic != 0x4f4d4831u)
            || zoom != BuildingSectorZoom || count > 8192) {
            return false;
        }
        const qint64 fileSize = file_.size();
        quint32 previousEnd = 7 + count * 12;
        index_.reserve(count);
        for (int i = 0; i < count; ++i) {
            quint16 x, y;
            Entry entry;
            input >> x >> y >> entry.offset >> entry.length;
            if (input.status() != QDataStream::Ok || x >= BuildingSectorsPerAxis || y >= BuildingSectorsPerAxis
                || entry.offset < previousEnd || entry.length < 5 || entry.length > MaximumSectorBytes
                || static_cast<qint64>(entry.offset) + entry.length > fileSize) {
                return false;
            }
            const quint32 key = sectorKey(x, y);
            if (index_.contains(key)) {
                return false;
            }
            index_.insert(key, entry);
            previousEnd = entry.offset + entry.length;
        }
        return input.status() == QDataStream::Ok;
    }

    static bool readVarUInt(QDataStream& input, quint32& value) {
        value = 0;
        for (int shift = 0; shift <= 14; shift += 7) {
            quint8 byte;
            input >> byte;
            if (input.status() != QDataStream::Ok) {
                return false;
            }
            value |= static_cast<quint32>(byte & 127) << shift;
            if (!(byte & 128)) {
                return true;
            }
        }
        return false;
    }

    static bool readDelta(QDataStream& input, int& coordinate) {
        quint32 encoded;
        if (!readVarUInt(input, encoded) || encoded > 131070) {
            return false;
        }
        const int magnitude = static_cast<int>(encoded >> 1);
        coordinate += (encoded & 1) ? -magnitude - 1 : magnitude;
        return coordinate >= -32768 && coordinate <= 32767;
    }

    static bool decode(const QByteArray& raw, BuildingSector& sector, int& cost, bool compact) {
        QDataStream input(raw);
        quint16 shapeCount, numberCount;
        input >> shapeCount >> numberCount;
        if (input.status() != QDataStream::Ok || shapeCount > 4096 || numberCount > 4096) {
            return false;
        }
        cost = sizeof(BuildingSector) + shapeCount * sizeof(BuildingShape) + numberCount * sizeof(HouseNumber);
        sector.shapes.reserve(shapeCount);
        sector.numbers.reserve(numberCount);
        int anchorX = 0;
        int anchorY = 0;
        for (int i = 0; i < shapeCount; ++i) {
            quint8 ringCount;
            input >> ringCount;
            if (input.status() != QDataStream::Ok || ringCount < 1 || ringCount > 24) {
                return false;
            }
            BuildingShape shape;
            shape.rings.reserve(ringCount);
            cost += ringCount * (sizeof(QPolygon) + 32);
            for (int r = 0; r < ringCount; ++r) {
                quint32 size;
                if (compact) {
                    if (!readVarUInt(input, size)) {
                        return false;
                    }
                } else {
                    quint16 legacySize;
                    input >> legacySize;
                    size = legacySize;
                }
                if (input.status() != QDataStream::Ok || size < 3 || size > 512) {
                    return false;
                }
                QPolygon polygon;
                polygon.reserve(size);
                if (compact) {
                    int x = anchorX;
                    int y = anchorY;
                    for (quint32 j = 0; j < size; ++j) {
                        if (!readDelta(input, x) || !readDelta(input, y)) {
                            return false;
                        }
                        polygon.append(QPoint(x, y));
                    }
                    anchorX = polygon.first().x();
                    anchorY = polygon.first().y();
                } else {
                    for (quint32 j = 0; j < size; ++j) {
                        qint16 x, y;
                        input >> x >> y;
                        polygon.append(QPoint(x, y));
                    }
                    if (input.status() != QDataStream::Ok) {
                        return false;
                    }
                }
                cost += size * sizeof(QPoint);
                shape.rings.append(polygon);
            }
            shape.bounds = shape.rings.first().boundingRect();
            sector.shapes.append(shape);
        }
        for (int i = 0; i < numberCount; ++i) {
            qint16 x, y;
            quint8 length;
            input >> x >> y >> length;
            if (x < 0 || x >= SectorSubdivisions || y < 0 || y >= SectorSubdivisions || length < 1 || length > 48) {
                return false;
            }
            char text[48];
            if (input.readRawData(text, length) != length) {
                return false;
            }
            HouseNumber mark = {QPoint(x, y), QString::fromUtf8(text, length)};
            cost += 24 + mark.value.size() * sizeof(QChar);
            sector.numbers.append(mark);
        }
        return input.status() == QDataStream::Ok && input.atEnd();
    }

    QFile file_;
    QHash<quint32, Entry> index_;
    QCache<quint32, BuildingSector> sectors_;
    QSet<quint32> rejected_;
    bool tried_ = false;
    bool compact_ = false;
};

}  // namespace

struct NodeMap::State {
    explicit State(NodeMap* owner)
        : owner(owner), memory(16 * TileSide * TileSide * 4), center(project(EmptyY, EmptyX)) {
        settle.setSingleShot(true);
        deadline.setSingleShot(true);
        buildingTimer.setSingleShot(true);
    }

    NodeMap* owner;
    QNetworkAccessManager* network = nullptr;
    QNetworkDiskCache* disk = nullptr;
    std::unique_ptr<LocalTileProxy> localProxy;
    std::function<void(const char*, int)> trace;
    QCache<QString, CachedImage> memory;
    QTimer settle;
    QTimer deadline;
    QTimer buildingTimer;
    QTimer camera;
    QElapsedTimer cameraClock;
    QPointF cameraFrom;
    QPointF cameraTo;
    double cameraScale = 0.0;
    double cameraFromScale = 0.0;
    int cameraZoom = 8;
    BuildingLayer buildings;
    QVector<quint32> buildingView;
    QVector<quint32> buildingQueue;
    QNetworkReply* reply = nullptr;
    QUrl replyCacheUrl;
    QString replyKey;
    QByteArray received;
    QString requestError;
    QSet<QString> attempted;
    QSet<QString> visibleKeys;
    Source source = Offline;
    QString issue;
    bool worldTried = false;
    bool firstTile = true;
    OfflineRegion region = OmskCity;
    OfflineRegion detailRegion = World;
    Atlas world;
    Atlas detail;
    QSet<int> missingAtlases;
    QVector<Pin> pins;
    QVector<QPair<QRect, QString>> pinLabels;
    Pin self;
    QString focused;
    int nodeCount = 0;
    QPointF center;
    int zoom = 8;
    enum View { Automatic, Markers, SavedRegion, Manual };
    View framing = Automatic;
    bool pressing = false;
    bool dragging = false;
    QPoint first;
    QPoint last;
    MapButton* buttons[MapControlCount] = {};

    void mark(const char* step, int value = Client::TraceWithoutValue) const {
        if (trace) {
            trace(step, value);
        }
    }

    QRect mapRect() const { return owner->rect(); }

    int controlSize() const {
        return qBound(18, (mapRect().height() - 15 - (MapControlCount - 1) * 4) / MapControlCount, 36);
    }

    QRect control(int index) const {
        const int size = controlSize();
        return QRect(mapRect().right() - size - 6, mapRect().top() + 7 + index * (size + 4), size, size);
    }

    void placeControls() {
        for (int i = 0; i < MapControlCount; ++i)
            buttons[i]->setGeometry(control(i));
    }

    QRect openMapRect() const {
        QRect field = mapRect();
        field.setRight(qMax(field.left(), control(0).left() - 5));
        return field;
    }

    QRect hintRect() const {
        const QRect field = openMapRect();
        return QRect(field.left() + 9, qMax(field.top() + 28, field.bottom() - 70),
                     qMax(1, field.width() - 18), 35);
    }

    static double zoomScale(int level) { return static_cast<double>(TileSide << level); }
    double scale() const { return cameraScale > 0.0 ? cameraScale : zoomScale(zoom); }
    QPoint mapAnchor() const { return openMapRect().center(); }

    QPointF screen(const QPointF& world) const {
        const QPoint midpoint = mapAnchor();
        return QPointF(midpoint.x() + (nearX(world.x(), center.x()) - center.x()) * scale(),
                       midpoint.y() + (world.y() - center.y()) * scale());
    }

    QPointF areaTopLeft(const QRect& area, double offset) const {
        const QPoint middle = mapAnchor();
        return QPointF(center.x() + (area.left() - middle.x()) / scale() - offset,
                       center.y() + (area.top() - middle.y()) / scale());
    }

    QPointF areaBottomRight(const QRect& area, double offset) const {
        const QPoint middle = mapAnchor();
        return QPointF(center.x() + (area.right() - middle.x()) / scale() - offset,
                       center.y() + (area.bottom() - middle.y()) / scale());
    }

    bool canSee(OfflineRegion selected) const {
        const QRectF bounds = regionBounds(selected);
        const double offset = nearX(bounds.center().x(), center.x()) - bounds.center().x();
        QRect map = mapRect();
        if (selected == detailRegion) {
            const int margin = qMin(map.width(), map.height()) / 3;
            map.adjust(-margin, -margin, margin, margin);
        }
        return intersects(QRectF(areaTopLeft(map, offset), areaBottomRight(map, offset)), bounds);
    }

    bool loadAtlas(OfflineRegion selected, Atlas& output) {
        if (missingAtlases.contains(selected)) {
            return false;
        }
        const QString resource = QLatin1String(":/maps/") + QString::fromLatin1(regionFile(selected));
        QFile embedded(resource);
        if (embedded.open(QIODevice::ReadOnly) && embedded.size() <= 256 * 1024 && readAtlas(embedded, output)) {
            mark(selected == OmskCity ? "map: embedded atlas selected" : "map: embedded region loaded", selected);
        } else {
            const QStringList paths = atlasPaths(selected);
            for (int i = 0; i < paths.size(); ++i) {
                QFile file(paths.at(i));
                if (file.open(QIODevice::ReadOnly) && file.size() <= 256 * 1024 && readAtlas(file, output)) {
                    mark("map: atlas file selected", selected);
                    break;
                }
            }
        }
        if (output.bounds.isEmpty()) {
            missingAtlases.insert(selected);
            mark("map: offline region unavailable", selected);
            return false;
        }
        mark("map: loaded atlas lines", output.lines.size());
        return true;
    }

    void prepareAtlases() {
        if (!owner->isVisible()) {
            return;
        }
        if (!worldTried) {
            worldTried = true;
            loadAtlas(World, world);
        }
        OfflineRegion wanted = World;
        if (zoom >= 8 && canSee(OmskCity) && !missingAtlases.contains(OmskCity)) {
            wanted = OmskCity;
        } else if (zoom >= 5 && canSee(OmskOblast) && !missingAtlases.contains(OmskOblast)) {
            wanted = OmskOblast;
        } else if (zoom >= 1 && canSee(Siberia) && !missingAtlases.contains(Siberia)) {
            wanted = Siberia;
        }
        if (wanted != detailRegion) {
            detail = Atlas();
            detailRegion = World;
            if (wanted != World && loadAtlas(wanted, detail)) {
                detailRegion = wanted;
            }
        }
    }

    int minimumZoom() const { return source == Offline ? 0 : MinimumOnlineZoom; }
    int maximumZoom() const { return source == Offline ? MaximumOfflineZoom : MaximumOnlineZoom; }

    QPair<QPointF, int> fittedView() const {
        QRectF regionBox = regionBounds(region);
        bool frameRegion = framing == SavedRegion;
        if (!frameRegion && framing == Automatic && source == Offline) {
            frameRegion = region != OmskCity;
            for (int i = 0; i < pins.size() && !frameRegion; ++i) {
                if (!regionBox.contains(pins.at(i).position)) {
                    frameRegion = true;
                }
            }
        }
        if (!frameRegion && !pins.isEmpty()) {
            const QPointF first = pins.first().position;
            double left = first.x(), right = first.x(), top = first.y(), bottom = first.y();
            for (int i = 1; i < pins.size(); ++i) {
                const QPointF point = pins.at(i).position;
                const double x = nearX(point.x(), first.x());
                left = qMin(left, x);
                right = qMax(right, x);
                top = qMin(top, static_cast<double>(point.y()));
                bottom = qMax(bottom, static_cast<double>(point.y()));
            }
            regionBox = QRectF(QPointF(left, top), QPointF(right, bottom));
        }
        QPointF target = regionBox.center();
        target.setX(wrapped(target.x()));
        const QRect map = openMapRect();
        const double minimumSpan = 0.000095;
        const double limit = qMin(qMax(1, map.width() - 36) / std::max(minimumSpan, static_cast<double>(regionBox.width())),
                                  qMax(1, map.height() - 48) / std::max(minimumSpan, static_cast<double>(regionBox.height())));
        int level = minimumZoom();
        while (level < maximumZoom() && zoomScale(level + 1) <= limit) {
            ++level;
        }
        return qMakePair(target, level);
    }

    void fit() {
        stopCamera();
        const QPair<QPointF, int> view = fittedView();
        center = view.first;
        zoom = view.second;
    }

    QVector<TilePlacement> tiles() const {
        const QRect map = mapRect();
        QVector<TilePlacement> result;
        if (map.width() <= 0) {
            return result;
        }
        const QPoint midpoint = mapAnchor();
        const double tileScale = zoomScale(zoom);
        const double ratio = scale() / tileScale;
        const double globalX = std::floor(center.x() * tileScale - (midpoint.x() - map.left()) / ratio);
        const double globalY = std::floor(center.y() * tileScale - (midpoint.y() - map.top()) / ratio);
        const int mask = (1 << zoom) - 1;
        int left = static_cast<int>(std::floor(globalX / 256.0));
        int right = static_cast<int>(std::floor((globalX + (map.width() - 1) / ratio) / TileSide));
        int top = qMax(0, static_cast<int>(std::floor(globalY / TileSide)));
        int bottom = qMin(mask, static_cast<int>(std::floor((globalY + (map.height() - 1) / ratio) / TileSide)));
        // The 640px phone screen needs at most four tiles along either axis.
        // At a fractional zoom, old cached tiles are only used while animating.
        const int limit = camera.isActive() ? 6 : 4;
        if (right - left >= limit) {
            left = (left + right) / 2 - 1;
            right = left + limit - 1;
        }
        if (bottom - top >= limit) {
            top = (top + bottom) / 2 - 1;
            bottom = qMin(mask, top + limit - 1);
        }
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                const int pixelX = qRound((x * TileSide - globalX) * ratio);
                const int pixelY = qRound((y * TileSide - globalY) * ratio);
                const int tileWidth = qRound(((x + 1) * TileSide - globalX) * ratio) - pixelX;
                const int tileHeight = qRound(((y + 1) * TileSide - globalY) * ratio) - pixelY;
                TilePlacement tile = {zoom, x & mask, y, QRect(map.left() + pixelX, map.top() + pixelY,
                                                             tileWidth, tileHeight)};
                if (tile.rect.intersects(map)) {
                    result.append(tile);
                }
            }
        }
        return result;
    }

    bool fullyDrawn(const QVector<TilePlacement>& tiles) const {
        if (tiles.isEmpty() || source == Offline) {
            return false;
        }
        for (int i = 0; i < tiles.size(); ++i) {
            if (!memory.contains(tiles.at(i).path())) {
                return false;
            }
        }
        return true;
    }

    QString status(bool complete) const {
        if (!issue.isEmpty()) {
            return issue;
        }
        if (source == Offline) {
            if (!buildingView.isEmpty() && zoom < HouseNumberZoom) {
                return ru("Приблизьте для номеров домов");
            }
            return worldTried && world.bounds.isEmpty() && detail.bounds.isEmpty()
                ? ru("Офлайн: подложка недоступна")
                : ru("Офлайн · узлы %1/%2").arg(pins.size()).arg(nodeCount);
        }
        return complete ? ru("OSM · localhost · готово") : ru("OSM · localhost · загрузка");
    }

    void schedule() {
        if (source != Loopback || !owner->isVisible() || camera.isActive()) {
            return;
        }
        QSet<QString> nextVisible;
        const QVector<TilePlacement> placements = tiles();
        for (int i = 0; i < placements.size(); ++i) {
            nextVisible.insert(placements.at(i).path());
        }
        if (nextVisible != visibleKeys) {
            attempted.intersect(nextVisible);
            const QList<QString> keys = memory.keys();
            const QDateTime now = QDateTime::currentDateTimeUtc();
            for (int i = 0; i < keys.size(); ++i) {
                const QString& key = keys.at(i);
                const CachedImage* tile = memory.object(key);
                if (!nextVisible.contains(key) && (tile->transient || tile->expires <= now)) {
                    memory.remove(key);
                }
            }
            visibleKeys = nextVisible;
        }
        settle.start(200);
    }

    void cancel() {
        settle.stop();
        deadline.stop();
        if (reply) {
            QNetworkReply* old = reply;
            reply = nullptr;
            old->disconnect(owner);
            // Destroy the old reply outside the input/network callback stack.
            old->deleteLater();
        }
        received.clear();
        requestError.clear();
    }

    void clearIssue() {
        issue.clear();
        attempted.clear();
    }

    void queueBuildings() {
        QVector<quint32> wanted;
        const QRect map = mapRect();
        const QRectF world(areaTopLeft(map, 0), areaBottomRight(map, 0));
        if (source == Offline && owner->isVisible() && zoom >= BuildingZoom
            && intersects(world, regionBounds(OmskCity))) {
            const QRectF area = world.intersected(regionBounds(OmskCity));
            int left = static_cast<int>(std::floor(area.left() * BuildingSectorsPerAxis));
            int right = static_cast<int>(std::floor(area.right() * BuildingSectorsPerAxis));
            int top = static_cast<int>(std::floor(area.top() * BuildingSectorsPerAxis));
            int bottom = static_cast<int>(std::floor(area.bottom() * BuildingSectorsPerAxis));
            // Nokia's viewport needs at most six sectors on either axis at z14.
            if (right - left >= 6) {
                left = (left + right) / 2 - 2;
                right = left + 5;
            }
            if (bottom - top >= 6) {
                top = (top + bottom) / 2 - 2;
                bottom = top + 5;
            }
            for (int y = top; y <= bottom; ++y) {
                for (int x = left; x <= right; ++x) {
                    wanted.append(sectorKey(x, y));
                }
            }
            const QPoint middle = (center * BuildingSectorsPerAxis).toPoint();
            std::sort(wanted.begin(), wanted.end(), [middle](quint32 a, quint32 b) {
                return (sectorPosition(a) - middle).manhattanLength() < (sectorPosition(b) - middle).manhattanLength();
            });
        }
        if (wanted == buildingView) {
            if (!buildingQueue.isEmpty() && !buildingTimer.isActive())
                buildingTimer.start(85);
            return;
        }
        buildingView = wanted;
        buildingQueue = wanted;
        buildingTimer.stop();
        if (!buildingQueue.isEmpty()) {
            buildingTimer.start(85);
        }
    }

    QPointF buildingOrigin(quint32 key) const {
        const QPoint position = sectorPosition(key);
        return screen(QPointF(position.x() / static_cast<double>(BuildingSectorsPerAxis),
                              position.y() / static_cast<double>(BuildingSectorsPerAxis)));
    }

    bool drawBuildings(QPainter& painter, const QRect& map) const {
        if (buildingView.isEmpty()) {
            return false;
        }
        const double edge = scale() / BuildingSectorsPerAxis;
        const double pixel = edge / SectorSubdivisions;
        QPen outline(color(BuildingOutline));
        outline.setCosmetic(true);
        bool drawn = false;
        for (int i = 0; i < buildingView.size(); ++i) {
            const quint32 key = buildingView.at(i);
            const BuildingSector* tile = buildings.cached(key);
            if (!tile || tile->shapes.isEmpty()) {
                continue;
            }
            const QPointF origin = buildingOrigin(key);
            const QRectF tileRect(origin, QSizeF(edge, edge));
            const QRectF onScreen = tileRect.intersected(QRectF(map));
            if (onScreen.isEmpty()) {
                continue;
            }
            const QRectF local((onScreen.topLeft() - origin) / pixel, onScreen.size() / pixel);
            painter.save();
            painter.setClipRect(tileRect, Qt::IntersectClip);
            painter.translate(origin);
            painter.scale(pixel, pixel);
            painter.setPen(outline);
            painter.setBrush(color(BuildingFill));
            for (int j = 0; j < tile->shapes.size(); ++j) {
                const BuildingShape& shape = tile->shapes.at(j);
                if (!intersects(local, QRectF(shape.bounds))) {
                    continue;
                }
                if (shape.rings.size() == 1) {
                    painter.drawPolygon(shape.rings.first());
                } else {
                    QPainterPath path;
                    for (int r = 0; r < shape.rings.size(); ++r) {
                        path.addPolygon(QPolygonF(shape.rings.at(r)));
                        path.closeSubpath();
                    }
                    painter.drawPath(path);
                }
                drawn = true;
            }
            painter.restore();
        }
        if (zoom >= HouseNumberZoom) {
            drawn = drawHouseNumbers(painter, map, pixel) || drawn;
        }
        return drawn;
    }

    bool drawHouseNumbers(QPainter& painter, const QRect& map, double pixel) const {
        painter.save();
        QFont font = painter.font();
        font.setPixelSize(11);
        font.setBold(false);
        painter.setFont(font);
        const QFontMetrics metrics(font);
        QVector<QRect> used;
        for (int i = 0; i < buildingView.size() && used.size() < 96; ++i) {
            const quint32 key = buildingView.at(i);
            const BuildingSector* tile = buildings.cached(key);
            if (!tile) {
                continue;
            }
            const QPointF origin = buildingOrigin(key);
            for (int j = 0; j < tile->numbers.size() && used.size() < 96; ++j) {
                const HouseNumber& number = tile->numbers.at(j);
                const QPoint anchor = (origin + QPointF(number.anchor) * pixel).toPoint();
                if (!map.contains(anchor)) {
                    continue;
                }
                const int width = metrics.width(number.value) + 4;
                const int height = metrics.height() + 1;
                const QRect label(anchor.x() - width / 2, anchor.y() - height / 2, width, height);
                bool available = map.contains(label);
                for (int k = 0; k < used.size() && available; ++k) {
                    available = !used.at(k).intersects(label.adjusted(-1, -1, 1, 1));
                }
                if (available) {
                    used.append(label);
                    painter.fillRect(label, color(0x263941));
                    painter.setPen(color(HouseNumberText));
                    painter.drawText(label, Qt::AlignCenter, number.value);
                }
            }
        }
        painter.restore();
        return !used.isEmpty();
    }

    void updateView() {
        prepareAtlases();
        queueBuildings();
        owner->update();
        schedule();
    }

    void stopCamera() {
        camera.stop();
        cameraScale = 0.0;
    }

    void setCamera(const QPointF& target, int level) {
        stopCamera();
        center = QPointF(wrapped(target.x()), target.y());
        zoom = level;
        updateView();
    }

    void moveCamera(QPointF target, int level) {
        level = qBound(minimumZoom(), level, maximumZoom());
        target.setX(nearX(target.x(), center.x()));
        const double currentScale = scale();
        const double targetScale = zoomScale(level);
        const double distance = (target - center).manhattanLength() * qMin(currentScale, targetScale);
        if (!owner->isVisible() || qAbs(level - zoom) > 1 ||
            distance > 2 * qMax(owner->width(), owner->height()) ||
            (distance < 1.0 && currentScale == targetScale)) {
            setCamera(target, level);
            return;
        }
        cameraFrom = center;
        cameraTo = target;
        cameraScale = cameraFromScale = currentScale;
        cameraZoom = level;
        settle.stop();
        buildingTimer.stop();
        cameraClock.start();
        camera.start(CameraFrame);
    }

    void cameraFrame() {
        const double progress = cameraClock.elapsed() / static_cast<double>(CameraDuration);
        if (progress >= 1.0) {
            setCamera(cameraTo, cameraZoom);
            return;
        }
        const double remaining = 1.0 - progress;
        const double eased = 1.0 - remaining * remaining * remaining;
        center = cameraFrom + (cameraTo - cameraFrom) * eased;
        center.setX(wrapped(center.x()));
        cameraScale = cameraFromScale + (zoomScale(cameraZoom) - cameraFromScale) * eased;
        owner->update();
    }

    void moveToFit() {
        const QPair<QPointF, int> view = fittedView();
        moveCamera(view.first, view.second);
    }

    void adjustZoom(int delta) {
        framing = Manual;
        moveCamera(center, (camera.isActive() ? cameraZoom : zoom) + delta);
    }

    void pan(const QPoint& pixels) {
        stopCamera();
        center.setX(wrapped(center.x() - pixels.x() / scale()));
        center.setY(qBound(0.0, center.y() - pixels.y() / scale(), 1.0));
        framing = Manual;
        updateView();
    }

    void overview() {
        if (!issue.isEmpty()) {
            clearIssue();
            updateView();
        } else {
            framing = Markers;
            moveToFit();
        }
    }

    void showRegion() {
        framing = SavedRegion;
        moveToFit();
    }

    void centerOnRadio() {
        if (self.id.isEmpty()) {
            showRegion();
            return;
        }
        framing = Manual;
        moveCamera(self.position, camera.isActive() ? cameraZoom : zoom);
    }

    bool prepareLoopback() {
        if (!localProxy)
            localProxy = std::make_unique<LocalTileProxy>();
        if (!localProxy->running()) {
            QString error;
            if (!localProxy->start(true, &error)) {
                issue = error.isEmpty() ? ru("Не удалось открыть localhost") : error;
                return false;
            }
            mark("map: localhost relay ready");
        }
        return true;
    }

    void stopLoopback() {
        if (source != Loopback || !localProxy)
            return;
        localProxy->stop();
    }

    void prepareNetwork() {
        if (!network) {
            mark("map: create network manager");
            network = new QNetworkAccessManager(owner);
            // Map HTTP requests go to the proxy inside this phone.
            network->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
            mark("map: setup tile cache");
            disk = new QNetworkDiskCache(owner);
            disk->setCacheDirectory(cachePath());
            disk->setMaximumCacheSize(3 * 1024 * 1024);
            mark("map: network and cache ready");
        }
    }

    QByteArray stored(const QUrl& url) {
        QIODevice* input = disk->data(url);
        if (!input) {
            return QByteArray();
        }
        const QByteArray data = input->read(MaximumTileBytes + 1);
        delete input;
        return data;
    }

    void save(const QUrl& url, const QByteArray& data, const QDateTime& expires,
              const QNetworkCacheMetaData::RawHeaderList& headers) {
        QNetworkCacheMetaData metadata;
        metadata.setUrl(url);
        metadata.setRawHeaders(headers);
        metadata.setExpirationDate(expires);
        metadata.setSaveToDisk(true);
        QIODevice* entry = disk->prepare(metadata);
        if (entry) {
            if (entry->write(data) == data.size()) {
                disk->insert(entry);
            } else {
                disk->remove(url);
            }
        }
    }

    void hold(const QString& key, const QImage& image, const QDateTime& expires, bool transient) {
        if (!transient || visibleKeys.contains(key)) {
            CachedImage* tile = new CachedImage{image, expires, transient};
            memory.insert(key, tile, TileSide * TileSide * 4);
        }
    }

    void grid(QPainter& painter, const QRect& map) const {
        painter.setPen(color(0x26343b));
        const QPoint midpoint = mapAnchor();
        const int left = static_cast<int>(std::fmod(midpoint.x() - center.x() * scale() - map.left(), 44));
        const int top = static_cast<int>(std::fmod(midpoint.y() - center.y() * scale() - map.top(), 44));
        for (int x = map.left() + left; x <= map.right(); x += 44) {
            painter.drawLine(x, map.top(), x, map.bottom());
        }
        for (int y = map.top() + top; y <= map.bottom(); y += 44) {
            painter.drawLine(map.left(), y, map.right(), y);
        }
    }

    bool drawAtlas(QPainter& painter, const QRect& map, const Atlas& atlas, OfflineRegion kind,
                   bool drawPlaceNames) const {
        if (atlas.bounds.isEmpty()) {
            return false;
        }
        const double nearest = nearX(atlas.bounds.center().x(), center.x()) - atlas.bounds.center().x();
        const QPoint midpoint = mapAnchor();
        const int copies = kind == World ? 1 : 0;
        bool drawn = false;
        for (int copy = -copies; copy <= copies; ++copy) {
            const double offset = nearest + copy;
            const QRectF visible(areaTopLeft(map, offset), areaBottomRight(map, offset));
            if (!intersects(visible, atlas.bounds)) {
                continue;
            }
            drawn = true;
            painter.save();
            painter.translate(midpoint.x(), midpoint.y());
            painter.scale(scale(), scale());
            painter.translate(offset - center.x(), -center.y());
            if (kind == World || kind == OmskCity || kind == OmskOblast || world.bounds.isEmpty()) {
                painter.fillRect(atlas.bounds, color(AtlasBackground));
            }
            for (int i = 0; i < atlas.lines.size(); ++i) {
                const AtlasLine& line = atlas.lines.at(i);
                const bool tooCoarse = (kind == OmskCity && line.kind < 6 && zoom < RoadLevels[line.kind])
                    || (kind == OmskOblast && line.kind == 3 && zoom < 7)
                    || (kind == World && detailRegion == OmskCity && line.kind == 1);
                if (tooCoarse || !intersects(visible, line.bounds)) {
                    continue;
                }
                QPen pen(color(AtlasColors[line.kind]));
                pen.setWidth((line.kind == 1 && kind == OmskCity)
                    || (line.kind == 5 && zoom >= 12 && kind == OmskCity) ? 2 : 1);
                pen.setCosmetic(true);
                painter.setPen(pen);
                painter.drawPolyline(line.points.constData(), line.points.size());
            }
            painter.restore();
        }
        if (drawn && drawPlaceNames) {
            drawNames(painter, map, atlas, kind);
        }
        return drawn;
    }

    void drawNames(QPainter& painter, const QRect& map, const Atlas& atlas, OfflineRegion layer) const {
        static const int levels[4][3] = {{13, 10, 8}, {8, 6, 5}, {5, 3, 1}, {4, 2, 0}};
        QVector<QRect> labels;
        for (int i = 0; i < atlas.places.size() && labels.size() < 12; ++i) {
            const AtlasPlace& place = atlas.places.at(i);
            if (zoom < levels[layer][place.kind]) {
                continue;
            }
            const QPoint position = screen(place.position).toPoint();
            if (!map.contains(position)) {
                continue;
            }
            QFont font = painter.font();
            font.setBold(place.kind == 2);
            painter.setFont(font);
            const QFontMetrics metrics(font);
            const QString name = metrics.elidedText(place.name, Qt::ElideRight, qMax(8, map.width() - 24));
            const int textWidth = metrics.width(name) + 4;
            const QRect label(qBound(map.left() + 2, position.x() - textWidth / 2,
                qMax(map.left() + 2, map.right() - textWidth - 3)),
                position.y() - metrics.height() - 6, textWidth + 4, metrics.height() + 2);
            bool crowded = false;
            for (int j = 0; j < labels.size(); ++j) {
                if (labels.at(j).intersects(label.adjusted(-8, -4, 8, 4))) {
                    crowded = true;
                    break;
                }
            }
            if (crowded || !map.contains(label)) {
                continue;
            }
            labels.append(label);
            painter.fillRect(label, color(AtlasBackground));
            painter.setPen(color(Paper));
            painter.drawText(label, Qt::AlignCenter, name);
            painter.fillRect(QRect(position.x(), position.y(), 2, 2), color(Muted));
        }
    }

    bool drawTiles(QPainter& painter, const QVector<TilePlacement>& placements) const {
        if (source == Offline) {
            return false;
        }
        bool drawn = false;
        for (int i = 0; i < placements.size(); ++i) {
            const TilePlacement& tile = placements.at(i);
            const CachedImage* image = memory.object(tile.path());
            if (image) {
                if (tile.rect.size() == image->image.size())
                    painter.drawImage(tile.rect.topLeft(), image->image);
                else
                    painter.drawImage(tile.rect, image->image);
                drawn = true;
            }
        }
        return drawn;
    }

    void drawPin(QPainter& painter, const Pin& pin, const QPoint& position) const {
        if (pin.id == focused || pin.id == self.id) {
            painter.setPen(QPen(color(pin.id == self.id ? Accent : Paper), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(position, 11, 11);
        }
        painter.setPen(QPen(color(Background), 2));
        if (pin.role == 2) {
            painter.setBrush(color(Repeater));
            const QPoint diamond[] = {position + QPoint(0, -7), position + QPoint(7, 0),
                position + QPoint(0, 7), position + QPoint(-7, 0)};
            painter.drawPolygon(diamond, 4);
        } else if (pin.role == 3) {
            painter.setBrush(color(Room));
            painter.drawRoundedRect(QRect(position.x() - 5, position.y() - 5, 11, 11), 3, 3);
        } else {
            painter.setBrush(color(pin.role == 4 ? Sensor : Accent));
            painter.drawEllipse(position, 6, 6);
        }
    }

    void drawPins(QPainter& painter, const QRect& viewport) {
        struct VisiblePin { const Pin* pin; QPoint position; };
        QVector<VisiblePin> visible;
        visible.reserve(pins.size());
        pinLabels.clear();
        for (int i = 0; i < pins.size(); ++i) {
            const Pin& pin = pins.at(i);
            const QPoint position = screen(pin.position).toPoint();
            if (!viewport.contains(position))
                continue;
            const VisiblePin item = {&pin, position};
            if (pin.id == focused || pin.id == self.id)
                visible.prepend(item);
            else
                visible.append(item);
        }

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        for (int i = visible.size() - 1; i >= 0; --i) {
            const VisiblePin& item = visible.at(i);
            drawPin(painter, *item.pin, item.position);
        }

        QFont font = painter.font();
        font.setPixelSize(12);
        font.setBold(false);
        painter.setFont(font);
        const QFontMetrics metrics(font);
        const QRect labelArea = openMapRect().adjusted(6, 36, -4, -35);
        const int limit = qBound(5, viewport.width() * viewport.height() / 8200, 16);
        const int candidatesLimit = qMin(visible.size(), 72);
        for (int i = 0; i < candidatesLimit && pinLabels.size() < limit; ++i) {
            const Pin& pin = *visible.at(i).pin;
            const QPoint& position = visible.at(i).position;
            if (pin.name.isEmpty())
                continue;
            const QString name = metrics.elidedText(pin.name, Qt::ElideRight, 106);
            const int w = metrics.width(name) + 8;
            const int h = metrics.height() + 4;
            const QRect candidates[] = {
                QRect(position.x() + 11, position.y() - h / 2, w, h),
                QRect(position.x() - w - 10, position.y() - h / 2, w, h),
                QRect(position.x() - w / 2, position.y() - h - 10, w, h),
                QRect(position.x() - w / 2, position.y() + 11, w, h)
            };
            for (const QRect& label : candidates) {
                if (!labelArea.contains(label))
                    continue;
                const QRect padding = label.adjusted(-4, -2, 4, 2);
                bool crowded = false;
                for (int j = 0; j < pinLabels.size() && !crowded; ++j)
                    crowded = pinLabels.at(j).first.intersects(padding);
                for (int j = 0; j < visible.size() && !crowded; ++j) {
                    const QPoint& other = visible.at(j).position;
                    crowded = j != i && padding.intersects(QRect(other.x() - 6, other.y() - 6, 13, 13));
                }
                if (crowded)
                    continue;
                pinLabels.append(qMakePair(label, pin.id));
                painter.setPen(Qt::NoPen);
                painter.setBrush(color(Background));
                painter.drawRoundedRect(label, 4, 4);
                painter.setPen(color(pin.id == focused || pin.id == self.id ? Accent : Paper));
                painter.drawText(label, Qt::AlignCenter, name);
                break;
            }
        }
        painter.restore();
    }
};

NodeMap::NodeMap(QWidget* parent) : QWidget(parent), state_(new State(this)) {
    setMinimumSize(190, 156);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    auto* controls = new QButtonGroup(this);
    controls->setExclusive(false);
    const char* const names[] = {"mapLayers", "mapFit", "mapCenter", "mapExpand", "mapZoomIn", "mapZoomOut"};
    const char* const captions[] = {"Подложки карты", "Все узлы", "Начальная область", "Развернуть карту",
                                    "Приблизить", "Отдалить"};
    for (int i = 0; i < MapControlCount; ++i) {
        MapButton* button = new MapButton(i, names[i], captions[i], this);
        button->setFocusPolicy(Qt::StrongFocus);
        state_->buttons[i] = button;
        controls->addButton(button, i);
    }
    state_->buttons[Expand]->setCheckable(true);
    state_->placeControls();
    connect(controls, SIGNAL(buttonClicked(int)), this, SLOT(activateControl(int)), Qt::QueuedConnection);
    connect(&state_->camera, SIGNAL(timeout()), this, SLOT(advanceCamera()));
    connect(&state_->settle, SIGNAL(timeout()), this, SLOT(loadVisibleTiles()));
    connect(&state_->deadline, SIGNAL(timeout()), this, SLOT(tileTimedOut()));
    connect(&state_->buildingTimer, SIGNAL(timeout()), this, SLOT(loadVisibleBuildings()));
}

NodeMap::~NodeMap() {
    state_->cancel();
    delete state_;
}

void NodeMap::setTrace(const std::function<void(const char*, int)>& trace) {
    state_->trace = trace;
}

NodeMap::Source NodeMap::sourceFromPreference(int value) {
    // Values 1 and 2 were direct HTTPS and a computer relay, respectively.
    return value == 1 || value == 2 || value == Loopback ? Loopback : Offline;
}

void NodeMap::setSource(Source source) {
    State& s = *state_;
    if (s.source == source) {
        return;
    }
    s.mark("map: apply source", static_cast<int>(source));
    s.stopCamera();
    s.cancel();
    s.stopLoopback();
    s.firstTile = true;
    s.memory.clear();
    s.visibleKeys.clear();
    s.clearIssue();
    s.source = source;
    if (s.framing != State::Manual) {
        s.fit();
    } else {
        s.zoom = qBound(s.minimumZoom(), s.zoom, s.maximumZoom());
    }
    s.updateView();
}

void NodeMap::setOfflineRegion(OfflineRegion region) {
    State& s = *state_;
    if (region != s.region) {
        s.region = region;
        s.showRegion();
    }
}

void NodeMap::setNodes(const QVector<NetworkNode>& nodes) {
    State& s = *state_;
    QVector<Pin> pins;
    pins.reserve(nodes.size());
    for (int i = 0; i < nodes.size(); ++i) {
        const NetworkNode& node = nodes.at(i);
        if (!nodePosition(node)) {
            continue;
        }
        const Pin pin = {node.id, node.title.simplified(), node.role, project(node.latitude, node.longitude)};
        pins.append(pin);
    }
    if (pins == s.pins && nodes.size() == s.nodeCount) {
        return;
    }
    s.pins = pins;
    s.nodeCount = nodes.size();
    if (s.framing == State::Automatic || s.framing == State::Markers) {
        s.fit();
    }
    s.updateView();
}

void NodeMap::setSelfNode(const NetworkNode& node) {
    State& s = *state_;
    const bool positioned = nodePosition(node) && !node.id.isEmpty();
    const Pin self = positioned ? Pin{node.id, node.title, node.role, project(node.latitude, node.longitude)}
                                : Pin();
    if (self == s.self)
        return;
    s.self = self;
    s.buttons[Center]->describe(positioned ? "К своему радио" : "Начальная область");
    update();
}

bool NodeMap::focusNode(const NetworkNode& node) {
    if (!nodePosition(node))
        return false;
    State& s = *state_;
    s.focused = node.id;
    s.framing = State::Manual;
    s.moveCamera(project(node.latitude, node.longitude), qMax(13, s.zoom));
    update();
    return true;
}

void NodeMap::setExpanded(bool expanded) {
    MapButton* button = state_->buttons[Expand];
    button->setChecked(expanded);
    button->describe(expanded ? "Свернуть карту" : "Развернуть карту");
}

void NodeMap::activateControl(int control) {
    if (!isVisible())
        return;
    State& s = *state_;
    switch (control) {
        case Layers: emit settingsRequested(); break;
        case Fit: s.overview(); break;
        case Center: s.centerOnRadio(); break;
        case Expand: emit expansionRequested(); break;
        case ZoomIn: s.adjustZoom(1); break;
        case ZoomOut: s.adjustZoom(-1); break;
    }
}

void NodeMap::advanceCamera() {
    state_->cameraFrame();
}

void NodeMap::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    state_->placeControls();
    if (state_->framing != State::Manual) {
        state_->fit();
    }
    state_->prepareAtlases();
    state_->queueBuildings();
    state_->schedule();
}

void NodeMap::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (state_->framing != State::Manual) {
        state_->fit();
    }
    state_->prepareAtlases();
    state_->queueBuildings();
    state_->schedule();
}

void NodeMap::hideEvent(QHideEvent* event) {
    if (state_->camera.isActive()) {
        state_->center = state_->cameraTo;
        state_->center.setX(wrapped(state_->center.x()));
        state_->zoom = state_->cameraZoom;
        state_->stopCamera();
    }
    if (state_->source == Loopback) {
        state_->cancel();
        state_->stopLoopback();
    }
    state_->buildingTimer.stop();
    state_->pinLabels.clear();
    state_->buildingView.clear();
    state_->buildingQueue.clear();
    state_->buildings.releaseSectors();
    state_->settle.stop();
    state_->visibleKeys.clear();
    state_->attempted.clear();
    const QList<QString> keys = state_->memory.keys();
    for (int i = 0; i < keys.size(); ++i) {
        if (state_->memory.object(keys.at(i))->transient) {
            state_->memory.remove(keys.at(i));
        }
    }
    QWidget::hideEvent(event);
}

void NodeMap::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    state_->pressing = true;
    state_->dragging = false;
    state_->first = state_->last = event->pos();
    event->accept();
}

void NodeMap::mouseMoveEvent(QMouseEvent* event) {
    State& s = *state_;
    if (!s.pressing || !s.mapRect().contains(s.first)) {
        return;
    }
    if (!s.dragging && (event->pos() - s.first).manhattanLength() >= 7) {
        s.dragging = true;
    }
    if (s.dragging) {
        s.pan(event->pos() - s.last);
        s.last = event->pos();
    }
}

void NodeMap::mouseReleaseEvent(QMouseEvent* event) {
    State& s = *state_;
    if (!s.pressing || event->button() != Qt::LeftButton) {
        return;
    }
    s.pressing = false;
    const QPoint point = event->pos();
    if (s.dragging || (point - s.first).manhattanLength() >= 7) {
        return;
    }
    if (!s.issue.isEmpty() && s.hintRect().contains(point)) {
        emit settingsRequested();
    } else if (s.mapRect().contains(point)) {
        for (int i = 0; i < s.pinLabels.size(); ++i) {
            if (s.pinLabels.at(i).first.contains(point)) {
                emit nodeSelected(s.pinLabels.at(i).second);
                return;
            }
        }
        QList<Pin> hits;
        for (int i = 0; i < s.pins.size(); ++i) {
            const Pin& pin = s.pins.at(i);
            if ((s.screen(pin.position) - point).manhattanLength() <= 19) {
                hits.append(pin);
            }
        }
        if (hits.size() == 1) {
            emit nodeSelected(hits.first().id);
        } else if (hits.size() > 1) {
            QMenu chooser(this);
            for (int i = 0; i < hits.size(); ++i) {
                const Pin& pin = hits.at(i);
                QAction* action = chooser.addAction(pin.name.isEmpty() ? pin.id : pin.name);
                action->setData(pin.id);
            }
            const QAction* selected = chooser.exec(mapToGlobal(point));
            if (selected) {
                emit nodeSelected(selected->data().toString());
            }
        }
    }
}

void NodeMap::keyPressEvent(QKeyEvent* event) {
    State& s = *state_;
    switch (event->key()) {
        case Qt::Key_Left: s.pan(QPoint(45, 0)); break;
        case Qt::Key_Right: s.pan(QPoint(-45, 0)); break;
        case Qt::Key_Up: s.pan(QPoint(0, 45)); break;
        case Qt::Key_Down: s.pan(QPoint(0, -45)); break;
        case Qt::Key_Plus: case Qt::Key_Equal: s.adjustZoom(1); break;
        case Qt::Key_Minus: s.adjustZoom(-1); break;
        case Qt::Key_0: s.overview(); break;
        case Qt::Key_O: s.showRegion(); break;
        case Qt::Key_C: s.centerOnRadio(); break;
        default: QWidget::keyPressEvent(event); break;
    }
}

QString NodeMap::statusText() const {
    const State& s = *state_;
    return s.status(s.source != Offline && s.fullyDrawn(s.tiles()));
}

void NodeMap::paintEvent(QPaintEvent*) {
    State& s = *state_;
    QPainter painter(this);
    painter.fillRect(rect(), color(Background));
    const QRect map = s.mapRect();
    const QRect field = s.openMapRect();
    const QVector<TilePlacement> placements = s.source == Offline ? QVector<TilePlacement>() : s.tiles();
    const bool complete = s.fullyDrawn(placements);
    QFont font = painter.font();
    font.setPixelSize(13);
    painter.setFont(font);
    painter.save();
    painter.setClipRect(map);
    s.grid(painter, map);
    const bool world = !complete && s.drawAtlas(painter, map, s.world, World, s.detail.bounds.isEmpty());
    const bool detail = !complete && s.drawAtlas(painter, map, s.detail, s.detailRegion, true);
    const bool buildings = s.drawBuildings(painter, map);
    const bool online = s.drawTiles(painter, placements);
    s.drawPins(painter, map);
    font.setPixelSize(11);
    painter.setFont(font);
    const QFontMetrics details(font);

    const QString caption = s.source == Offline && s.issue.isEmpty()
        ? ru("Офлайн · %1").arg(regionCaption(s.region))
        : s.status(complete);
    const int captionWidth = details.width(caption) + 2;
    const int statusWidth = qMin(field.width() - 12, captionWidth + 16);
    const QRect status(field.left() + 6, field.top() + 6, statusWidth, 22);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color(Chrome));
    painter.drawRoundedRect(status, 6, 6);
    painter.setPen(color(s.issue.isEmpty() ? Paper : Repeater));
    painter.drawText(status.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                     captionWidth <= statusWidth - 16 ? caption
                         : details.elidedText(caption, Qt::ElideRight, qMax(1, statusWidth - 16)));
    painter.setRenderHint(QPainter::Antialiasing, false);

    // Keep every visible layer's credit above the pins and scale.
    QStringList attribution;
    if (online || buildings || (detail && (s.detailRegion == OmskCity || s.detailRegion == OmskOblast))) {
        attribution.append(credit());
    }
    if (world || (detail && s.detailRegion == Siberia)) {
        attribution.append(QLatin1String("Natural Earth"));
    }
    if (attribution.isEmpty() && (s.source == Offline || !s.issue.isEmpty())) {
        attribution.append(ru("Подложка недоступна"));
    }
    QRect footer;
    if (!attribution.isEmpty()) {
        int w = 0;
        for (int i = 0; i < attribution.size(); ++i) {
            w = qMax(w, details.width(attribution.at(i)));
        }
        const int attributionHeight = details.height() * attribution.size() + 6;
        const int width = qMin(w + 10, field.width() - 8);
        footer = QRect(field.right() - width - 3, field.bottom() - attributionHeight - 2,
                       width, attributionHeight);
        painter.fillRect(footer, color(Background));
        painter.setPen(color(Muted));
        painter.drawText(footer.adjusted(5, 0, -5, 0), Qt::AlignRight | Qt::AlignVCenter,
            attribution.join(QLatin1String("\n")));
    }
    const double latitudeScale = 1.0 / std::cosh((0.5 - s.center.y()) * 2 * Pi);
    const double metersPerPixel = 40030230.0 * latitudeScale / s.scale();
    const double available = qMin(64, qMax(22, map.width() / 4)) * metersPerPixel;
    double unit = 1.0;
    while (unit * 10 <= available) {
        unit *= 10;
    }
    const int distance = static_cast<int>((available >= unit * 5 ? 5 : available >= unit * 2 ? 2 : 1) * unit);
    const int pixels = qMax(1, static_cast<int>(distance / metersPerPixel));
    const QString distanceLabel = distance >= 1000 ? ru("%1 км").arg(distance / 1000) : ru("%1 м").arg(distance);
    const int scaleWidth = qMax(pixels, details.width(distanceLabel)) + 14;
    const int baseline = !footer.isEmpty() && field.left() + scaleWidth + 10 >= footer.left()
        ? footer.top() - 6 : field.bottom() - 8;
    painter.fillRect(QRect(field.left() + 6, baseline - 18, scaleWidth, 23), color(Background));
    painter.setPen(color(Muted));
    painter.drawText(13, baseline - 5, distanceLabel);
    painter.drawLine(13, baseline, 13 + pixels, baseline);
    painter.drawLine(13, baseline - 3, 13, baseline + 3);
    painter.drawLine(13 + pixels, baseline - 3, 13 + pixels, baseline + 3);
    if (!s.issue.isEmpty()) {
        const QRect hint = s.hintRect();
        painter.fillRect(hint, color(Chrome));
        painter.setPen(color(Paper));
        painter.drawText(hint.adjusted(6, 2, -6, -2), Qt::AlignCenter, ru("Открыть настройки карты"));
    }
    painter.restore();
}

void NodeMap::loadVisibleBuildings() {
    State& s = *state_;
    if (!isVisible() || s.camera.isActive() || s.source != Offline || s.zoom < BuildingZoom ||
        s.buildingQueue.isEmpty()) {
        return;
    }
    if (!s.buildings.tried()) {
        const int sectors = s.buildings.open();
        s.mark("map: offline building sectors indexed", sectors);
        if (sectors == 0) {
            s.buildingQueue.clear();
            return;
        }
    }
    int loaded = 0;
    // A dense city view can span several sectors. Yield to input between pairs.
    for (int i = 0; i < 2 && !s.buildingQueue.isEmpty(); ++i) {
        const quint32 key = s.buildingQueue.first();
        s.buildingQueue.remove(0);
        if (s.buildings.load(key)) {
            ++loaded;
        }
    }
    if (loaded > 0) {
        s.mark("map: offline building sectors decoded", loaded);
        update();
    }
    if (!s.buildingQueue.isEmpty()) {
        s.buildingTimer.start(0);
    }
}

void NodeMap::loadVisibleTiles() {
    State& s = *state_;
    if (s.source != Loopback || s.reply || !isVisible() || !s.issue.isEmpty() || s.camera.isActive()) {
        return;
    }
    s.prepareNetwork();
    QVector<TilePlacement> placements = s.tiles();
    const QPoint middle = s.mapAnchor();
    std::sort(placements.begin(), placements.end(), [middle](const TilePlacement& left, const TilePlacement& right) {
        return (left.rect.center() - middle).manhattanLength() < (right.rect.center() - middle).manhattanLength();
    });
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < placements.size(); ++i) {
        const QString key = placements.at(i).path();
        const CachedImage* decoded = s.memory.object(key);
        if ((decoded && decoded->expires > now) || s.attempted.contains(key)) {
            continue;
        }
        s.attempted.insert(key);
        // Keep the cache stable when the local proxy restarts on a different port.
        const QUrl cacheUrl(QLatin1String("http://localhost/ommesh-osm-tiles/") + key);
        QNetworkCacheMetaData saved = s.disk->metaData(cacheUrl);
        if (saved.isValid() && saved.expirationDate() > now) {
            const QImage image = decodeTile(s.stored(cacheUrl));
            if (image.isNull()) {
                s.disk->remove(cacheUrl);
                saved = QNetworkCacheMetaData();
            } else {
                s.hold(key, image, saved.expirationDate(), false);
                update();
                continue;
            }
        }
        if (!s.prepareLoopback()) {
            update();
            return;
        }
        QNetworkRequest request(QUrl(s.localProxy->baseUrl() + key));
        request.setRawHeader("User-Agent", "OMMesh-Belle-SIS/0.2.19 (+https://github.com/cm4ker/ommesh)");
        request.setRawHeader("Accept", "image/png");
        request.setRawHeader("Accept-Encoding", "identity");
        const QNetworkCacheMetaData::RawHeaderList previous = saved.rawHeaders();
        const QByteArray etag = headerValue(previous, "etag");
        const QByteArray modified = headerValue(previous, "last-modified");
        if (!etag.isEmpty()) {
            request.setRawHeader("If-None-Match", etag);
        }
        if (!modified.isEmpty()) {
            request.setRawHeader("If-Modified-Since", modified);
        }
        s.replyCacheUrl = cacheUrl;
        s.replyKey = key;
        s.received.clear();
        s.requestError.clear();
        if (s.firstTile) {
            s.mark("map: first tile request");
        }
        s.reply = s.network->get(request);
        if (s.firstTile) {
            s.mark("map: request returned from Qt");
        }
        s.reply->setReadBufferSize(MaximumTileBytes + 1);
        connect(s.reply, SIGNAL(readyRead()), this, SLOT(readTileData()));
        connect(s.reply, SIGNAL(finished()), this, SLOT(finishCurrentReply()), Qt::QueuedConnection);
        s.deadline.start(24000);
        if (s.reply->isFinished()) {
            QTimer::singleShot(0, this, SLOT(finishTile()));
        }
        return;
    }
}

void NodeMap::readTileData() {
    State& s = *state_;
    if (!s.reply || sender() != s.reply || !s.requestError.isEmpty()) {
        return;
    }
    s.received += s.reply->read(MaximumTileBytes + 1 - s.received.size());
    if (s.received.size() > MaximumTileBytes) {
        s.requestError = ru("Плитка слишком большая");
        QTimer::singleShot(0, this, SLOT(abortFailedTile()));
    }
}

void NodeMap::finishCurrentReply() {
    if (sender() == state_->reply) {
        finishTile();
    }
}

void NodeMap::finishTile() {
    State& s = *state_;
    if (!s.reply || !s.reply->isFinished()) {
        return;
    }
    QNetworkReply* reply = s.reply;
    s.reply = nullptr;
    s.deadline.stop();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (s.firstTile || status >= 400 || reply->error() != QNetworkReply::NoError) {
        s.mark("map: HTTP status", status);
        if (reply->error() != QNetworkReply::NoError) {
            s.mark("map: Qt reply error", static_cast<int>(reply->error()));
        }
    }
    s.firstTile = false;
    QString failure = s.requestError;
    if (failure.isEmpty() && (status == 200 || status == 304)) {
        QNetworkCacheMetaData::RawHeaderList headers = reply->rawHeaderPairs();
        QByteArray data;
        if (status == 304) {
            data = s.stored(s.replyCacheUrl);
            QNetworkCacheMetaData::RawHeaderList previous = s.disk->metaData(s.replyCacheUrl).rawHeaders();
            for (int i = 0; i < headers.size(); ++i) {
                const QByteArray name = headers.at(i).first.toLower();
                for (int j = previous.size() - 1; j >= 0; --j) {
                    if (previous.at(j).first.toLower() == name) {
                        previous.removeAt(j);
                    }
                }
                previous.append(headers.at(i));
            }
            headers = previous;
        } else {
            data = s.received + reply->read(MaximumTileBytes + 1 - s.received.size());
        }
        const QImage image = decodeTile(data);
        if (image.isNull()) {
            s.disk->remove(s.replyCacheUrl);
            failure = ru("Плитка не прочитана");
        } else {
            const QByteArray control = headerValue(headers, "cache-control").toLower();
            const bool noStore = control.contains("no-store");
            const QDateTime expires = tileExpiry(headers, QDateTime::currentDateTimeUtc());
            s.hold(s.replyKey, image, expires, noStore);
            if (noStore) {
                s.disk->remove(s.replyCacheUrl);
            } else {
                s.save(s.replyCacheUrl, data, expires, headers);
            }
        }
    } else if (failure.isEmpty()) {
        if (s.localProxy && (status == 502 || status == 504) && !s.localProxy->lastError().isEmpty()) {
            failure = s.localProxy->lastError();
        } else if (status > 0) {
            failure = ru("Сервер карты: HTTP %1").arg(status);
        } else if (reply->error() == QNetworkReply::ContentAccessDenied) {
            failure = ru("Проверьте разрешение сети");
        } else {
            failure = ru("Нет связи с прокси на телефоне");
        }
    }
    reply->deleteLater();
    s.received.clear();
    s.requestError.clear();
    if (!failure.isEmpty()) {
        s.issue = failure;
    } else if (isVisible() && !s.settle.isActive() && !s.camera.isActive()) {
        s.settle.start(0);
    }
    update();
}

void NodeMap::tileTimedOut() {
    State& s = *state_;
    if (s.reply && !s.reply->isFinished()) {
        s.requestError = ru("Тайм-аут карты");
        abortFailedTile();
    }
}

void NodeMap::abortFailedTile() {
    State& s = *state_;
    if (s.reply && !s.requestError.isEmpty() && !s.reply->isFinished()) {
        s.reply->abort();
    }
}

MapSettingsPanel::MapSettingsPanel(QWidget* parent)
    : QWidget(parent), modes_(new QButtonGroup(this)), regions_(new QButtonGroup(this)),
      regionChoices_(new QWidget(this)), instructions_(new QLabel(this)), validation_(new QLabel(this)) {
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QLatin1String(
        "QPushButton#mapOffline:checked, QPushButton#mapLoopback:checked,"
        "QPushButton#mapRegionOmsk:checked, QPushButton#mapRegionOblast:checked,"
        "QPushButton#mapRegionSiberia:checked, QPushButton#mapRegionWorld:checked"
        " { border: 1px solid #62d5be; background: #26413f; color: #baf4e7; }"
        "QLabel#mapValidation { color: #f2bc73; }"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    modes_->setExclusive(true);
    const struct { NodeMap::Source mode; const char* caption; const char* name; } options[] = {
        {NodeMap::Offline, "Офлайн", "mapOffline"},
        {NodeMap::Loopback, "OpenStreetMap · localhost", "mapLoopback"},
    };
    for (const auto& option : options) {
        auto* button = new QPushButton(ru(option.caption), this);
        button->setObjectName(QLatin1String(option.name));
        button->setCheckable(true);
        button->setMinimumHeight(40);
        modes_->addButton(button, option.mode);
        layout->addWidget(button);
    }
    instructions_->setTextFormat(Qt::PlainText);
    instructions_->setWordWrap(true);
    layout->addWidget(instructions_);
    auto* regionLayout = new QGridLayout(regionChoices_);
    regionLayout->setContentsMargins(0, 0, 0, 0);
    regionLayout->setSpacing(4);
    const char* regionNames[] = {"mapRegionOmsk", "mapRegionOblast", "mapRegionSiberia", "mapRegionWorld"};
    regions_->setExclusive(true);
    for (int i = NodeMap::OmskCity; i <= NodeMap::World; ++i) {
        auto* button = new QPushButton(regionCaption(static_cast<NodeMap::OfflineRegion>(i)), regionChoices_);
        button->setObjectName(QLatin1String(regionNames[i]));
        button->setMinimumHeight(38);
        button->setCheckable(true);
        regions_->addButton(button, i);
        regionLayout->addWidget(button, 0, i);
    }
    layout->addWidget(regionChoices_);
    validation_->setObjectName(QLatin1String("mapValidation"));
    validation_->setTextFormat(Qt::PlainText);
    validation_->setWordWrap(true);
    layout->addWidget(validation_);
    auto* done = new QPushButton(ru("Готово"), this);
    done->setObjectName(QLatin1String("mapDone"));
    done->setMinimumHeight(40);
    layout->addWidget(done);
    layout->addStretch(1);
    connect(modes_, SIGNAL(buttonClicked(int)), this, SLOT(selectMode(int)), Qt::QueuedConnection);
    connect(done, SIGNAL(clicked()), this, SLOT(scheduleSave()));
    reload();
}

void MapSettingsPanel::reload() {
    saveQueued_ = false;
    QSettings preferences;
    const int mode = preferences.value(QLatin1String("map/source"), NodeMap::Offline).toInt();
    const int selected = NodeMap::sourceFromPreference(mode);
    modes_->button(selected)->setChecked(true);
    const int savedRegion = preferences.value(QLatin1String("map/region"), NodeMap::OmskCity).toInt();
    regions_->button(qBound(static_cast<int>(NodeMap::OmskCity), savedRegion,
                            static_cast<int>(NodeMap::World)))->setChecked(true);
    selectMode(selected);
}

void MapSettingsPanel::selectMode(int mode) {
    if (mode != modes_->checkedId()) {
        return;
    }
    validation_->hide();
    regionChoices_->setVisible(mode == NodeMap::Offline);
    instructions_->setText(mode == NodeMap::Offline
        ? ru("Выберите начальную область. Все четыре карты работают без интернета.")
        : ru("Прокси localhost работает на телефоне. Для плиток OpenStreetMap допускается неизвестный центр сертификации. Нужен интернет."));
}

void MapSettingsPanel::scheduleSave() {
    if (!saveQueued_) {
        saveQueued_ = true;
        QTimer::singleShot(0, this, SLOT(save()));
    }
}

void MapSettingsPanel::save() {
    if (!saveQueued_) {
        return;
    }
    saveQueued_ = false;
    if (!isVisible()) {
        return;
    }
    const int mode = modes_->checkedId();
    QSettings preferences;
    preferences.setValue(QLatin1String("map/source"), mode);
    if (mode == NodeMap::Offline) {
        preferences.setValue(QLatin1String("map/region"), regions_->checkedId());
    }
    preferences.sync();
    if (preferences.status() != QSettings::NoError) {
        validation_->setText(ru("Не удалось сохранить настройки карты."));
        validation_->show();
        return;
    }
    validation_->hide();
    setFocus(Qt::OtherFocusReason);
    emit saved();
}
