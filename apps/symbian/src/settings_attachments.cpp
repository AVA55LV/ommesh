#include "settings_attachments.hpp"

#include "native_echo.hpp"
#include "native_tile_https.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QRegExp>
#include <QtCore/QSettings>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtGui/QCheckBox>
#include <QtGui/QComboBox>
#include <QtGui/QDesktopServices>
#include <QtGui/QFileDialog>
#include <QtGui/QHBoxLayout>
#include <QtGui/QLabel>
#include <QtGui/QLineEdit>
#include <QtGui/QPushButton>
#include <QtGui/QVBoxLayout>
#include <QtNetwork/QHostAddress>
#ifndef Q_OS_SYMBIAN
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>
#endif

#include <algorithm>
#include <utility>

namespace {

constexpr int MaxFileBytes = 512 * 1024;
constexpr int MaxCaBytes = 64 * 1024;
constexpr int TimeoutMs = 75000;
constexpr char Scheme[] = "https";
constexpr char Organization[] = "OMMeshBelle";
constexpr char SettingsFile[] = "S3-Private";

enum Addressing { AutoAddress, PathAddress, BucketAddress };

struct Settings {
    bool enabled = false;
    QString endpoint;
    QString bucket;
    QString region;
    QString access;
    QString secret;
    QString publicBase;
    QString certificate;
    Addressing addressing = AutoAddress;
};

struct Target {
    QByteArray authority;
    QByteArray path;
};

QString ui(const char* text) { return QString::fromUtf8(text); }

Settings storedSettings() {
    QSettings preferences(QSettings::NativeFormat, QSettings::UserScope,
        QLatin1String(Organization), QLatin1String(SettingsFile));
    preferences.setFallbacksEnabled(false);
    Settings s;
    s.enabled = preferences.value(QLatin1String("enabled"), false).toBool();
    s.endpoint = preferences.value(QLatin1String("endpoint")).toString();
    s.bucket = preferences.value(QLatin1String("bucket")).toString();
    s.region = preferences.value(QLatin1String("region")).toString();
    s.access = preferences.value(QLatin1String("access")).toString();
    s.secret = preferences.value(QLatin1String("secret")).toString();
    s.publicBase = preferences.value(QLatin1String("publicBase")).toString();
    s.certificate = preferences.value(QLatin1String("certificate")).toString();
    s.addressing = static_cast<Addressing>(qBound(int(AutoAddress),
        preferences.value(QLatin1String("addressing"), int(AutoAddress)).toInt(), int(BucketAddress)));
    return s;
}

bool storeSettings(const Settings& s) {
    // Separate private settings file, outside the capabilities of application backup/export.
    QSettings preferences(QSettings::NativeFormat, QSettings::UserScope,
        QLatin1String(Organization), QLatin1String(SettingsFile));
    preferences.setFallbacksEnabled(false);
    preferences.setValue(QLatin1String("enabled"), s.enabled);
    preferences.setValue(QLatin1String("endpoint"), s.endpoint);
    preferences.setValue(QLatin1String("bucket"), s.bucket);
    preferences.setValue(QLatin1String("region"), s.region);
    preferences.setValue(QLatin1String("publicBase"), s.publicBase);
    preferences.setValue(QLatin1String("certificate"), s.certificate);
    preferences.setValue(QLatin1String("addressing"), int(s.addressing));
    if (s.access.isEmpty())
        preferences.remove(QLatin1String("access"));
    else
        preferences.setValue(QLatin1String("access"), s.access);
    if (s.secret.isEmpty())
        preferences.remove(QLatin1String("secret"));
    else
        preferences.setValue(QLatin1String("secret"), s.secret);
    preferences.sync();
    return preferences.status() == QSettings::NoError;
}

QUrl httpsUrl(const QString& value) {
    const QUrl url(value, QUrl::StrictMode);
    const int port = url.port(443);
    const QByteArray host = QUrl::toAce(url.host()).toLower();
    const QRegExp domain(QLatin1String("^[a-z0-9][a-z0-9.-]{0,252}$"));
    if (value.size() > 512 || !url.isValid() || url.scheme() != QLatin1String(Scheme)
            || !domain.exactMatch(QString::fromLatin1(host.constData())) || port < 1 || port > 65535
            || !url.userName().isEmpty() || !url.password().isEmpty() || url.hasFragment()
            || url.hasQuery() || url.authority().contains(QLatin1Char('@')))
        return QUrl();
    return url;
}

bool matches(const QString& value, const char* pattern) {
    return QRegExp(QLatin1String(pattern)).exactMatch(value);
}

QString settingsError(const Settings& s) {
    const QUrl endpoint = httpsUrl(s.endpoint);
    if (endpoint.isEmpty() || (!endpoint.path().isEmpty() && endpoint.path() != QLatin1String("/")))
        return ui("Введите HTTPS-адрес S3 без пути, параметров и учётных данных.");
    if (!matches(s.bucket, "^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$"))
        return ui("Введите имя бакета из 3–63 строчных букв, цифр, точек и дефисов.");
    if (!matches(s.region, "^[a-z0-9][a-z0-9-]{0,31}$"))
        return ui("Укажите регион S3.");
    if (!matches(s.access, "^[!-~]{1,128}$") || !matches(s.secret, "^[!-~]{1,256}$"))
        return ui("Укажите ключ доступа и секретный ключ S3.");
    if (!s.publicBase.isEmpty() && httpsUrl(s.publicBase).isEmpty())
        return ui("Публичный адрес должен быть HTTPS-ссылкой без параметров.");
    const QHostAddress ip(endpoint.host());
    if (s.addressing == BucketAddress && !ip.isNull())
        return ui("Для IP-адреса используйте бакет в пути.");
    return QString();
}

QByteArray authority(const QUrl& url) {
    QByteArray value = QUrl::toAce(url.host()).toLower();
    if (url.port(443) != 443)
        value += ':' + QByteArray::number(url.port());
    return value;
}

Target uploadTarget(const Settings& s, const QByteArray& key) {
    const QUrl endpoint = httpsUrl(s.endpoint);
    const QByteArray bucket = s.bucket.toLatin1();
    const bool pathStyle = s.addressing == PathAddress ||
        (s.addressing == AutoAddress && (endpoint.port() >= 0 || !QHostAddress(endpoint.host()).isNull()
                                        || bucket.contains('.')));
    return pathStyle ? Target{authority(endpoint), '/' + bucket + '/' + key}
                     : Target{bucket + '.' + authority(endpoint), '/' + key};
}

QByteArray hash(const QByteArray& bytes) {
    const auto digest = meshcore::sha256Digest({reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                               static_cast<std::size_t>(bytes.size())});
    return QByteArray(reinterpret_cast<const char*>(digest.data()), static_cast<int>(digest.size()));
}

QByteArray hmac(QByteArray key, const QByteArray& data) {
    if (key.size() > 64)
        key = hash(key);
    QByteArray inner(64, '\x36');
    QByteArray outer(64, '\x5c');
    for (int i = 0; i < key.size(); ++i) {
        inner[i] = static_cast<char>(static_cast<unsigned char>(inner.at(i)) ^ static_cast<unsigned char>(key.at(i)));
        outer[i] = static_cast<char>(static_cast<unsigned char>(outer.at(i)) ^ static_cast<unsigned char>(key.at(i)));
    }
    inner.append(data);
    outer.append(hash(inner));
    key.fill('\0');
    inner.fill('\0');
    const QByteArray digest = hash(outer);
    outer.fill('\0');
    return digest;
}

QByteArray escape(const QByteArray& data) {
    return QUrl::toPercentEncoding(QString::fromLatin1(data.constData(), data.size()));
}

QUrl signedPut(const Settings& s, const QByteArray& key, const QDateTime& time) {
    const Target target = uploadTarget(s, key);
    const QByteArray stamp = time.toUTC().toString(QLatin1String("yyyyMMdd'T'hhmmss'Z'")).toLatin1();
    const QByteArray date = stamp.left(8);
    const QByteArray region = s.region.toLatin1();
    const QByteArray scope = date + '/' + region + "/s3/aws4_request";
    const QByteArray query = "X-Amz-Algorithm=AWS4-HMAC-SHA256"
        "&X-Amz-Content-Sha256=UNSIGNED-PAYLOAD&X-Amz-Credential=" + escape(s.access.toLatin1() + '/' + scope)
        + "&X-Amz-Date=" + stamp + "&X-Amz-Expires=900&X-Amz-SignedHeaders=host";
    const QByteArray request = "PUT\n" + target.path + '\n' + query + "\nhost:" + target.authority
        + "\n\nhost\nUNSIGNED-PAYLOAD";
    const QByteArray input = "AWS4-HMAC-SHA256\n" + stamp + '\n' + scope + '\n' + hash(request).toHex();
    QByteArray signingKey = hmac("AWS4" + s.secret.toLatin1(), date);
    signingKey = hmac(signingKey, region);
    signingKey = hmac(signingKey, "s3");
    signingKey = hmac(signingKey, "aws4_request");
    const QByteArray signature = hmac(signingKey, input).toHex();
    signingKey.fill('\0');
    return QUrl::fromEncoded("https://" + target.authority + target.path + '?' + query
        + "&X-Amz-Signature=" + signature, QUrl::StrictMode);
}

QUrl publicLink(const Settings& s, const QByteArray& key) {
    const bool custom = !s.publicBase.isEmpty();
    QByteArray base = httpsUrl(custom ? s.publicBase : s.endpoint).toEncoded();
    while (base.endsWith('/'))
        base.chop(1);
    if (!custom)
        base += '/' + s.bucket.toLatin1();
    return QUrl::fromEncoded(base + '/' + key, QUrl::StrictMode);
}

QByteArray freshId() {
    QByteArray id = QUuid::createUuid().toString().toLatin1();
    id.remove(0, 1);
    id.replace("-", "");
    return id.left(12);
}

struct FileKind {
    QByteArray category;
    QByteArray extension;
    QByteArray type;
};

FileKind fileKind(const QString& path) {
    QString extension = QFileInfo(path).suffix().toLower();
    if (!matches(extension, "^[a-z0-9]{1,8}$"))
        extension = QLatin1String("bin");
    if (extension == QLatin1String("jpg") || extension == QLatin1String("jpeg"))
        return {"p", "jpg", "image/jpeg"};
    if (extension == QLatin1String("png"))
        return {"p", "png", "image/png"};
    if (extension == QLatin1String("pdf"))
        return {"f", "pdf", "application/pdf"};
    if (extension == QLatin1String("txt"))
        return {"f", "txt", "text/plain"};
    if (extension == QLatin1String("zip"))
        return {"f", "zip", "application/zip"};
    return {"f", extension.toLatin1(), "application/octet-stream"};
}

QByteArray selectedCa(const QString& path) {
    if (path.isEmpty())
        return QByteArray();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxCaBytes)
        return QByteArray();
    const QByteArray bytes = file.read(MaxCaBytes + 1);
    return bytes.size() <= MaxCaBytes && bytes.contains("-----BEGIN CERTIFICATE-----") &&
        bytes.contains("-----END CERTIFICATE-----") && !bytes.contains("PRIVATE KEY-----")
        ? bytes : QByteArray();
}

