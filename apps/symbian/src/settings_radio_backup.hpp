#ifndef OMMESH_BELLE_SETTINGS_RADIO_BACKUP_HPP
#define OMMESH_BELLE_SETTINGS_RADIO_BACKUP_HPP

#include <QtCore/QByteArray>
#include <QtGui/QWidget>
#include <memory>

class Client;

class RadioBackupCompletion : public QObject {
    Q_OBJECT

public:
    RadioBackupCompletion(Client& client, int request, const QString& sourceKey,
                          const QByteArray& belleHistory, const QByteArray& androidHistory);

signals:
    void completed(int request, const QString& text);

private slots:
    void finish(int request, bool success, const QString& publicKey, const QString& detail);

private:
    Client& client_;
    const int request_;
    const QString sourceKey_;
    const QByteArray belleHistory_;
    const QByteArray androidHistory_;
};

class RadioBackupPanel : public QWidget {
    Q_OBJECT

public:
    explicit RadioBackupPanel(Client& client, QWidget* parent = nullptr);
    ~RadioBackupPanel() override;

signals:
    void detailRequested(QWidget* page, const QString& title);
    void detailCloseRequested(QWidget* page);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void activateAction();
    void openKeyFile();
    void reviewKeyText();
    void choosePath(const QString& path);
    void applyRestore();
    void cancelReview();
    void updateAvailability();
    void revealReview();
    void keyResult(int request, int result, const QByteArray& key, const QString& detail);
    void progress(int request, const QString& stage, int done, int total);
    void restoreResult(int request, const QString& detail);

private:
    enum Action { SaveRadio = 1, RestoreRadio, SaveKey, RestoreKey, History };
    void openPicker(Action action);
    void preview(const QByteArray& bytes, Action action);
    void showHistory();
    void report(const QString& text);

    struct State;
    std::unique_ptr<State> state_;
};

#endif
