#ifndef OMMESH_BELLE_SETTINGS_BACKUP_HPP
#define OMMESH_BELLE_SETTINGS_BACKUP_HPP

#include "backup_history_import.hpp"

#include <QtCore/QVariant>
#include <QtCore/QStringList>
#include <QtGui/QWidget>

class Client;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSettings;

namespace SettingsBackup {

struct Data {
    QVariantMap values;
    int histories = 0;
    int messages = 0;
};

bool writeFile(const QString& path, QSettings& settings, QString& error);
bool readFile(const QString& path, Data& data, QString& error);
bool restore(const Data& data, QSettings& settings, QString& error);
QString defaultDirectory();
bool saveBytes(const QString& path, const QByteArray& bytes, QString& error);
bool readRadioHistory(QSettings& settings, const QString& publicKey, QByteArray& bytes, int& count);
bool validRadioHistory(const QByteArray& bytes, int& count);
bool restoreRadioHistory(QSettings& settings, const QString& publicKey, const QByteArray& bytes, QString& error);

}  // namespace SettingsBackup

class SettingsBackupFilePicker : public QWidget {
    Q_OBJECT

public:
    SettingsBackupFilePicker(bool exportFile, const QString& directory, QWidget* parent = nullptr,
                             const QStringList& patterns = QStringList());

signals:
    void pathSelected(const QString& path);
    void closeRequested(QWidget* page);

private slots:
    void showDrives();
    void goUp();
    void activateEntry(QListWidgetItem* entry);
    void openPendingDirectory();
    void updateSelection();
    void choosePath();
    void cancel();

private:
    void showDirectory(const QString& directory);
    void addEntry(const QString& label, const QString& path, bool isDirectory);

    const bool exportFile_;
    const QStringList patterns_;
    QString directory_;
    QString pendingDirectory_;
    QListWidget* entries_;
    QLabel* path_;
    QLabel* empty_;
    QPushButton* disks_;
    QPushButton* up_;
    QPushButton* choose_;
    bool closing_ = false;
};

class SettingsBackupPanel : public QWidget {
    Q_OBJECT

public:
    explicit SettingsBackupPanel(Client& client, QWidget* parent = nullptr);

signals:
    void backupRestored();
    void pickerRequested(QWidget* page, const QString& title);
    void pickerClosed(QWidget* page);

private slots:
    void chooseExportDirectory();
    void chooseImportFile();
    void chooseAndroidHistory();
    void exportBackup(const QString& directory);
    void reviewBackup(const QString& path);
    void reviewAndroidHistory(const QString& path);
    void applyBackup();
    void cancelRestore();

private:
    void chooseFile(bool exportFile);
    bool canRestore() const;
    void setFeedback(const QString& message);

    Client& client_;
    SettingsBackup::Data pending_;
    BackupHistoryImport::Plan importedHistory_;
    QString directory_;
    QLabel* feedback_ = nullptr;
    QLabel* confirmationNote_ = nullptr;
    QWidget* confirmation_ = nullptr;
};

#endif