QString transferError(int code) {
    switch (code) {
    case -5: return ui("Не удалось прочитать файл.");
    case -4: return ui("Загрузка отменена.");
    case -3: return ui("Сервер не ответил вовремя.");
    case -2: return ui("Не удалось проверить HTTPS-сертификат. Проверьте дату телефона и файл CA.");
    case 401: case 403: return ui("Доступ запрещён. Проверьте ключи, регион и разрешения бакета S3.");
    default: return code > 0 ? ui("Сервер вернул HTTP %1. Проверьте адрес, бакет и регион.").arg(code)
                             : ui("Не удалось подключиться к серверу через интернет.");
    }
}

#ifndef Q_OS_SYMBIAN
int transferAttachment(const QUrl& address, bool put, const QByteArray& contentType, const QByteArray& disposition,
                       const QByteArray& body, const QByteArray& customCa, const QAtomicInt* cancelled) {
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest request(address);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    if (put) {
        request.setRawHeader("Content-Type", contentType);
        request.setRawHeader("Content-Disposition", disposition);
    }
    QSslConfiguration tls = request.sslConfiguration();
    if (!customCa.isEmpty()) {
        const auto roots = QSslCertificate::fromData(customCa, QSsl::Pem);
        if (roots.isEmpty())
            return -2;
        tls.setCaCertificates(tls.caCertificates() + roots);
    }
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(tls);
    QNetworkReply* reply = put ? network.put(request, body) : network.get(request);
    reply->setReadBufferSize(4096);
    QEventLoop wait;
    QTimer pulse;
    QObject::connect(&pulse, SIGNAL(timeout()), &wait, SLOT(quit()));
    QObject::connect(reply, SIGNAL(metaDataChanged()), &wait, SLOT(quit()));
    QObject::connect(reply, SIGNAL(finished()), &wait, SLOT(quit()));
    pulse.start(100);
    QElapsedTimer timer;
    timer.start();
    while (const_cast<QAtomicInt*>(cancelled)->fetchAndAddAcquire(0) == 0 && timer.elapsed() < TimeoutMs) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status > 0)
            return status;
        if (reply->isFinished())
            return reply->error() == QNetworkReply::SslHandshakeFailedError ? -2 : -1;
        wait.exec();
    }
    reply->abort();
    return timer.elapsed() >= TimeoutMs ? -3 : -4;
}
#endif

