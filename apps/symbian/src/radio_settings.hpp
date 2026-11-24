#ifndef OMMESH_BELLE_RADIO_SETTINGS_HPP
#define OMMESH_BELLE_RADIO_SETTINGS_HPP

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QDialog>
#include <QtGui/QWidget>

class Client;
class ChatAppearancePreview;
class NativeVibration;
class NodeMap;
class QButtonGroup;
class QLabel;
class QComboBox;
class QLineEdit;
class QPushButton;
class QTextEdit;

void configureNodeMap(NodeMap* map);

class RadioSettingsPanel : public QWidget {
    Q_OBJECT

public:
    RadioSettingsPanel(Client& client, QWidget* parent);
    ~RadioSettingsPanel() override;
    bool filter(const QString& query);
    QWidget* makeConnectionPreferences(QWidget* parent);

public slots:
    void editChannels();
    void addChannel();
    void advertise();
    void updateStatus();

signals:
    void detailRequested(QWidget* page, const QString& title);
    void detailCloseRequested(QWidget* page);
    void mapSettingsRequested();
    void appearanceChanged(int theme, int chatTextSize);
    void connectionSettingsRequested();
    void exitRequested();

private slots:
    void setReconnect(bool enabled);
    void setVibration(bool enabled);
    void onIncomingMessage(const QString& target, const QString& text, bool room);
    void testVibration();
    void retryTestVibration();
    void refreshMessages();
    void refreshInventory();
    void showIdentity();
    void showInfo();
    void showParameters();
    void showBattery();
    void showContacts();
    void showLocation();
    void showAdvanced();
    void showAttachments();
    void showBackup();
    void showAir();
    void showMaintenance();
    void setRadioClock();
    void clockResult(int request, bool success, const QString& status);
    void showNotifications();
    void chooseNotificationSound();
    void showLog();
    void showAbout();
    void showMessagePreferences();
    void showBlockedSenders();
    void unblockSender();
    void setLookalikeDetailsVisible(bool visible);
    void setLinkPreviewMode(int mode);
    void changeSendTries();
    void showAppearance();
    void updateAppearanceSummary(int theme, int chatTextSize);
    void setUserPreference(bool enabled);
    void refreshBatteryReadout();
    void updatePrivacyControls();
    void applyPrivacySettings();
    void showTrustedContacts();
    void updateTrustedContactSelection();
    void setSelectedTrustedContact();
    void finishPrivacySettings(int requestId, bool success, const QString& status);
    void openConsole();
    void toggleDemo();

private:
    void saveToggle(const char* key, bool enabled, QLabel* feedback = nullptr);
    void showVibrationResult(QLabel* feedback, int result);
    void updateSendTriesControls(QWidget* controls);
    void openChannels(bool adding);
    void updateBatteryPage();
    void refreshTrustedContacts();

    Client& client_;
    NativeVibration* vibration_;
    QPointer<QPushButton> demo_;
    QPushButton* nameRow_ = nullptr;
    QPushButton* frequencyRow_ = nullptr;
    QPushButton* batteryRow_ = nullptr;
    QPushButton* contactsRow_ = nullptr;
    QPushButton* locationRow_ = nullptr;
    QPointer<QPushButton> channelsRow_;
    QPushButton* connectionRow_ = nullptr;
    QPushButton* notificationsRow_ = nullptr;
    QPushButton* messagesRow_ = nullptr;
    QPushButton* attachmentsRow_ = nullptr;
    QPushButton* appearanceRow_ = nullptr;
    QPointer<QPushButton> mapsRow_;
    QPointer<QPushButton> refreshRow_;
    QPointer<QPushButton> clockRow_;
    QPointer<QLabel> clockFeedback_;
    int clockRequest_ = 0;
    QPointer<QLabel> notificationFeedback_;
    QPointer<QPushButton> notificationSoundRow_;
    QPointer<QLabel> vibrationRetryFeedback_;
    QPointer<QLabel> connectionFeedback_;
    QPointer<QLabel> liveReadout_;
    QPointer<QWidget> batteryPage_;
    QPointer<QLabel> batteryState_;
    QPointer<QPushButton> batteryRefresh_;
    QPointer<QPushButton> batteryPrivacy_;
    QPointer<QLineEdit> privacyLatitude_;
    QPointer<QLineEdit> privacyLongitude_;
    QPointer<QPushButton> privacyShare_;
    QPointer<QComboBox> privacyBase_;
    QPointer<QComboBox> privacySensors_;
    QPointer<QPushButton> privacySave_;
    QPointer<QPushButton> privacyTrusted_;
    QPointer<QLabel> privacyFeedback_;
    QPointer<QComboBox> trustedContacts_;
    QPointer<QPushButton> trustedChange_;
    QPointer<QLabel> trustedFeedback_;
    QString privacyRadioKey_;
    int privacyRequest_ = 0;
    int trustRequest_ = 0;
    QElapsedTimer lastNotification_;
    QTimer vibrationRetryTimer_;
    bool vibrateOnMessage_ = false;
};

class AppearancePanel : public QWidget {
    Q_OBJECT

public:
    explicit AppearancePanel(QWidget* parent);

signals:
    void appearanceChanged(int theme, int chatTextSize);

private slots:
    void selectTheme(int theme);
    void setChatSize(int pixels);
    void syncPreferences();
    void chooseChatBackground();

private:
    QSettings settings_;
    QLabel* sizeValue_ = nullptr;
    QPushButton* backgroundRow_ = nullptr;
    ChatAppearancePreview* preview_ = nullptr;
    int theme_ = 0;
    int chatTextSize_ = 0;
};

class RadioConsoleDialog : public QDialog {
    Q_OBJECT

public:
    RadioConsoleDialog(Client& client, const QString& nodeId, const QString& title, QWidget* parent,
                       bool loginOnly = false);

private slots:
    void submit();
    void login();
    void readyChanged();
    void result(int requestId, int operation, bool success, const QString& text);

private:
    void append(const QString& text);
    void updateControls();

    Client& client_;
    QString nodeId_;
    const bool loginOnly_;
    QLabel* state_;
    QLineEdit* password_;
    QLineEdit* command_ = nullptr;
    QPushButton* login_;
    QPushButton* send_ = nullptr;
    QTextEdit* output_ = nullptr;
    QString transcript_;
    int request_ = 0;
    bool authorized_ = false;
    bool connected_ = false;
};

class RadioChannelsDialog : public QDialog {
    Q_OBJECT

public:
    RadioChannelsDialog(Client& client, QWidget* parent, bool adding = false);

private slots:
    void selectSlot(int index);
    void saveChannel();
    void clearChannel();
    void receiveResult(int slot, bool success, const QString& text);

private:
    void populate(int preferredSlot);
    void submit(const QString& name, const QString& hex);

    Client& client_;
    QComboBox* slots_;
    QLineEdit* name_;
    QLineEdit* key_;
    QLabel* state_;
    QPushButton* save_;
    QPushButton* clear_;
    int pendingSlot_ = -1;
    const bool adding_;
};

#endif
