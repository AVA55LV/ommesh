#include "settings_backup.hpp"

#include "client.hpp"
#include "demo_radio.hpp"
#include "window.hpp"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimer>
#include <QtGui/QDesktopServices>
#include <QtGui/QHBoxLayout>
#include <QtGui/QLabel>
#include <QtGui/QListWidget>
#include <QtGui/QPushButton>
#include <QtGui/QVBoxLayout>

namespace {

constexpr char Signature[] = "OMMBELLE";
constexpr int SignatureBytes = sizeof(Signature) - 1;
constexpr int DigestBytes = 20;
constexpr int BackupVersion = 1;
constexpr int MaximumEntries = 1536;
constexpr int MaximumItems = 1024;
constexpr int MaximumFileBytes = 8 * 1024 * 1024;
constexpr int MaximumHistoryBytes = 2 * 1024 * 1024;
constexpr int MaximumHistoryMessages = 400;
constexpr int MaximumReactions = 32;
constexpr int MaximumStringBytes = 1024;

QString ui(const char* text) { return QString::fromUtf8(text); }

bool fail(QString& error, const char* text) {
    error = ui(text);
    return false;
}

bool hex(const QString& text, int bytes) {
    if (text.size() != bytes * 2)
        return false;
    for (const auto character : text) {
        const ushort c = character.unicode();
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f'))
            return false;
    }
    return true;
}

enum ValueType { Unsupported, Toggle, Number, Text, PublicKey, List, TextMap, ToggleMap, History };

struct Rule {
    ValueType type = Unsupported;
    int min = 0;
    int max = 0;
};

Rule ruleFor(const QString& key) {
    const char* const toggles[] = {
        "ui/autoReconnect", "ui/wlanRecovery", "ui/vibration", "ui/doNotDisturb",
        "ui/notifyDirect", "ui/notifyChannels", "ui/openAtUnread", "ui/compactCyrillic",
        "ui/compactNear", "ui/compactCursive", "ui/restoreCyrillic", "network/favoritesFirst"
    };
    for (const char* name : toggles)
        if (key == QLatin1String(name))
            return {Toggle};

    struct NumericPreference { const char* name; int min; int max; };
    const NumericPreference numbers[] = {
        {"ui/theme", 0, 13}, {"ui/chatTextSize", 12, 22}, {"ui/linkPreviewMode", 0, 2},
        {"ui/sendTries", Client::MinimumSendTries, Client::MaximumSendTries},
        {"ui/notificationSound", 0, 4}, {"ui/chatBackground", 0, 8}, {"map/source", 0, 3},
        {"map/region", 0, 3}, {"network/display", 0, 6}, {"network/order", 0, 3},
        {"network/silenceDays", 0, 90}
    };
    for (const auto& preference : numbers)
        if (key == QLatin1String(preference.name))
            return {Number, preference.min, preference.max};

    if (key == QLatin1String("radio/lastName")) return {Text};
    if (key == QLatin1String("radio/last")) return {PublicKey};
    if (key == QLatin1String("network/favorites")) return {List};
    if (key == QLatin1String("chats/pins")) return {ToggleMap};
    if (key == QLatin1String("ui/blockedSenders")) return {TextMap};
    if (key.startsWith(QLatin1String("radios/")) && key.endsWith(QLatin1String("/history")) &&
            key.size() == 79 && hex(key.mid(7, 64), 32))
        return {History};
    if (key.startsWith(QLatin1String("chatOptions/"))) {
        const int separator = key.lastIndexOf(QLatin1Char('/'));
        const QString target = key.mid(12, separator - 12);
        if (separator > 12 && target.size() <= 256 && hex(target, target.size() / 2)) {
            const QString option = key.mid(separator + 1);
            if (option == QLatin1String("notices")) return {Number, -1, 2};
            if (option == QLatin1String("sound")) return {Number, -1, 4};
            if (option == QLatin1String("background")) return {Number, -1, 8};
        }
    }
    return {};
}

bool readBytes(QDataStream& stream, QByteArray& bytes, int maximum) {
    quint32 length = 0;
    stream >> length;
    if (stream.status() != QDataStream::Ok || length > static_cast<quint32>(maximum) ||
            length > stream.device()->bytesAvailable())
        return false;
    bytes.resize(static_cast<int>(length));
    return !length || stream.readRawData(bytes.data(), static_cast<int>(length)) == static_cast<int>(length);
}

bool readText(QDataStream& stream, QString& text, int maximum = MaximumStringBytes) {
    QByteArray bytes;
    if (!readBytes(stream, bytes, maximum))
        return false;
    text = QString::fromUtf8(bytes.constData(), bytes.size());
    return text.toUtf8() == bytes;
}

bool writeText(QDataStream& stream, const QString& text) {
    const QByteArray bytes = text.toUtf8();
    if (bytes.size() > MaximumStringBytes)
        return false;
    stream << (bytes.isNull() ? QByteArray("") : bytes);
    return stream.status() == QDataStream::Ok;
}

bool readToggle(QDataStream& stream, bool& value) {
    quint8 bit = 0;
    stream >> bit;
    value = bit != 0;
    return stream.status() == QDataStream::Ok && bit <= 1;
}

bool skipQtString(QDataStream& stream) {
    quint32 bytes = 0;
    stream >> bytes;
    if (stream.status() != QDataStream::Ok)
        return false;
    if (bytes == 0xffffffffU)
        return true;
    return bytes % 2 == 0 && bytes <= 65536 && bytes <= stream.device()->bytesAvailable() &&
           (!bytes || stream.skipRawData(static_cast<int>(bytes)) == static_cast<int>(bytes));
}

bool skipQtStrings(QDataStream& stream, int count) {
    for (int index = 0; index < count; ++index)
        if (!skipQtString(stream))
            return false;
    return true;
}

bool validHistory(const QByteArray& bytes, int& messages) {
    if (bytes.size() < 8 || bytes.size() > MaximumHistoryBytes)
        return false;
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_4_6);
    quint32 version = 0, count = 0;
    stream >> version >> count;
    if (version < 1 || version > 4 || count > MaximumHistoryMessages)
        return false;
    // Validate lengths before the client reads Qt strings into allocated memory.
    for (quint32 index = 0; index < count; ++index) {
        bool outgoing = false, unread = false;
        quint32 timestamp = 0;
        if (!skipQtStrings(stream, 4))
            return false;
        stream >> timestamp;
        if (!readToggle(stream, outgoing) || !skipQtString(stream) || !readToggle(stream, unread))
            return false;
        if (version >= 2) {
            qint32 relays = 0, hops = 0;
            stream >> relays >> hops;
            if (!skipQtString(stream))
                return false;
        }
        if (version >= 3) {
            bool partial = false;
            if (!readToggle(stream, partial))
                return false;
            quint16 reactions = 0;
            stream >> reactions;
            if (reactions > MaximumReactions)
                return false;
            for (quint16 reaction = 0; reaction < reactions; ++reaction) {
                bool own = false;
                if (!skipQtStrings(stream, 2))
                    return false;
                stream >> timestamp;
                if (!readToggle(stream, own))
                    return false;
            }
        }
        if (version >= 4 && !skipQtString(stream))
            return false;
    }
    messages = static_cast<int>(count);
    return stream.status() == QDataStream::Ok && stream.atEnd();
}

