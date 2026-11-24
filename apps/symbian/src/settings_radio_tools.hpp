#ifndef BELLE_SETTINGS_RADIO_TOOLS_HPP
#define BELLE_SETTINGS_RADIO_TOOLS_HPP

#include <QtCore/QElapsedTimer>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtGui/QWidget>

class Client;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

class RadioAirPanel : public QWidget {
    Q_OBJECT

public:
    explicit RadioAirPanel(Client& client, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void receivePacket(int snr, int rssi, int type, int hops, int bytes, int route);
    void refreshView();
    void poll();
    void toggleListening();
    void clearLog();

private:
    struct Packet {
        QString time;
        int snr;
        int rssi;
        int type;
        int hops;
        int bytes;
        int route;
    };

    Client& client_;
    QLabel* status_;
    QLabel* empty_;
    QListWidget* packets_;
    QPushButton* pause_;
    QPushButton* clear_;
    QTimer* timer_;
    QElapsedTimer lastReadings_;
    QVector<Packet> pending_;
    bool paused_ = false;
};

class RadioMaintenancePanel : public QWidget {
    Q_OBJECT

public:
    explicit RadioMaintenancePanel(Client& client, QWidget* parent = nullptr);

protected:
    void hideEvent(QHideEvent* event) override;

private slots:
    void askReboot();
    void askReset();
    void askErase();
    void cancel();
    void confirm();
    void updateAvailability();

private:
    enum Action { None, Reboot, FactoryReset, EraseHistory };
    void ask(Action action);

    Client& client_;
    QLabel* status_;
    QLabel* explanation_;
    QPushButton* reboot_;
    QPushButton* reset_;
    QPushButton* erase_;
    QPushButton* confirm_;
    QWidget* confirmation_;
    QString targetKey_;
    Action pending_ = None;
};

#endif
