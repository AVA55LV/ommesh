#include "local_tile_proxy.hpp"

#include "native_tile_https.hpp"

#include <QtCore/QAtomicInt>
#include <QtCore/QBasicTimer>
#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QList>
#include <QtCore/QThread>
#include <QtCore/QTimerEvent>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <utility>

namespace {

const int HeaderLimit = 4096;
const int TileLimit = 160 * 1024;
const int HeaderTimeout = 3000;
const int WriteTimeout = 5000;
const int ServiceInterval = 30;

QString ru(const char* text) { return QString::fromUtf8(text); }

bool plainHeader(const QByteArray& value) {
    if (value.size() > 1024) {
        return false;
    }
    for (int i = 0; i < value.size(); ++i) {
        const unsigned char character = static_cast<unsigned char>(value.at(i));
        if (character != '\t' && (character < 32 || character > 126)) {
            return false;
        }
    }
    return true;
}

bool parseNumber(const QByteArray& raw, int lengthLimit, int& number) {
    if (raw.isEmpty() || raw.size() > lengthLimit) {
        return false;
    }
    number = 0;
    for (int i = 0; i < raw.size(); ++i) {
        const unsigned char digit = static_cast<unsigned char>(raw.at(i));
        if (digit < '0' || digit > '9') {
            return false;
        }
        number = 10 * number + digit - '0';
    }
    return true;
}

bool parsePath(const QByteArray& path, TileHttpsRequest& request) {
    if (!path.startsWith("/tiles/") || !path.endsWith(".png")) {
        return false;
    }
    const QList<QByteArray> parts = path.mid(7, path.size() - 11).split('/');
    if (parts.size() != 3 ||
        !parseNumber(parts.at(0), 2, request.z) ||
        !parseNumber(parts.at(1), 6, request.x) ||
        !parseNumber(parts.at(2), 6, request.y) ||
        request.z < 1 || request.z > 17) {
        return false;
    }
    return request.x < (1 << request.z) && request.y < (1 << request.z);
}

// Zero means that the only permitted upstream query was parsed successfully.
int parseHeaders(const QByteArray& raw, quint16 port, TileHttpsRequest& request) {
    const int requestEnd = raw.indexOf("\r\n");
    const QByteArray line = requestEnd < 0 ? raw : raw.left(requestEnd);
    if (!plainHeader(line)) {
        return 400;
    }
    const QList<QByteArray> fields = line.split(' ');
    if (fields.size() != 3 || (fields.at(2) != "HTTP/1.0" && fields.at(2) != "HTTP/1.1")) {
        return 400;
    }
    if (fields.at(0) != "GET") {
        return 405;
    }
    if (!parsePath(fields.at(1), request)) {
        return 404;
    }

    bool hostSeen = false;
    bool etagSeen = false;
    bool modifiedSeen = false;
    int start = requestEnd < 0 ? raw.size() : requestEnd + 2;
    while (start < raw.size()) {
        int end = raw.indexOf("\r\n", start);
        if (end < 0) {
            end = raw.size();
        }
        const QByteArray header = raw.mid(start, end - start);
        const int separator = header.indexOf(':');
        if (!plainHeader(header) || separator <= 0 || header.startsWith(' ') || header.startsWith('\t')) {
            return 400;
        }
        const QByteArray name = header.left(separator).toLower();
        const QByteArray value = header.mid(separator + 1).trimmed();
        if (name == "host") {
            if (hostSeen || value != "127.0.0.1:" + QByteArray::number(port)) {
                return 400;
            }
            hostSeen = true;
        } else if (name == "transfer-encoding" || (name == "content-length" && value != "0")) {
            return 400;
        } else if (name == "if-none-match") {
            if (etagSeen) {
                return 400;
            }
            request.etag = value;
            etagSeen = true;
        } else if (name == "if-modified-since") {
            if (modifiedSeen) {
                return 400;
            }
            request.lastModified = value;
            modifiedSeen = true;
        }
        start = end + 2;
    }
    return fields.at(2) == "HTTP/1.1" && !hostSeen ? 400 : 0;
}

class TileWorker final : public QThread {
public:
    explicit TileWorker(TileHttpsRequest request) : request_(std::move(request)) {
        request_.cancelled = &cancelled_;
        setStackSize(96 * 1024);
    }

