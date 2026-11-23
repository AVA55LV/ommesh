#ifndef OMMESH_NATIVE_WLAN_HPP
#define OMMESH_NATIVE_WLAN_HPP

#include <QtCore/QObject>
#include <QtCore/QSharedPointer>
#include <QtCore/QString>

class QNetworkConfigurationManager;
class QNetworkSession;
class QTcpSocket;

class NativeWlan : public QObject {
    Q_OBJECT

public:
    explicit NativeWlan(QTcpSocket* socket, QObject* parent = nullptr);
    ~NativeWlan() override;

    void rememberActive();
    void open();
    // Call after aborting the TCP socket, including on a failed open.
    void reset();

signals:
    void ready();
    void failed(const QString& reason);

private slots:
    void sessionOpened();
    void sessionFailed();

private:
    QNetworkConfigurationManager& configurations();

    QTcpSocket* socket_;
    QNetworkConfigurationManager* configurations_ = nullptr;
    QSharedPointer<QNetworkSession> session_;
    QString selectedId_;
    QString selectedName_;
};

#endif
