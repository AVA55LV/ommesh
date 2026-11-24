#ifndef OMMESH_NATIVE_TILE_HTTPS_HPP
#define OMMESH_NATIVE_TILE_HTTPS_HPP

#include <QtCore/QAtomicInt>
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QUrl>

#ifdef Q_OS_SYMBIAN
QByteArray fetchOmmeshLinkPreview(const QByteArray& encodedUrl, const QAtomicInt* cancelled, int maximumBytes);

// Strictly verified HTTPS with no redirects. Test reads never receive the signed PUT address.
int transferAttachment(const QUrl& address, bool put, const QByteArray& contentType,
                       const QByteArray& disposition, const QByteArray& body,
                       const QByteArray& customCa, const QAtomicInt* cancelled);
#endif

#endif
