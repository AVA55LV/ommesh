#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QImage>

namespace LinkPreview {

enum Mode { Off, OnTap, Automatic };
enum State { Missing, Loading, Ready, Empty, Failed };

struct Link {
    int start;
    int length;
    QUrl address;
    bool previewable;
};

struct Card {
    State state = Missing;
    QString host;
    QString title;
    QString description;
    QImage image;
};

Mode preference();
QList<Link> linksIn(const QString& text);
QUrl previewAddress(QUrl url);
Card parseAnswer(const QByteArray& json);

class Worker;

class Store : public QObject {
    Q_OBJECT

public:
    explicit Store(QObject* parent);
    ~Store() override;
    Mode mode() const { return mode_; }
    void setMode(Mode mode);
    void setVisible(const QList<QUrl>& addresses);
    Card card(const QUrl& address) const;
    void request(const QUrl& address);

signals:
    void changed();

private slots:
    void pump();
    void completed();

private:
    struct Stored {
        Card card;
        qint64 expires;
    };
    void schedule(int delay = 0);
    void remember(const QUrl& address, const Card& card, qint64 expires);

    Mode mode_ = Off;
    QList<QUrl> visible_;
    QList<QUrl> manual_;
    QList<QUrl> recent_;
    QList<qint64> autoTimes_;
    QHash<QUrl, Stored> cards_;
    QTimer timer_;
    Worker* worker_ = nullptr;
};

} // namespace LinkPreview
