#include "phone_location.hpp"

#include "client.hpp"
#include "demo_radio.hpp"
#include "native_location.hpp"
#include "phone_location_policy.hpp"

#include <QtCore/QEvent>
#include <QtCore/QSettings>
#include <QtGui/QApplication>

namespace {

constexpr int ScanIntervalMs = 60000;

QString text(const char* value) { return QString::fromUtf8(value); }

QString followSetting(const QString& key) { return QLatin1String("location/follow/") + key; }

QString currentKey(const Client& client) {
    return client.connected() && !demoRadioActive(client) ? client.selfNode().publicKey : QString();
}

} // namespace

PhoneLocation::PhoneLocation(Client& client, QObject* parent)
    : QObject(parent), client_(client), source_(new NativeLocation(this)),
      foreground_(QApplication::activeWindow() != nullptr) {
    clock_.start();
    next_.setSingleShot(true);
    qApp->installEventFilter(this);
    connect(&next_, SIGNAL(timeout()), this, SLOT(poll()));
    connect(&client_, SIGNAL(changed()), this, SLOT(refresh()), Qt::QueuedConnection);
    connect(&client_, SIGNAL(radioSettingsResult(int,bool,QString)),
            this, SLOT(written(int,bool,QString)), Qt::QueuedConnection);
    connect(source_, SIGNAL(fixReady(double,double,double)), this, SLOT(fixReady(double,double,double)));
    connect(source_, SIGNAL(fixFailed(int)), this, SLOT(fixFailed(int)));
    refresh();
}

bool PhoneLocation::available() const {
    return foreground_ && !key_.isEmpty() && session_ == client_.radioSessionToken() &&
           key_ == currentKey(client_);
}

bool PhoneLocation::following() const { return followEnabled_ || request_ == EnableFollow; }

bool PhoneLocation::radioGpsEnabled() const { return client_.radioGpsState() == RadioGpsState::Enabled; }

bool PhoneLocation::userRequestPending() const {
    return request_ == Manual || request_ == EnableFollow || writeRequest_;
}

QString PhoneLocation::message() const { return message_; }

bool PhoneLocation::eventFilter(QObject* watched, QEvent* event) {
    if (watched == qApp) {
        if (event->type() == QEvent::ApplicationActivate) {
            foreground_ = true;
            refresh();
            emit changed();
        } else if (event->type() == QEvent::ApplicationDeactivate) {
            foreground_ = false;
            next_.stop();
            cancelFix();
            emit changed();
        }
    }
    return QObject::eventFilter(watched, event);
}

void PhoneLocation::locate() {
    if (!available() || userRequestPending())
        return;
    begin(Manual);
}

void PhoneLocation::setFollowing(bool enabled) {
    if (!available())
        return;
    if (enabled) {
        if (!following() && !radioGpsEnabled() && !userRequestPending()) {
            client_.requestRadioGps();
            begin(EnableFollow);
        }
        return;
    }
    QSettings().remove(followSetting(key_));
    followEnabled_ = false;
    next_.stop();
    if (request_ == EnableFollow || request_ == Automatic)
        cancelFix();
    message_.clear();
    emit changed();
}

void PhoneLocation::cancelUserRequest() {
    if (request_ == Manual || request_ == EnableFollow) {
        cancelFix();
        refresh();
        emit changed();
    }
}

void PhoneLocation::refresh() {
    const QString key = currentKey(client_);
    const quint64 session = client_.radioSessionToken();
    if (session != session_ || key != key_) {
        next_.stop();
        cancelFix();
        writeRequest_ = 0;
        key_ = key;
        session_ = session;
        followEnabled_ = !key_.isEmpty() && QSettings().value(followSetting(key_), false).toBool();
        message_.clear();
        emit changed();
    }
    if (!available() || !followEnabled_ || radioGpsEnabled()) {
        next_.stop();
        if (request_ == Automatic)
            cancelFix();
        return;
    }
    if (request_ == Idle && !writeRequest_ && !next_.isActive())
        schedule(0);
}

void PhoneLocation::poll() {
    if (!available() || !followEnabled_ || request_ != Idle || writeRequest_ || radioGpsEnabled())
        return;
    if (client_.radioGpsState() == RadioGpsState::Unknown) {
        client_.requestRadioGps();
        schedule(PhoneLocationPolicy::RetryDelayMs);
        return;
    }
    const qint64 last = lastWrite_.value(key_, -1);
    const qint64 remaining = PhoneLocationPolicy::MinimumWritePeriodMs - (clock_.elapsed() - last);
    if (last >= 0 && remaining > 0) {
        schedule(static_cast<int>(remaining));
        return;
    }
    begin(Automatic);
}