class Worker final : public QThread {
public:
    Worker(QUrl putAddress, QUrl readAddress, QString path, FileKind kind, QByteArray ca, bool test)
        : putAddress_(std::move(putAddress)), readAddress_(std::move(readAddress)), path_(std::move(path)),
          kind_(std::move(kind)), ca_(std::move(ca)), test_(test) {
        setStackSize(96 * 1024);
    }

    void cancel() { cancellation_.fetchAndStoreRelease(1); }

    QUrl readAddress() const { return readAddress_; }
    int status = 0;
    int publicStatus = 0;

protected:
    void run() override {
        QByteArray bytes("ommesh\n");
        QString name = QLatin1String("ommesh-test.txt");
        if (!test_) {
            QFile file(path_);
            if (!file.open(QIODevice::ReadOnly) || file.size() > MaxFileBytes) {
                status = -5;
                return;
            }
            bytes = file.read(MaxFileBytes + 1);
            if (bytes.size() != file.size() || bytes.size() > MaxFileBytes || file.error() != QFile::NoError) {
                status = -5;
                return;
            }
            name = QFileInfo(path_).fileName().left(120);
        }
        const QByteArray filename = QUrl::toPercentEncoding(name);
        const QByteArray disposition = "inline; filename*=UTF-8''" + filename;
        status = transferAttachment(putAddress_, true, kind_.type, disposition, bytes, ca_, &cancellation_);
        // The presigned address expires soon and is only ever used for this PUT.
        putAddress_ = QUrl();
        if (test_ && status >= 200 && status < 300)
            publicStatus = transferAttachment(readAddress_, false, {}, {}, {}, ca_, &cancellation_);
    }

private:
    QUrl putAddress_;
    QUrl readAddress_;
    QString path_;
    FileKind kind_;
    QByteArray ca_;
    bool test_;
    QAtomicInt cancellation_{0};
};

