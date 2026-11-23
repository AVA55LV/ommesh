#ifndef OMMESH_BELLE_NETWORK_NODE_CARD_HPP
#define OMMESH_BELLE_NETWORK_NODE_CARD_HPP

#include "client.hpp"

#include <QtGui/QWidget>

class QLabel;
class QPushButton;

class NetworkNodeCard : public QWidget {
    Q_OBJECT

public:
    NetworkNodeCard(Client& client, const NetworkNode& node, bool favorite, QWidget* parent = nullptr);

signals:
    void favoriteChanged(bool favorite);
    void chatRequested(const QString& id);
    void showOnMap(const QString& id);
    void managementRequested(const QString& id, int page);

private slots:
    void openChat();
    void locateNode();
    void openSignIn();
    void signInAgain();
    void signOut();
    void openConsole();
    void openMore();
    void runRemoteOption();
    void runAction();
    void checkRoute();
    void copyKey();
    void copyName();
    void copyCoordinates();
    void refreshState();
    void managementResult(int request, int operation, bool success, const QByteArray& data, const QString& error);
    void consoleResult(int request, int operation, bool success, const QString& text);

private:
    void copyText(const QString& text, const QString& confirmation);
    void showFeedback(const QString& message);
    void openRoute();
    void requestRouteCheck(bool direct);
    void requestReadings();
    void showReadingsHistory();
    void updateChatOptions();
    void editChatOption(int action);

    struct ManagementRow {
        QPushButton* button;
        QLabel* status;
        bool admin;
    };

    Client& client_;
    NetworkNode node_;
    QString coordinates_;
    QLabel* feedback_ = nullptr;
    QLabel* routeValue_ = nullptr;
    QLabel* routeFeedback_ = nullptr;
    QLabel* managementHint_ = nullptr;
    QLabel* notifications_ = nullptr;
    QLabel* sound_ = nullptr;
    QLabel* background_ = nullptr;
    QLabel* readingsValue_ = nullptr;
    QWidget* readings_ = nullptr;
    QPushButton* signIn_ = nullptr;
    QPushButton* routeCheck_ = nullptr;
    QPushButton* readingButton_ = nullptr;
    QVector<ManagementRow> management_;
    int managementRequest_ = 0;
    int commandRequest_ = 0;
};

#endif
