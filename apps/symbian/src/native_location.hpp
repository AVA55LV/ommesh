#pragma once

#include <QtCore/QObject>
#include <QtCore/QTimer>

class NativeLocation : public QObject {
    Q_OBJECT
public:
    explicit NativeLocation(QObject* parent = 0);
    ~NativeLocation() override;

    void requestFix();
    void cancel();
    bool isRequesting() const;

signals:
    void fixReady(double latitude, double longitude, double horizontalAccuracy);
    void fixFailed(int error);

private slots:
    void timerElapsed();

private:
    class Backend;
    enum Phase { Idle, Starting, Waiting };

    void complete(int error, double latitude = 0, double longitude = 0,
                  double horizontalAccuracy = -1);

    Backend* backend_ = nullptr;
    QTimer timer_;
    Phase phase_ = Idle;
};