class TransferRegistry final : public QObject {
public:
    explicit TransferRegistry(QObject* parent) : QObject(parent) {
        setObjectName(QLatin1String("ommeshAttachmentTransfer"));
    }

    ~TransferRegistry() override {
        if (worker)
            worker->cancel();
    }

    QPointer<Worker> worker;
};

TransferRegistry* transferRegistry(bool create) {
    QObject* app = QCoreApplication::instance();
    if (!app)
        return nullptr;
    auto* registry = static_cast<TransferRegistry*>(
        app->findChild<QObject*>(QLatin1String("ommeshAttachmentTransfer")));
    return registry || !create ? registry : new TransferRegistry(app);
}

bool transferBusy() {
    const auto* registry = transferRegistry(false);
    return registry && registry->worker;
}

void cancelWorker(Worker*& worker, QObject* receiver) {
    if (!worker)
        return;
    worker->disconnect(receiver);
    worker->cancel();
    if (worker->isFinished())
        worker->deleteLater();
    worker = nullptr;
}

Worker* startWorker(Worker* worker, QObject* receiver) {
    transferRegistry(true)->worker = worker;
    QObject::connect(worker, SIGNAL(finished()), receiver, SLOT(transferFinished()), Qt::QueuedConnection);
    QObject::connect(worker, SIGNAL(finished()), worker, SLOT(deleteLater()), Qt::QueuedConnection);
    worker->start();
    return worker;
}