    void cancel() {
        cancelled_.fetchAndStoreRelease(1);
    }

    TileHttpsResult take() {
        return std::move(result_);
    }

protected:
    void run() override {
        result_ = fetchOsmTile(request_);
    }

private:
    QAtomicInt cancelled_{0};
    TileHttpsRequest request_;
    TileHttpsResult result_;
};

bool validTile(const TileHttpsResult& result) {
    const int parameters = result.contentType.indexOf(';');
    const QByteArray type = (parameters < 0 ? result.contentType : result.contentType.left(parameters))
                                .trimmed().toLower();
    static const char header[] = "\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00\x01\x00\x00\x00\x01\x00";
    return type == "image/png" && result.body.size() >= 33 && result.body.size() <= TileLimit &&
        result.body.startsWith(QByteArray::fromRawData(header, sizeof(header) - 1));
}

void appendHeader(QByteArray& headers, const char* name, const QByteArray& value) {
    if (!value.isEmpty() && plainHeader(value)) {
        headers += name;
        headers += ": ";
        headers += value;
        headers += "\r\n";
    }
}

const char* statusText(int status) {
    switch (status) {
    case 200: return "OK";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 431: return "Request Header Fields Too Large";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Tile Service Response";
    }
}

}

struct LocalTileProxy::State final : QTcpServer {
    QBasicTimer timer;
    QElapsedTimer clock;
    QTcpSocket* peer = nullptr;
    TileWorker* worker = nullptr;
    QByteArray received;
    QString error;
    qint64 peerDeadline = 0;
    bool allowUntrustedCa = false;
    bool expectsTile = false;
    bool sending = false;

    State() {
        setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        setMaxPendingConnections(2);
        clock.start();
    }

    ~State() override {
        shutDown();
        timer.stop();
        if (worker) {
            if (worker->wait(50)) {
                delete worker;
            } else {
                // No mutable State survives in the worker. Let cancellation unwind it.
                connect(worker, SIGNAL(finished()), worker, SLOT(deleteLater()), Qt::QueuedConnection);
                if (worker->isFinished()) {
                    worker->deleteLater();
                }
            }
        }
    }

    void shutDown() {
        close();
        dropSocket();
        while (hasPendingConnections()) {
            delete nextPendingConnection();
        }
        if (!worker) {
            timer.stop();
        }
    }

    void dropSocket() {
        if (peer) {
            peer->abort();
            delete peer;
            peer = nullptr;
        }
        if (worker && expectsTile) {
            worker->cancel();
        }
        expectsTile = false;
        sending = false;
        received.clear();
    }

    void incomingConnection(int descriptor) override {
        QTcpServer::incomingConnection(descriptor);
        if (!timer.isActive()) {
            timer.start(ServiceInterval, this);
        }
    }

    void timerEvent(QTimerEvent* event) override {
        if (event->timerId() != timer.timerId()) {
            QTcpServer::timerEvent(event);
            return;
        }
        if (peer && peer->state() == QAbstractSocket::UnconnectedState) {
            dropSocket();
        }
        if (worker && worker->isFinished() && worker->wait(0)) {
            TileHttpsResult fetched = worker->take();
            delete worker;
            worker = nullptr;
            if (peer && expectsTile) {
                expectsTile = false;
                returnTile(std::move(fetched));
            }
        }
        if (!isListening()) {
            if (!worker) {
                timer.stop();
            }
            return;
        }
        if (!peer && hasPendingConnections()) {
            peer = nextPendingConnection();
            peer->setReadBufferSize(HeaderLimit + 1);
            peerDeadline = clock.elapsed() + HeaderTimeout;
        }
        if (!peer && !worker) {
            timer.stop();
            return;
        }
        if (!peer || expectsTile) {
            return;
        }
        if (clock.elapsed() >= peerDeadline) {
            if (sending) dropSocket();
            else respond(408);
            return;
        }
        if (!sending) {
            readRequest();
        }
    }

