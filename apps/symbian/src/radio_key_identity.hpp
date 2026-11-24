#ifndef OMMESH_BELLE_RADIO_KEY_IDENTITY_HPP
#define OMMESH_BELLE_RADIO_KEY_IDENTITY_HPP

#include <QtCore/QByteArray>

// MeshCore stores the expanded Ed25519 private key as a clamped 32-byte
// scalar followed by a 32-byte signing nonce prefix. Return a raw 32-byte
// public key, or an empty value if the key is invalid for MeshCore.
QByteArray deriveRadioPublicKey(const QByteArray& expandedPrivate);

// Compare the derived key with the supplied raw 32-byte public key.
bool radioKeyMatchesPublic(const QByteArray& expandedPrivate, const QByteArray& publicKey);

#endif