QLabel* label(const QString& title, QWidget* parent, const char* style = "muted") {
    auto* result = new QLabel(title, parent);
    result->setObjectName(QLatin1String(style));
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return result;
}

QPushButton* button(const QString& title, QWidget* parent, const char* name) {
    auto* result = new QPushButton(title, parent);
    result->setObjectName(QLatin1String(name));
    result->setMinimumHeight(42);
    return result;
}

void selectFile(QWidget* parent, const QString& title, const QString& filters, const char* slot) {
    QString initial = QDesktopServices::storageLocation(QDesktopServices::DocumentsLocation);
    if (initial.isEmpty())
        initial = QDir::homePath();
    auto* picker = new QFileDialog(parent, title, initial, filters);
    picker->setFileMode(QFileDialog::ExistingFile);
    picker->setAcceptMode(QFileDialog::AcceptOpen);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(picker, SIGNAL(fileSelected(QString)), parent, slot, Qt::QueuedConnection);
    picker->open();
}

}  // namespace

namespace Attachments {

bool ready() {
    const Settings s = storedSettings();
    return s.enabled && settingsError(s).isEmpty();
}

QString statusText() {
    const Settings s = storedSettings();
    if (!s.enabled)
        return ui("Выкл");
    if (!settingsError(s).isEmpty())
        return ui("Настроить S3");
    return ui("Вкл");
}

}  // namespace Attachments

struct AttachmentSettingsPanel::State {
    QCheckBox* enabled = nullptr;
    QLineEdit* endpoint = nullptr;
    QLineEdit* bucket = nullptr;
    QLineEdit* region = nullptr;
    QLineEdit* access = nullptr;
    QLineEdit* secret = nullptr;
    QLineEdit* publicBase = nullptr;
    QLineEdit* certificate = nullptr;
    QComboBox* addressing = nullptr;
    QPushButton* save = nullptr;
    QPushButton* test = nullptr;
    QLabel* status = nullptr;
    Worker* worker = nullptr;

    Settings read() const {
        Settings s;
        s.enabled = enabled->isChecked();
        s.endpoint = endpoint->text().trimmed();
        s.bucket = bucket->text().trimmed();
        s.region = region->text().trimmed();
        s.access = access->text().trimmed();
        s.secret = secret->text().trimmed();
        s.publicBase = publicBase->text().trimmed();
        s.certificate = certificate->text().trimmed();
        s.addressing = static_cast<Addressing>(addressing->currentIndex());
        return s;
    }

    void statusMessage(const QString& message) {
        status->setText(message);
        status->setVisible(!message.isEmpty());
    }
};