bool writeValue(QDataStream& stream, const QVariant& value, const Rule& rule) {
    switch (rule.type) {
    case Toggle:
        stream << quint8(value.toBool());
        break;
    case Number: {
        bool valid = false;
        const int number = value.toInt(&valid);
        if (!valid || number < rule.min || number > rule.max)
            return false;
        stream << qint32(number);
        break;
    }
    case PublicKey:
        if (!hex(value.toString(), 32))
            return false;
        return writeText(stream, value.toString());
    case Text:
        return writeText(stream, value.toString());
    case List: {
        const QStringList entries = value.toStringList();
        if (entries.size() > MaximumItems)
            return false;
        stream << quint16(entries.size());
        for (const QString& entry : entries)
            if (!writeText(stream, entry))
                return false;
        break;
    }
    case TextMap:
    case ToggleMap: {
        const QVariantMap entries = value.toMap();
        if (entries.size() > MaximumItems)
            return false;
        stream << quint16(entries.size());
        for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
            if (!writeText(stream, it.key()))
                return false;
            if (rule.type == ToggleMap)
                stream << quint8(it.value().toBool());
            else if (!writeText(stream, it.value().toString()))
                return false;
        }
        break;
    }
    case History: {
        const QByteArray bytes = value.toByteArray();
        int messages = 0;
        if (!validHistory(bytes, messages))
            return false;
        stream << bytes;
        break;
    }
    case Unsupported:
        return false;
    }
    return stream.status() == QDataStream::Ok;
}