    void readRequest() {
        received += peer->read(HeaderLimit + 1 - received.size());
        if (received.size() > HeaderLimit) {
            respond(431);
            return;
        }
        const int end = received.indexOf("\r\n\r\n");
        if (end < 0) {
            return;
        }
        if (end + 4 != received.size()) {
            respond(400);
            return;
        }
        TileHttpsRequest request;
        const int parseStatus = parseHeaders(received.left(end), serverPort(), request);
        received.clear();
        if (parseStatus != 0) {
            respond(parseStatus);
            return;
        }
        error.clear();
        if (worker) {
            error = ru("Предыдущий запрос карты ещё завершает работу.");
            respond(503);
            return;
        }
        request.allowUntrustedCa = allowUntrustedCa;
        expectsTile = true;
        worker = new TileWorker(std::move(request));
        worker->start();
    }

    void returnTile(TileHttpsResult fetched) {
        if (fetched.status == 200 && !validTile(fetched)) {
            error = ru("Сервер карты не вернул PNG 256 × 256.");
            respond(502);
            return;
        }
        if (fetched.status != 200 && fetched.status != 304) {
            error = fetched.error;
            if (fetched.status < 400 || fetched.status > 599) {
                fetched.status = 502;
            }
            respond(fetched.status);
            return;
        }
        QByteArray headers;
        if (fetched.status == 200 && fetched.cacheControl.isEmpty() && fetched.expires.isEmpty()) {
            fetched.cacheControl = "max-age=604800";
        }
        appendHeader(headers, "Cache-Control", fetched.cacheControl);
        appendHeader(headers, "ETag", fetched.etag);
        appendHeader(headers, "Last-Modified", fetched.lastModified);
        appendHeader(headers, "Expires", fetched.expires);
        appendHeader(headers, "Age", fetched.age);
        if (fetched.status == 200) {
            appendHeader(headers, "Content-Type", "image/png");
        }
        respond(fetched.status, headers, fetched.status == 200 ? fetched.body : QByteArray());
    }

    void respond(int status, const QByteArray& headers = QByteArray(),
                 const QByteArray& body = QByteArray()) {
        QByteArray head = "HTTP/1.0 ";
        head += QByteArray::number(status);
        head += " ";
        head += statusText(status);
        head += "\r\nConnection: close\r\n";
        if (status != 304) {
            appendHeader(head, "Content-Length", QByteArray::number(body.size()));
        }
        if (status == 405) {
            appendHeader(head, "Allow", "GET");
        }
        head += headers;
        head += "\r\n";
        peer->write(head);
        if (!body.isEmpty()) {
            peer->write(body);
        }
        sending = true;
        peerDeadline = clock.elapsed() + WriteTimeout;
        peer->disconnectFromHost();
    }
};

LocalTileProxy::LocalTileProxy() : state_(new State) {}
LocalTileProxy::~LocalTileProxy() = default;

bool LocalTileProxy::start(bool allowUntrustedCa, QString* error) {
    if (error) {
        error->clear();
    }
    if (running() && state_->allowUntrustedCa == allowUntrustedCa) {
        return true;
    }
    stop();
    state_->error.clear();
    if (!state_->listen(QHostAddress::LocalHost, 0)) {
        state_->error = ru("Не удалось открыть локальный порт карты.") + QLatin1Char(' ') +
                        state_->errorString();
        if (error) {
            *error = state_->error;
        }
        return false;
    }
    state_->allowUntrustedCa = allowUntrustedCa;
    return true;
}

void LocalTileProxy::stop() {
    state_->shutDown();
}

bool LocalTileProxy::running() const {
    return state_->isListening();
}

QString LocalTileProxy::baseUrl() const {
    return running() ? QString::fromLatin1("http://127.0.0.1:%1/tiles/").arg(state_->serverPort())
                     : QString();
}

QString LocalTileProxy::lastError() const {
    return state_->error;
}
