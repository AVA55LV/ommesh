#include "native_wlan.hpp"

#include <QtCore/QSettings>
#include <QtCore/QVariant>
#include <QtNetwork/QNetworkConfiguration>
#include <QtNetwork/QNetworkConfigurationManager>
#include <QtNetwork/QNetworkSession>
#include <QtNetwork/QTcpSocket>

// Qt 4.8's Symbian socket engine reads this exact private metatype.
Q_DECLARE_METATYPE(QSharedPointer<QNetworkSession>)

namespace {

const char kSocketSession[] = "_q_networksession";
const char kWlanId[] = "network/wlanId";
const char kWlanName[] = "network/wlanName";

bool isSavedWlan(const QNetworkConfiguration& config) {
    return config.isValid() && config.type() == QNetworkConfiguration::InternetAccessPoint &&
           config.bearerType() == QNetworkConfiguration::BearerWLAN &&
           config.state().testFlag(QNetworkConfiguration::Defined) &&
           !config.identifier().isEmpty() && !config.name().isEmpty();
}

QNetworkConfiguration chooseWlan(const QList<QNetworkConfiguration>& configs) {
    const QSettings settings;
    const QString savedId = settings.value(QLatin1String(kWlanId)).toString();
    const QString savedName = settings.value(QLatin1String(kWlanName)).toString();
    QNetworkConfiguration saved;
    QNetworkConfiguration active;
    int activeCount = 0;
    for (const auto& config : configs) {
        if (!isSavedWlan(config))
            continue;
        const bool matches = config.identifier() == savedId && config.name() == savedName;
        if (matches)
            saved = config;
        if (config.state().testFlag(QNetworkConfiguration::Active)) {
            if (matches)
                return config;
            active = config;
            ++activeCount;
        }
    }
    return activeCount == 1 ? active : saved;
}

void rememberWlan(const QString& id, const QString& name) {
    QSettings settings;
    settings.setValue(QLatin1String(kWlanId), id);
    settings.setValue(QLatin1String(kWlanName), name);
}

}  // namespace

NativeWlan::NativeWlan(QTcpSocket* socket, QObject* parent)
    : QObject(parent), socket_(socket) {}

NativeWlan::~NativeWlan() = default;

QNetworkConfigurationManager& NativeWlan::configurations() {
    if (!configurations_)
        configurations_ = new QNetworkConfigurationManager(this);
    return *configurations_;
}

void NativeWlan::rememberActive() {
    const auto active = chooseWlan(configurations().allConfigurations(QNetworkConfiguration::Active));
    if (active.isValid())
        rememberWlan(active.identifier(), active.name());
}

void NativeWlan::open() {
    reset();
    const auto config = chooseWlan(configurations().allConfigurations(QNetworkConfiguration::Defined));
    if (!config.isValid()) {
        emit failed(QString::fromUtf8("Откройте сохранённую сеть Wi-Fi в настройках телефона"));
        return;
    }

    selectedId_ = config.identifier();
    selectedName_ = config.name();
    session_ = QSharedPointer<QNetworkSession>(new QNetworkSession(config));
    session_->setSessionProperty(QLatin1String("ConnectInBackground"), true);
    qRegisterMetaType<QNetworkSession::SessionError>();
    connect(session_.data(), SIGNAL(opened()), this, SLOT(sessionOpened()), Qt::QueuedConnection);
    connect(session_.data(), SIGNAL(closed()), this, SLOT(sessionFailed()), Qt::QueuedConnection);
    connect(session_.data(), SIGNAL(error(QNetworkSession::SessionError)),
            this, SLOT(sessionFailed()), Qt::QueuedConnection);
    session_->open();
}

void NativeWlan::reset() {
    if (!session_)
        return;
    socket_->setProperty(kSocketSession, QVariant());
    disconnect(session_.data(), nullptr, this, nullptr);
    session_->close();
    session_.clear();
    selectedId_.clear();
    selectedName_.clear();
}

void NativeWlan::sessionOpened() {
    if (!session_ || sender() != session_.data())
        return;
    const QString activeId = session_->sessionProperty(QLatin1String("ActiveConfiguration")).toString();
    const auto active = configurations().configurationFromIdentifier(activeId);
    if (!session_->isOpen() || activeId != selectedId_ || !isSavedWlan(active) ||
            active.name() != selectedName_) {
        emit failed(QString::fromUtf8("Телефон не подтвердил подключение к выбранной сети Wi-Fi"));
        return;
    }

    socket_->setProperty(kSocketSession, QVariant::fromValue(session_));
    if (socket_->property(kSocketSession).value<QSharedPointer<QNetworkSession>>() != session_) {
        emit failed(QString::fromUtf8("Приложение не смогло привязать соединение к Wi-Fi"));
        return;
    }
    rememberWlan(activeId, selectedName_);
    emit ready();
}

void NativeWlan::sessionFailed() {
    if (session_ && sender() == session_.data())
        emit failed(QString::fromUtf8("Сохранённая сеть Wi-Fi недоступна"));
}
