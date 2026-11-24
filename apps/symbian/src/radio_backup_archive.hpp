#ifndef OMMESH_BELLE_RADIO_BACKUP_ARCHIVE_HPP
#define OMMESH_BELLE_RADIO_BACKUP_ARCHIVE_HPP

#include "client.hpp"

#include <QtCore/QDateTime>
#include <QtCore/QVariantMap>

namespace RadioBackupArchive {

enum Part { Identity = 1, Settings = 2, Channels = 4, Contacts = 8, All = 15 };
enum { MaxFileBytes = 2 * 1024 * 1024 };

struct KeyFile {
    QString name;
    QString publicKey;
    QByteArray privateKey;
    QDateTime saved;
};

struct Archive {
    RadioBackupSnapshot source;
    QVariantMap settings;
    QByteArray privateKey;
    QByteArray belleHistory;
    QByteArray androidHistory;
    QDateTime saved;
    int messageCount = 0;

    bool hasKey() const { return !privateKey.isEmpty(); }
};

// Errors are user-facing and never include fields containing secrets.
QByteArray encode(const RadioBackupSnapshot& source, const QByteArray& privateKey,
                  const QByteArray& belleHistory, QString& error);
bool decode(const QByteArray& json, Archive& out, QString& error);
QByteArray encodeKey(const RadioBackupSnapshot& source, const QByteArray& privateKey, QString& error);
bool decodeKey(const QByteArray& input, KeyFile& out, QString& error);
bool planRestore(const Archive& archive, const RadioBackupSnapshot& target, unsigned parts,
                 RadioBackupRestore& out, QString& error);
bool planKeyRestore(const KeyFile& key, const RadioBackupSnapshot& target,
                    RadioBackupRestore& out, QString& error);

} // namespace RadioBackupArchive

#endif