AttachmentSettingsPanel::AttachmentSettingsPanel(QWidget* parent)
    : QWidget(parent), state_(new State) {
    State& s = *state_;
    setObjectName(QLatin1String("attachmentSettingsPanel"));
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(8, 6, 8, 9);
    page->setSpacing(6);

    const Settings values = storedSettings();
    s.enabled = new QCheckBox(ui("Включить вложения"), this);
    s.enabled->setObjectName(QLatin1String("attachmentsEnabled"));
    s.enabled->setMinimumHeight(43);
    s.enabled->setChecked(values.enabled);
    page->addWidget(s.enabled);
    page->addWidget(label(ui("Файл до 512 КиБ загружается в ваш бакет S3. В чат добавляется открытая ссылка."), this));

    const auto field = [this, page](const char* title, const QString& value, const char* name,
                                   int max = 512, bool masked = false) {
        page->addWidget(label(ui(title), this, "subtitle"));
        auto* edit = new QLineEdit(this);
        edit->setObjectName(QLatin1String(name));
        edit->setMaxLength(max);
        edit->setMinimumHeight(37);
        if (masked)
            edit->setEchoMode(QLineEdit::Password);
        edit->setText(value);
        page->addWidget(edit);
        return edit;
    };
    s.endpoint = field("Адрес S3", values.endpoint, "attachmentsEndpoint");
    s.endpoint->setPlaceholderText(QLatin1String("https://s3.example.net"));
    s.bucket = field("Бакет", values.bucket, "attachmentsBucket", 63);
    s.region = field("Регион", values.region, "attachmentsRegion", 32);
    s.access = field("Ключ доступа", values.access, "attachmentsAccess", 128);
    s.secret = field("Секретный ключ", values.secret, "attachmentsSecret", 256, true);
    s.publicBase = field("Публичный адрес (необязательно)", values.publicBase, "attachmentsPublicBase");
    s.publicBase->setPlaceholderText(QLatin1String("https://files.example.net"));

    page->addWidget(label(ui("Адресация бакета"), this, "subtitle"));
    s.addressing = new QComboBox(this);
    s.addressing->setObjectName(QLatin1String("attachmentsAddressing"));
    s.addressing->setMinimumHeight(37);
    s.addressing->addItem(ui("Автоматически"));
    s.addressing->addItem(ui("Бакет в пути"));
    s.addressing->addItem(ui("Бакет в имени сервера"));
    s.addressing->setCurrentIndex(int(values.addressing));
    page->addWidget(s.addressing);

    s.certificate = field("Файл CA PEM (необязательно)", values.certificate, "attachmentsCaFile");
    auto* ca = button(ui("Выбрать файл CA"), this, "attachmentsPickCa");
    page->addWidget(ca);
    connect(ca, SIGNAL(clicked()), this, SLOT(chooseCertificate()), Qt::QueuedConnection);

    s.status = label(QString(), this);
    s.status->setObjectName(QLatin1String("attachmentSettingsStatus"));
    s.status->hide();
    page->addWidget(s.status);
    auto* actions = new QHBoxLayout;
    s.save = button(ui("Сохранить"), this, "attachmentsSave");
    s.test = button(ui("Проверить бакет"), this, "attachmentsTestBucket");
    actions->addWidget(s.save, 1);
    actions->addWidget(s.test, 1);
    page->addLayout(actions);
    page->addWidget(label(ui("Проверка загружает небольшой файл и открывает его по публичному адресу."), this));
    page->addStretch(1);
    connect(s.save, SIGNAL(clicked()), this, SLOT(save()), Qt::QueuedConnection);
    connect(s.test, SIGNAL(clicked()), this, SLOT(testBucket()), Qt::QueuedConnection);
}

AttachmentSettingsPanel::~AttachmentSettingsPanel() {
    cancelWorker(state_->worker, this);
}

void AttachmentSettingsPanel::chooseCertificate() {
    selectFile(this, ui("Выбрать сертификаты CA"),
               ui("Сертификаты (*.pem *.crt);;Все файлы (*)"),
               SLOT(certificateSelected(QString)));
}

void AttachmentSettingsPanel::certificateSelected(const QString& path) {
    State& s = *state_;
    if (selectedCa(path).isEmpty()) {
        s.statusMessage(ui("Выберите файл CA в формате PEM до 64 КиБ."));
        return;
    }
    s.certificate->setText(path);
    s.statusMessage(QString());
}