bool readValue(QDataStream& stream, QVariant& value, const Rule& rule, SettingsBackup::Data& data) {
    switch (rule.type) {
    case Toggle: {
        bool bit = false;
        if (!readToggle(stream, bit))
            return false;
        value = bit;
        break;
    }
    case Number: {
        qint32 number = 0;
        stream >> number;
        if (number < rule.min || number > rule.max)
            return false;
        value = number;
        break;
    }
    case Text:
    case PublicKey: {
        QString text;
        if (!readText(stream, text) || (rule.type == PublicKey && !hex(text, 32)))
            return false;
        value = text;
        break;
    }
    case List:
    case TextMap:
    case ToggleMap: {
        quint16 count = 0;
        stream >> count;
        if (count > MaximumItems)
            return false;
        QStringList list;
        QVariantMap map;
        for (quint16 index = 0; index < count; ++index) {
            QString key;
            if (!readText(stream, key) || map.contains(key))
                return false;
            if (rule.type == List) {
                list.append(key);
            } else if (rule.type == TextMap) {
                QString text;
                if (!readText(stream, text))
                    return false;
                map.insert(key, text);
            } else {
                bool bit = false;
                if (!readToggle(stream, bit))
                    return false;
                map.insert(key, bit);
            }
        }
        value = rule.type == List ? QVariant(list) : QVariant(map);
        break;
    }
    case History: {
        QByteArray bytes;
        int count = 0;
        if (!readBytes(stream, bytes, MaximumHistoryBytes) || !validHistory(bytes, count))
            return false;
        value = bytes;
        ++data.histories;
        data.messages += count;
        break;
    }
    case Unsupported:
        return false;
    }
    return stream.status() == QDataStream::Ok;
}

QLabel* feedbackLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QLatin1String("muted"));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

}  // namespace

