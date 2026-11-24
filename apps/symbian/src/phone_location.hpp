#ifndef OMMESH_BELLE_PHONE_LOCATION_HPP
#define OMMESH_BELLE_PHONE_LOCATION_HPP

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>

class Client;
class NativeLocation;

class PhoneLocation : public QObject {
    Q_OBJECT

public:
    explicit PhoneLocation(Client& client, QObject* parent);
    bool available() const;
    bool following() const;
    bool radioGpsEnabled() const;
    bool userRequestPending() const;
    QString message() const;
    void locate();
    void setFollowing(bool enabled);
    void cancelUserRequest();

signals:
    void changed();
    void positionWritten(bool manual);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void refresh();
    void poll();
    void fixReady(double latitude, double longitude, double accuracy);
    void fixFailed(int error);
    void written(int requestId, bool success, const QString& status);

private:
    enum Request { Idle, Manual, EnableFollow, Automatic };
    void begin(Request request);
    void cancelFix();
    void schedule(int delayMs);
    void writePosition(qint32 latitude, qint32 longitude, bool manual);

    Client& client_;
    NativeLocation* source_;
    QTimer next_;
    QElapsedTimer clock_;
    QHash<QString, qint64> lastWrite_;
    QString key_;
    QString message_;
    quint64 session_ = 0;
    bool foreground_ = false;
    bool followEnabled_ = false;
    Request request_ = Idle;
    int writeRequest_ = 0;
    bool manualWrite_ = false;
};

#endif