void AttachmentSettingsPanel::save() {
    State& s = *state_;
    const Settings values = s.read();
    if (values.enabled) {
        const QString error = settingsError(values);
        if (!error.isEmpty()) {
            s.statusMessage(error);
            return;
        }
    }
    if (!values.certificate.isEmpty() && selectedCa(values.certificate).isEmpty()) {
        s.statusMessage(ui("Выберите файл CA в формате PEM до 64 КиБ."));
        return;
    }
    if (!storeSettings(values)) {
        s.statusMessage(ui("Не удалось сохранить настройки вложений."));
        return;
    }
    s.statusMessage(ui("Настройки сохранены."));
    emit saved();
}

void AttachmentSettingsPanel::testBucket() {
    State& s = *state_;
    if (transferBusy()) {
        s.statusMessage(ui("Дождитесь завершения текущей загрузки."));
        return;
    }
    const Settings values = s.read();
    const QString error = settingsError(values);
    if (!error.isEmpty()) {
        s.statusMessage(error);
        return;
    }
    const QByteArray ca = selectedCa(values.certificate);
    if (!values.certificate.isEmpty() && ca.isEmpty()) {
        s.statusMessage(ui("Файл CA не читается."));
        return;
    }
    const QByteArray key = "t/" + freshId() + ".txt";
    const auto put = signedPut(values, key, QDateTime::currentDateTimeUtc());
    const auto link = publicLink(values, key);
    if (!put.isValid() || put.isEmpty() || !link.isValid() || link.isEmpty()) {
        s.statusMessage(ui("Проверьте адрес S3 и публичный адрес."));
        return;
    }
    s.test->setEnabled(false);
    s.statusMessage(ui("Проверяем загрузку и публичную ссылку…"));
    s.worker = startWorker(new Worker(put, link, QString(), {"t", "txt", "text/plain"}, ca, true), this);
}

void AttachmentSettingsPanel::transferFinished() {
    State& s = *state_;
    Worker* worker = s.worker;
    s.worker = nullptr;
    if (!worker)
        return;
    s.test->setEnabled(true);
    if (worker->status < 200 || worker->status >= 300) {
        s.statusMessage(transferError(worker->status));
    } else if (worker->publicStatus >= 200 && worker->publicStatus < 300) {
        s.statusMessage(ui("Бакет принимает файлы. Публичная ссылка открывается."));
    } else {
        s.statusMessage(ui("Файл загружен. Публичная ссылка недоступна. %1").arg(transferError(worker->publicStatus)));
    }
}

struct AttachmentUploadPanel::State {
    int remaining = 0;
    QString path;
    QLabel* selected = nullptr;
    QLabel* status = nullptr;
    QPushButton* choose = nullptr;
    QPushButton* upload = nullptr;
    Worker* worker = nullptr;
};