namespace SettingsBackup {

QString defaultDirectory() {
#ifdef Q_OS_SYMBIAN
    const QString phoneData = QLatin1String("C:/Data");
    if (QDir(phoneData).exists())
        return phoneData;
#endif
    const QString documents = QDesktopServices::storageLocation(QDesktopServices::DocumentsLocation);
    return QFileInfo(documents).isDir() ? documents : QDir::homePath();
}

bool saveBytes(const QString& path, const QByteArray& bytes, QString& error) {
    QTemporaryFile temporary(QFileInfo(path).absolutePath() + QLatin1String("/.ommesh-backup-XXXXXX"));
    if (bytes.isEmpty() || !temporary.open() || temporary.write(bytes) != bytes.size() || !temporary.flush())
        return fail(error, "Не удалось записать копию в выбранную папку.");
    if (!temporary.rename(path))
        return fail(error, "Не удалось сохранить копию. Возможно, такое имя уже занято.");
    temporary.setAutoRemove(false);
    return true;
}

bool readRadioHistory(QSettings& settings, const QString& publicKey, QByteArray& bytes, int& count) {
    if (!hex(publicKey, 32))
        return false;
    bytes = settings.value(QLatin1String("radios/") + publicKey + QLatin1String("/history")).toByteArray();
    return validRadioHistory(bytes, count);
}

bool validRadioHistory(const QByteArray& bytes, int& count) {
    count = 0;
    return bytes.isEmpty() || validHistory(bytes, count);
}

bool restoreRadioHistory(QSettings& settings, const QString& publicKey, const QByteArray& bytes, QString& error) {
    if (bytes.isEmpty())
        return true;
    Data data;
    if (!hex(publicKey, 32) || !validHistory(bytes, data.messages))
        return fail(error, "История Belle в файле копии повреждена.");
    data.histories = 1;
    data.values.insert(QLatin1String("radios/") + publicKey + QLatin1String("/history"), bytes);
    return restore(data, settings, error);
}

bool writeFile(const QString& path, QSettings& settings, QString& error) {
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return fail(error, "Не удалось прочитать настройки OMMesh.");

    QStringList keys;
    for (const QString& key : settings.allKeys())
        if (ruleFor(key).type != Unsupported)
            keys.append(key);
    if (keys.isEmpty())
        return fail(error, "Пока нет настроек или истории для копирования.");
    if (keys.size() > MaximumEntries)
        return fail(error, "Слишком много записей для одной копии.");

    QByteArray content;
    QDataStream stream(&content, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_4_6);
    stream.writeRawData(Signature, SignatureBytes);
    stream << quint16(BackupVersion) << quint16(keys.size());
    for (const QString& key : keys) {
        if (!writeText(stream, key) || !writeValue(stream, settings.value(key), ruleFor(key)))
            return fail(error, "Одна из записей настроек или истории повреждена.");
        if (content.size() > MaximumFileBytes - DigestBytes)
            return fail(error, "Копия превышает допустимый размер 8 МБ.");
    }
    content.append(QCryptographicHash::hash(content, QCryptographicHash::Sha1));
    return saveBytes(path, content, error);
}

bool readFile(const QString& path, Data& data, QString& error) {
    QFile file(path);
    if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly))
        return fail(error, "Не удалось открыть выбранный файл.");
    QByteArray content = file.read(MaximumFileBytes + 1);
    if (content.size() > MaximumFileBytes)
        return fail(error, "Копия превышает допустимый размер 8 МБ.");
    if (content.size() < SignatureBytes + 4 + DigestBytes || !content.startsWith(Signature))
        return fail(error, "Выберите резервную копию OMMesh Belle.");
    const QByteArray digest = content.right(DigestBytes);
    content.chop(DigestBytes);
    if (digest != QCryptographicHash::hash(content, QCryptographicHash::Sha1))
        return fail(error, "Файл копии повреждён.");

    QDataStream stream(content);
    stream.setVersion(QDataStream::Qt_4_6);
    stream.skipRawData(SignatureBytes);
    quint16 version = 0, count = 0;
    stream >> version >> count;
    if (version != BackupVersion)
        return fail(error, "Эта версия файла копии не поддерживается.");
    if (!count || count > MaximumEntries)
        return fail(error, "В копии недопустимое число записей.");
    Data parsed;
    for (quint16 index = 0; index < count; ++index) {
        QString key;
        QVariant value;
        if (!readText(stream, key, 300) || parsed.values.contains(key) ||
                !readValue(stream, value, ruleFor(key), parsed))
            return fail(error, "Файл копии содержит недопустимую запись.");
        parsed.values.insert(key, value);
    }
    if (!stream.atEnd())
        return fail(error, "Файл копии содержит лишние данные.");
    data = parsed;
    return true;
}

bool restore(const Data& data, QSettings& settings, QString& error) {
    if (data.values.isEmpty() || data.values.size() > MaximumEntries)
        return fail(error, "В копии недопустимое число записей.");
    for (auto it = data.values.constBegin(); it != data.values.constEnd(); ++it)
        if (ruleFor(it.key()).type == Unsupported)
            return fail(error, "Файл копии содержит недопустимую запись.");
    settings.sync();
    if (settings.status() != QSettings::NoError || !settings.isWritable())
        return fail(error, "Хранилище настроек недоступно для записи.");

    QVariantMap previous;
    for (auto it = data.values.constBegin(); it != data.values.constEnd(); ++it) {
        previous.insert(it.key(), settings.value(it.key()));
        settings.setValue(it.key(), it.value());
    }
    settings.sync();
    if (settings.status() == QSettings::NoError)
        return true;
    for (auto it = previous.constBegin(); it != previous.constEnd(); ++it) {
        if (it.value().isValid())
            settings.setValue(it.key(), it.value());
        else
            settings.remove(it.key());
    }
    settings.sync();
    return fail(error, "Не удалось записать настройки и историю из копии.");
}

}  // namespace SettingsBackup

