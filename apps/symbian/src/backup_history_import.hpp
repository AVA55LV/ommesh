#ifndef OMMESH_BELLE_BACKUP_HISTORY_IMPORT_HPP
#define OMMESH_BELLE_BACKUP_HISTORY_IMPORT_HPP

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVector>

class QSettings;
struct RadioChannel;

namespace BackupHistoryImport {

struct Plan {
    QString radioKey;
    QString radioName;
    QByteArray before;
    QByteArray after;
    QString selectedBefore;
    int added = 0;
    int duplicates = 0;
    int skipped = 0;
    int omitted = 0;
};

bool preview(const QString& path, QSettings& settings, const QVector<RadioChannel>& channels,
             Plan& plan, QString& error);
bool preview(const QByteArray& archiveBytes, QSettings& settings, const QVector<RadioChannel>& channels,
             Plan& plan, QString& error);
bool apply(const Plan& plan, QSettings& settings, QString& error);

}  // namespace BackupHistoryImport

#endif