void PhoneLocation::begin(Request request) {
    next_.stop();
    cancelFix();
    request_ = request;
    if (request_ != Automatic)
        message_ = text("Ищу местоположение телефона…");
    emit changed();
    source_->requestFix();
}

void PhoneLocation::cancelFix() {
    if (request_ == Idle)
        return;
    const bool user = request_ != Automatic;
    request_ = Idle;
    source_->cancel();
    if (user)
        message_.clear();
}

void PhoneLocation::schedule(int delayMs) {
    if (available() && followEnabled_ && !radioGpsEnabled())
        next_.start(delayMs);
}

void PhoneLocation::fixReady(double latitude, double longitude, double accuracy) {
    using namespace PhoneLocationPolicy;
    if (request_ == Idle || !available()) {
        refresh();
        return;
    }
    const Fix fix{latitude, longitude, accuracy};
    if (!usableFix(fix)) {
        fixFailed(-6);
        return;
    }
    const Request source = request_;
    request_ = Idle;
    if (source == EnableFollow) {
        if (radioGpsEnabled()) {
            message_ = text("Нода использует свой GPS.");
            emit changed();
            return;
        }
        QSettings().setValue(followSetting(key_), true);
        followEnabled_ = true;
        message_.clear();
    }

    const qint32 lat = static_cast<qint32>(qRound64(latitude * 1000000.0));
    const qint32 lon = static_cast<qint32>(qRound64(longitude * 1000000.0));
    if (source == Manual) {
        writePosition(lat, lon, true);
    } else if (client_.radioGpsState() == RadioGpsState::Unknown) {
        schedule(0);
    } else if (!radioGpsEnabled() && followEnabled_) {
        const auto& radio = client_.radioIdentity();
        const qint64 last = lastWrite_.value(key_, -1);
        const Decision decision = followStep(fix, radio.latitude, radio.longitude,
                                             last < 0 ? -1 : clock_.elapsed() - last);
        if (decision == Decision::Write && !client_.busy()) {
            writePosition(lat, lon, false);
        } else {
            if (decision == Decision::Rough || decision == Decision::Invalid)
                message_ = text("Жду более точные координаты GPS.");
            else
                message_.clear();
            schedule(decision == Decision::Write ? RetryDelayMs : ScanIntervalMs);
        }
    }
    emit changed();
}

void PhoneLocation::fixFailed(int error) {
    if (request_ == Idle)
        return;
    request_ = Idle;
    if (!available()) {
        refresh();
        return;
    }
    if (error == -46 || error == -21)
        message_ = text("Доступ к GPS запрещён системными настройками.");
    else if (error == -5 || error == -1 || error == -18)
        message_ = text("GPS недоступен. Проверьте методы определения местоположения в телефоне.");
    else if (error == -33)
        message_ = text("GPS не успел найти координаты. Попробуйте под открытым небом.");
    else
        message_ = text("Не удалось получить координаты телефона (%1).").arg(error);
    schedule(ScanIntervalMs);
    emit changed();
}

void PhoneLocation::writePosition(qint32 latitude, qint32 longitude, bool manual) {
    const auto& radio = client_.radioIdentity();
    if (radio.latitude == latitude && radio.longitude == longitude) {
        if (manual) {
            message_ = text("Эти координаты уже сохранены на ноде.");
            emit positionWritten(true);
        }
        schedule(ScanIntervalMs);
        return;
    }
    const int id = client_.nextConsoleRequestId();
    if (client_.applyRadioSettings(id, {meshcore::setLocation(latitude, longitude)})) {
        writeRequest_ = id;
        manualWrite_ = manual;
        // Even a lost acknowledgement can mean the radio has written to flash.
        lastWrite_.insert(key_, clock_.elapsed());
        message_ = manual ? text("Записываю координаты на ноду…") : QString();
    } else {
        if (manual)
            message_ = text("Нода занята. Попробуйте ещё раз.");
        schedule(PhoneLocationPolicy::RetryDelayMs);
    }
}

void PhoneLocation::written(int requestId, bool success, const QString& status) {
    if (requestId != writeRequest_)
        return;
    const bool manual = manualWrite_;
    writeRequest_ = 0;
    if (client_.radioSessionToken() != session_ || currentKey(client_) != key_) {
        refresh();
        return;
    }
    message_ = success ? text("Координаты телефона сохранены на ноде.") : status;
    if (success)
        emit positionWritten(manual);
    schedule(PhoneLocationPolicy::MinimumWritePeriodMs);
    emit changed();
}