AttachmentUploadPanel::AttachmentUploadPanel(const QString& draft, int maximumUtf8Bytes, QWidget* parent)
    : QDialog(parent), state_(new State) {
    State& s = *state_;
    const int separator = draft.isEmpty() || draft.endsWith(QLatin1Char('\n')) ? 0 : 1;
    s.remaining = qMax(0, maximumUtf8Bytes - draft.toUtf8().size() - separator);
    setObjectName(QLatin1String("attachmentUpload"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::WindowModal);
    setWindowTitle(ui("Вложение"));
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(12, 10, 12, 10);
    page->setSpacing(8);
    page->addWidget(label(ui("Выберите файл до 512 КиБ"), this, "subtitle"));
    s.choose = button(ui("Выбрать файл"), this, "attachmentChooseFile");
    page->addWidget(s.choose);
    s.selected = label(QString(), this);
    s.selected->setObjectName(QLatin1String("attachmentSelectedFile"));
    s.selected->hide();
    page->addWidget(s.selected);
    s.status = label(QString(), this);
    s.status->setObjectName(QLatin1String("attachmentUploadStatus"));
    s.status->hide();
    page->addWidget(s.status);
    auto* actions = new QHBoxLayout;
    auto* cancel = button(ui("Отмена"), this, "attachmentCancel");
    s.upload = button(ui("Загрузить"), this, "attachmentUploadFile");
    s.upload->setEnabled(false);
    actions->addWidget(cancel, 1);
    actions->addWidget(s.upload, 1);
    page->addLayout(actions);
    connect(s.choose, SIGNAL(clicked()), this, SLOT(chooseFile()), Qt::QueuedConnection);
    connect(s.upload, SIGNAL(clicked()), this, SLOT(upload()), Qt::QueuedConnection);
    connect(cancel, SIGNAL(clicked()), this, SLOT(reject()), Qt::QueuedConnection);
    if (s.remaining == 0) {
        s.choose->setEnabled(false);
        s.status->setText(ui("В черновике больше нет места для ссылки."));
        s.status->show();
    }
}

AttachmentUploadPanel::~AttachmentUploadPanel() {
    cancelWorker(state_->worker, this);
}

void AttachmentUploadPanel::chooseFile() {
    selectFile(this, ui("Выбрать вложение"), ui("Все файлы (*)"), SLOT(fileSelected(QString)));
}

void AttachmentUploadPanel::fileSelected(const QString& path) {
    State& s = *state_;
    const QFileInfo info(path);
    s.path.clear();
    s.upload->setEnabled(false);
    s.selected->setText(info.fileName());
    s.selected->setVisible(!info.fileName().isEmpty());
    if (!info.isFile() || !info.isReadable()) {
        s.status->setText(ui("Не удалось прочитать файл."));
        s.status->show();
        return;
    }
    if (info.size() > MaxFileBytes) {
        s.status->setText(ui("Файл больше 512 КиБ."));
        s.status->show();
        return;
    }
    s.path = path;
    s.selected->setText(ui("%1 · %2 байт").arg(info.fileName()).arg(info.size()));
    s.status->hide();
    s.upload->setEnabled(true);
}

void AttachmentUploadPanel::upload() {
    State& s = *state_;
    if (transferBusy()) {
        s.status->setText(ui("Дождитесь завершения текущей загрузки."));
        s.status->show();
        return;
    }
    const Settings values = storedSettings();
    if (!values.enabled || !settingsError(values).isEmpty() || s.path.isEmpty()) {
        s.status->setText(ui("Настройте бакет S3 в разделе «Вложения»."));
        s.status->show();
        return;
    }
    const QByteArray ca = selectedCa(values.certificate);
    if (!values.certificate.isEmpty() && ca.isEmpty()) {
        s.status->setText(ui("Файл CA не читается."));
        s.status->show();
        return;
    }

    const FileKind kind = fileKind(s.path);
    const QByteArray key = kind.category + '/' + freshId() + '.' + kind.extension;
    const QUrl link = publicLink(values, key);
    const QByteArray linkText = link.toEncoded();
    if (linkText.isEmpty() || linkText.size() > s.remaining) {
        s.status->setText(ui("Ссылка занимает %1 байт, в сообщении осталось %2. Сократите текст или публичный адрес.")
            .arg(linkText.size()).arg(s.remaining));
        s.status->show();
        return;
    }
    const auto put = signedPut(values, key, QDateTime::currentDateTimeUtc());
    if (!put.isValid() || put.isEmpty()) {
        s.status->setText(ui("Проверьте адрес S3."));
        s.status->show();
        return;
    }
    s.choose->setEnabled(false);
    s.upload->setEnabled(false);
    s.status->setText(ui("Загружаем файл в ваш бакет S3…"));
    s.status->show();
    s.worker = startWorker(new Worker(put, link, s.path, kind, ca, false), this);
}

void AttachmentUploadPanel::transferFinished() {
    State& s = *state_;
    Worker* worker = s.worker;
    s.worker = nullptr;
    if (!worker || !isVisible())
        return;
    if (worker->status >= 200 && worker->status < 300) {
        emit linkReady(QString::fromLatin1(worker->readAddress().toEncoded()));
        accept();
    } else {
        s.status->setText(transferError(worker->status));
        s.choose->setEnabled(true);
        s.upload->setEnabled(true);
    }
}