SettingsBackupFilePicker::SettingsBackupFilePicker(bool exportFile, const QString& directory,
                                                   QWidget* parent, const QStringList& patterns)
    : QWidget(parent), exportFile_(exportFile),
      patterns_(patterns.isEmpty() ? QStringList(QLatin1String("*.omb")) : patterns), entries_(makeTouchList(this)),
      path_(feedbackLabel(QString(), this)), empty_(feedbackLabel(QString(), this)),
      disks_(new QPushButton(ui("Диски"), this)), up_(new QPushButton(ui("Вверх"), this)),
      choose_(new QPushButton(exportFile ? ui("Сохранить сюда") : ui("Выбрать файл"), this)) {
    setObjectName(QLatin1String("settingsBackupPicker"));
    entries_->setObjectName(QLatin1String("settingsBackupFiles"));
    path_->setObjectName(QLatin1String("settingsBackupPath"));
    choose_->setObjectName(QLatin1String("settingsBackupChoose"));
    disks_->setObjectName(QLatin1String("settingsBackupDrives"));
    up_->setObjectName(QLatin1String("settingsBackupUp"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(7);
    layout->addWidget(path_);
    layout->addWidget(empty_);

    auto* locations = new QHBoxLayout;
    locations->setSpacing(7);
    locations->addWidget(disks_, 1);
    locations->addWidget(up_, 1);
    layout->addLayout(locations);
    layout->addWidget(entries_, 1);

    auto* cancelButton = new QPushButton(ui("Отмена"), this);
    cancelButton->setObjectName(QLatin1String("settingsBackupCancelPicker"));
    for (QPushButton* button : {disks_, up_, cancelButton, choose_})
        button->setMinimumHeight(44);
    auto* footer = new QHBoxLayout;
    footer->setSpacing(7);
    footer->addWidget(cancelButton, 1);
    footer->addWidget(choose_, 1);
    layout->addLayout(footer);

    connect(disks_, SIGNAL(clicked()), this, SLOT(showDrives()), Qt::QueuedConnection);
    connect(up_, SIGNAL(clicked()), this, SLOT(goUp()), Qt::QueuedConnection);
    connect(entries_, SIGNAL(itemClicked(QListWidgetItem*)), this, SLOT(activateEntry(QListWidgetItem*)));
    connect(entries_, SIGNAL(itemActivated(QListWidgetItem*)), this, SLOT(activateEntry(QListWidgetItem*)));
    connect(entries_, SIGNAL(itemSelectionChanged()), this, SLOT(updateSelection()));
    connect(choose_, SIGNAL(clicked()), this, SLOT(choosePath()), Qt::QueuedConnection);
    connect(cancelButton, SIGNAL(clicked()), this, SLOT(cancel()), Qt::QueuedConnection);

    if (QFileInfo(directory).isDir())
        showDirectory(directory);
    else
        showDrives();
}

void SettingsBackupFilePicker::addEntry(const QString& label, const QString& path, bool isDirectory) {
    auto* entry = new QListWidgetItem(label, entries_);
    entry->setData(Qt::UserRole, path);
    entry->setData(Qt::UserRole + 1, isDirectory);
    entry->setSizeHint(QSize(0, 44));
}

void SettingsBackupFilePicker::showDrives() {
    if (closing_)
        return;
    pendingDirectory_.clear();
    directory_.clear();
    entries_->clear();
    for (const QFileInfo& drive : QDir::drives()) {
        const QString path = drive.absoluteFilePath();
#ifdef Q_OS_SYMBIAN
        if (path.startsWith(QLatin1String("Z:"), Qt::CaseInsensitive))
            continue;
#endif
        addEntry(QDir::toNativeSeparators(path), path, true);
    }
    path_->setText(ui("Выберите диск"));
    empty_->setText(ui("Доступных дисков нет."));
    empty_->setVisible(entries_->count() == 0);
    disks_->setEnabled(false);
    up_->setEnabled(false);
    updateSelection();
}

void SettingsBackupFilePicker::showDirectory(const QString& directory) {
    QDir folder(directory);
    if (!folder.exists() || !folder.isReadable()) {
        empty_->setText(ui("Не удалось открыть папку."));
        empty_->show();
        return;
    }
    directory_ = folder.absolutePath();
    entries_->clear();
    QDir::Filters filters = QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable;
    if (!exportFile_) {
        filters |= QDir::Files | QDir::AllDirs;
        folder.setNameFilters(patterns_);
    }
    const QFileInfoList items = folder.entryInfoList(filters, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& item : items) {
        const QString name = item.isDir() ? item.fileName() + QLatin1Char('/') : item.fileName();
        addEntry(name, item.absoluteFilePath(), item.isDir());
    }
    path_->setText(QDir::toNativeSeparators(directory_));
    empty_->setText(exportFile_ ? ui("В папке нет вложенных папок.") : ui("В папке нет подходящих файлов."));
    empty_->setVisible(entries_->count() == 0);
    disks_->setEnabled(true);
    up_->setEnabled(true);
    updateSelection();
}

void SettingsBackupFilePicker::goUp() {
    if (closing_ || directory_.isEmpty())
        return;
    pendingDirectory_.clear();
    QDir parent(directory_);
    if (parent.cdUp() && parent.absolutePath() != directory_)
        showDirectory(parent.absolutePath());
    else
        showDrives();
}

void SettingsBackupFilePicker::activateEntry(QListWidgetItem* entry) {
    if (closing_ || !entry || !entry->data(Qt::UserRole + 1).toBool())
        return;
    const bool scheduled = !pendingDirectory_.isEmpty();
    pendingDirectory_ = entry->data(Qt::UserRole).toString();
    if (!scheduled)
        QTimer::singleShot(0, this, SLOT(openPendingDirectory()));
}

void SettingsBackupFilePicker::openPendingDirectory() {
    const QString selected = pendingDirectory_;
    pendingDirectory_.clear();
    if (!closing_ && !selected.isEmpty())
        showDirectory(selected);
}

void SettingsBackupFilePicker::updateSelection() {
    const QListWidgetItem* current = entries_->currentItem();
    choose_->setEnabled(!closing_ && !directory_.isEmpty() &&
                        (exportFile_ || (current && !current->data(Qt::UserRole + 1).toBool())));
}

void SettingsBackupFilePicker::choosePath() {
    if (closing_ || !choose_->isEnabled())
        return;
    const QListWidgetItem* current = entries_->currentItem();
    if (!exportFile_ && (!current || current->data(Qt::UserRole + 1).toBool()))
        return;
    const QString selected = exportFile_ ? directory_ : current->data(Qt::UserRole).toString();
    closing_ = true;
    updateSelection();
    emit pathSelected(selected);
    emit closeRequested(this);
}

void SettingsBackupFilePicker::cancel() {
    if (!closing_) {
        closing_ = true;
        emit closeRequested(this);
    }
}

SettingsBackupPanel::SettingsBackupPanel(Client& client, QWidget* parent)
    : QWidget(parent), client_(client) {
    setObjectName(QLatin1String("settingsBackup"));
    directory_ = SettingsBackup::defaultDirectory();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 8);
    layout->setSpacing(10);
    layout->addWidget(feedbackLabel(ui("Копия для OMMesh Belle содержит настройки приложения и историю чатов. "
        "Храните файл в безопасном месте: сообщения доступны в нём без пароля."), this));
    auto* save = new QPushButton(ui("Сохранить копию"), this);
    save->setObjectName(QLatin1String("settingsBackupExport"));
    save->setMinimumHeight(44);
    layout->addWidget(save);
    connect(save, SIGNAL(clicked()), this, SLOT(chooseExportDirectory()), Qt::QueuedConnection);
    auto* open = new QPushButton(ui("Выбрать копию"), this);
    open->setObjectName(QLatin1String("settingsBackupImport"));
    open->setMinimumHeight(44);
    layout->addWidget(open);
    connect(open, SIGNAL(clicked()), this, SLOT(chooseImportFile()), Qt::QueuedConnection);

    auto* import = new QPushButton(ui("Импорт истории OMMesh"), this);
    import->setObjectName(QLatin1String("settingsBackupAndroidHistory"));
    import->setMinimumHeight(44);
    layout->addWidget(import);
    connect(import, SIGNAL(clicked()), this, SLOT(chooseAndroidHistory()), Qt::QueuedConnection);

    feedback_ = feedbackLabel(QString(), this);
    layout->addWidget(feedback_);
    feedback_->hide();

    confirmation_ = new QWidget(this);
    auto* actions = new QVBoxLayout(confirmation_);
    actions->setContentsMargins(0, 0, 0, 0);
    confirmationNote_ = feedbackLabel(QString(), confirmation_);
    actions->addWidget(confirmationNote_);
    auto* buttons = new QHBoxLayout;
    auto* confirm = new QPushButton(ui("Восстановить"), confirmation_);
    auto* cancel = new QPushButton(ui("Отмена"), confirmation_);
    confirm->setObjectName(QLatin1String("settingsBackupConfirm"));
    confirm->setMinimumHeight(44);
    cancel->setMinimumHeight(44);
    buttons->addWidget(confirm, 1);
    buttons->addWidget(cancel, 1);
    actions->addLayout(buttons);
    connect(confirm, SIGNAL(clicked()), this, SLOT(applyBackup()));
    connect(cancel, SIGNAL(clicked()), this, SLOT(cancelRestore()));
    layout->addWidget(confirmation_);
    confirmation_->hide();
    layout->addStretch();
}

void SettingsBackupPanel::chooseExportDirectory() { chooseFile(true); }

void SettingsBackupPanel::chooseImportFile() {
    if (!canRestore()) {
        cancelRestore();
        setFeedback(ui("Отключите ноду, чтобы восстановить копию."));
        return;
    }
    chooseFile(false);
}

void SettingsBackupPanel::chooseFile(bool exportFile) {
    cancelRestore();
    auto* picker = new SettingsBackupFilePicker(exportFile, directory_, this);
    connect(picker, SIGNAL(pathSelected(QString)), this,
            exportFile ? SLOT(exportBackup(QString)) : SLOT(reviewBackup(QString)));
    connect(picker, SIGNAL(closeRequested(QWidget*)), this, SIGNAL(pickerClosed(QWidget*)));
    emit pickerRequested(picker, exportFile ? ui("Папка для копии") : ui("Выберите копию"));
}

void SettingsBackupPanel::chooseAndroidHistory() {
    cancelRestore();
    if (client_.connecting() || client_.reconnecting()) {
        setFeedback(ui("Дождитесь подключения радио."));
        return;
    }
    auto* picker = new SettingsBackupFilePicker(false, directory_, this, QStringList(QLatin1String("*.json")));
    connect(picker, SIGNAL(pathSelected(QString)), this, SLOT(reviewAndroidHistory(QString)));
    connect(picker, SIGNAL(closeRequested(QWidget*)), this, SIGNAL(pickerClosed(QWidget*)));
    emit pickerRequested(picker, ui("Выберите историю OMMesh"));
}

void SettingsBackupPanel::reviewAndroidHistory(const QString& path) {
    cancelRestore();
    directory_ = QFileInfo(path).absolutePath();
    QSettings settings;
    settings.setFallbacksEnabled(false);
    QString error;
    if (!BackupHistoryImport::preview(path, settings, client_.radioChannels(), importedHistory_, error)) {
        setFeedback(error);
        return;
    }
    setFeedback(ui("%1\nНовых сообщений: %2 · уже есть: %3 · пропущено: %4")
                    .arg(importedHistory_.radioName).arg(importedHistory_.added)
                    .arg(importedHistory_.duplicates).arg(importedHistory_.skipped + importedHistory_.omitted));
    confirmationNote_->setText(ui("Новые сообщения будут добавлены к истории этого радио."));
    confirmation_->setVisible(importedHistory_.added > 0);
}

void SettingsBackupPanel::exportBackup(const QString& directory) {
    directory_ = directory;
    const QString filename = QLatin1String("ommesh-belle-") +
        QDateTime::currentDateTime().toString(QLatin1String("yyyyMMdd-HHmmss-zzz")) + QLatin1String(".omb");
    QSettings settings;
    settings.setFallbacksEnabled(false);
    QString error;
    if (SettingsBackup::writeFile(QDir(directory).filePath(filename), settings, error))
        setFeedback(ui("Копия сохранена\n%1").arg(QDir::toNativeSeparators(QDir(directory).filePath(filename))));
    else
        setFeedback(error);
}

void SettingsBackupPanel::reviewBackup(const QString& path) {
    cancelRestore();
    const QFileInfo file(path);
    directory_ = file.absolutePath();
    QString error;
    if (!SettingsBackup::readFile(path, pending_, error)) {
        setFeedback(error);
        return;
    }
    setFeedback(ui("%1\nНод с историей: %2 · сообщений: %3 · записей настроек: %4").arg(file.fileName())
        .arg(pending_.histories).arg(pending_.messages).arg(pending_.values.size() - pending_.histories));
    confirmationNote_->setText(ui("Совпадающие настройки и история тех же нод будут заменены."));
    confirmation_->show();
}

bool SettingsBackupPanel::canRestore() const {
    return !client_.connected() && !client_.connecting() && !client_.reconnecting() && !demoRadioActive(client_);
}

void SettingsBackupPanel::applyBackup() {
    if (!importedHistory_.radioKey.isEmpty()) {
        if (client_.connecting() || client_.reconnecting() ||
                (client_.connected() && !client_.canLoadRadioBackupHistory(importedHistory_.radioKey))) {
            setFeedback(ui("Дождитесь окончания отправки и проверьте, что подключено нужное радио."));
            return;
        }
        QSettings settings;
        settings.setFallbacksEnabled(false);
        QString error;
        if (!BackupHistoryImport::apply(importedHistory_, settings, error)) {
            setFeedback(error);
            return;
        }
        const int added = importedHistory_.added;
        const QString key = importedHistory_.radioKey;
        cancelRestore();
        if (client_.connected())
            client_.reloadRadioBackupHistory(key);
        else
            client_.reloadStoredHistory();
        setFeedback(ui("Добавлено сообщений: %1").arg(added));
        return;
    }
    if (pending_.values.isEmpty())
        return;
    if (!canRestore()) {
        setFeedback(ui("Отключите ноду перед восстановлением и выберите копию снова."));
        confirmation_->hide();
        pending_ = SettingsBackup::Data();
        return;
    }
    QSettings settings;
    settings.setFallbacksEnabled(false);
    QString error;
    if (!SettingsBackup::restore(pending_, settings, error)) {
        setFeedback(error);
        return;
    }
    confirmation_->hide();
    pending_ = SettingsBackup::Data();
    client_.reloadStoredHistory();
    setFeedback(ui("Копия восстановлена. Перезапустите OMMesh, чтобы применить настройки оформления и уведомлений."));
    emit backupRestored();
}

void SettingsBackupPanel::cancelRestore() {
    confirmation_->hide();
    pending_ = SettingsBackup::Data();
    importedHistory_ = BackupHistoryImport::Plan();
    setFeedback(QString());
}

void SettingsBackupPanel::setFeedback(const QString& message) {
    feedback_->setText(message);
    feedback_->setVisible(!message.isEmpty());
}
