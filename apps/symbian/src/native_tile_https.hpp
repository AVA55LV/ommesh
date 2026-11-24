#ifndef OMMESH_NATIVE_TILE_HTTPS_HPP
#define OMMESH_NATIVE_TILE_HTTPS_HPP

#include <QtCore/QAtomicInt>
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QUrl>

struct TileHttpsRequest {
    int z = 0;
    int x = 0;
    int y = 0;
    QByteArray etag;
    QByteArray lastModified;
    bool allowUntrustedCa = false;
    const QAtomicInt* cancelled = nullptr;
};

struct TileHttpsResult {
    int status = 0;
    QByteArray contentType;
    QByteArray body;
    QByteArray cacheControl;
    QByteArray etag;
    QByteArray lastModified;
    QByteArray expires;
    QByteArray age;
    QString error;
};

// Own one upstream request on a dedicated worker. The worker must keep the
// cancellation flag alive until this call returns.
TileHttpsResult fetchOsmTile(const TileHttpsRequest& request);

#ifdef Q_OS_SYMBIAN
QByteArray fetchOmmeshLinkPreview(const QByteArray& encodedUrl, const QAtomicInt* cancelled, int maximumBytes);

// Strictly verified HTTPS with no redirects. Test reads never receive the signed PUT address.
int transferAttachment(const QUrl& address, bool put, const QByteArray& contentType,
                       const QByteArray& disposition, const QByteArray& body,
                       const QByteArray& customCa, const QAtomicInt* cancelled);
#endif

#endif
