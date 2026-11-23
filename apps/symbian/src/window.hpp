#ifndef OMMESH_BELLE_WINDOW_HPP
#define OMMESH_BELLE_WINDOW_HPP

#include "appearance.hpp"
#include "client.hpp"

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QVariant>
#include <QtGui/QWidget>

class ChatView;
class NodeMap;
class RadioSettingsPanel;
class QAction;
class QButtonGroup;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

QScrollArea* makeTouchScrollArea(QWidget* parent, QWidget* content);

class ClientWindow : public QWidget {
    Q_OBJECT

public:
    explicit ClientWindow(Client& client, QWidget* parent = nullptr);

public slots:
    void showDetailPage(QWidget* page, const QString& title);

protected:
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void selectTab(int tab);
    void openConnection();
    void returnToDashboard();
    void showChats();
    void applyNavigation();
    void exitApplication();
    void openNewChat();
    void filterChooser(const QString& text);
    void selectChooserKind(int kind);
    void openChatChannels();
    void filterChats();
    void chooseChatFilter();
    void applyChatFilter(QAction* choice);
    void filterNodes();
    void filterSettings(const QString& text);
    void toggleSettingsSearch(bool open);
    void openTarget(QListWidgetItem* item);
    void openPendingTarget();
    void openNode(QListWidgetItem* item);
    void openNode(const QString& id);
    void showMap();
    void openMapSettings();
    void mapSettingsSaved();
    void showChatOptions(const QPoint& position);
    void toggleActivePin();
    void manageSettingsConnection();
    void retrySettingsConnection();
    void scheduleConnection();
    void toggleConnection();
    void scheduleViewUpdate();
    void applyAppearance(int theme, int chatTextSize);
    void applyViewUpdate();
    void updateView();
    void sendMessage(const QString& text);
    void resendMessage(const QString& text);
    void reactToMessage(const ChatMessage& message, const QString& emoji);
    void setSenderBlocked(const QString& id, const QString& name, bool blocked);

private:
    enum Tab { Chats, Network, Settings };
    enum class ConnectionRequest { None, Connect, Disconnect };
    enum class Navigation { None, Tab, Dashboard, Connection, Detail, Map, MapSettings, MapSaved, Exit, NewChat };

    QWidget* makeDashboard();
    QWidget* makeTabs();
    QWidget* makeChatsPage();
    QWidget* makeNetworkPage();
    QWidget* makeSettingsPage();
    QWidget* makeConnectionPage();
    void queueNavigation(Navigation action);
    void focusNavigation(int tab);
    void showPendingDetail();
    void clearDetail();
    void makeChatChooser();
    void updateChatChooser(const QVector<ChatTarget>& targets);
    void showTargets(const QVector<ChatTarget>& targets);
    void showNodes(const QVector<NetworkNode>& nodes);
    void showMessages(const QVector<ChatTarget>& targets);
    void saveDraft();
    bool isPinned(const ChatTarget& target) const;
    void togglePin(const QString& id);

    Client& client_;
    QStackedWidget* pages_ = nullptr;
    QWidget* dashboard_ = nullptr;
    QStackedWidget* tabPages_ = nullptr;
    QWidget* connectionPage_ = nullptr;
    ChatView* chatPage_ = nullptr;
    QWidget* detailPage_ = nullptr;
    QVBoxLayout* detailLayout_ = nullptr;
    QLabel* detailTitle_ = nullptr;
    QPointer<QWidget> detailContent_;
    QPointer<QWidget> pendingDetail_;
    QButtonGroup* tabs_ = nullptr;
    QToolButton* chatsTab_ = nullptr;
    QStackedWidget* networkViews_ = nullptr;
    NodeMap* map_ = nullptr;
    QPushButton* mapButton_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* radio_ = nullptr;
    QLabel* radioState_ = nullptr;
    QLabel* settingsName_ = nullptr;
    QLabel* settingsNodeAvatar_ = nullptr;
    QLabel* settingsStatus_ = nullptr;
    QLabel* settingsBatteryPercent_ = nullptr;
    QLabel* settingsBatteryVoltage_ = nullptr;
    QLabel* settingsReconnectText_ = nullptr;
    QLabel* emptySettings_ = nullptr;
    QScrollArea* settingsScroll_ = nullptr;
    QLineEdit* settingsSearch_ = nullptr;
    QToolButton* settingsSearchButton_ = nullptr;
    QWidget* settingsCard_ = nullptr;
    QWidget* settingsReconnectBanner_ = nullptr;
    RadioSettingsPanel* radioSettings_ = nullptr;
    QPushButton* settingsLink_ = nullptr;
    QPushButton* settingsAnnounce_ = nullptr;
    QPushButton* settingsRetryNow_ = nullptr;
    QPushButton* connectionBack_ = nullptr;
    QPushButton* detailBack_ = nullptr;
    QLineEdit* host_ = nullptr;
    QLineEdit* port_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QLabel* error_ = nullptr;
    QLineEdit* chatSearch_ = nullptr;
    QPushButton* clearChatSearch_ = nullptr;
    QPushButton* chatFilter_ = nullptr;
    int chatFilterIndex_ = 0;
    QLabel* chatCount_ = nullptr;
    QLabel* emptyChats_ = nullptr;
    QPushButton* connectFromChats_ = nullptr;
    QListWidget* targets_ = nullptr;
    QPointer<QListWidget> chooser_;
    QPointer<QLineEdit> chooserSearch_;
    QPointer<QLabel> chooserEmpty_;
    QPointer<QPushButton> chooserChannels_;
    int chooserKind_ = 0;
    QLineEdit* nodeSearch_ = nullptr;
    QLabel* nodeCount_ = nullptr;
    QLabel* emptyNodes_ = nullptr;
    QListWidget* nodes_ = nullptr;
    QString activeId_;
    QString activeTitle_;
    QString pendingTargetId_;
    QString pendingDetailTitle_;
    QHash<QString, QString> drafts_;
    QVariantMap pins_;
    int tab_ = Chats;
    int pendingTab_ = Chats;
    int unreadBadge_ = 0;
    int appearanceTheme_ = -1;
    int chatTextSize_ = Appearance::DefaultChatTextSize;
    bool appearanceDark_ = true;
    bool mapVisible_ = false;
    bool wasConnected_ = false;
    Navigation pendingNavigation_ = Navigation::None;
    ConnectionRequest connectionRequest_ = ConnectionRequest::None;
    bool viewUpdatePending_ = false;
    bool updating_ = false;
};

#endif
