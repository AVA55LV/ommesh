#include "settings_radio_backup.hpp"

#include "backup_history_import.hpp"
#include "client.hpp"
#include "demo_radio.hpp"
#include "radio_backup_archive.hpp"
#include "settings_backup.hpp"
#include "window.hpp"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QRegExp>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QHBoxLayout>
#include <QtGui/QLabel>
#include <QtGui/QLineEdit>
#include <QtGui/QPushButton>
#include <QtGui/QScrollArea>
#include <QtGui/QVBoxLayout>

namespace {

QString ui(const char* text) { return QString::fromUtf8(text); }

QLabel* caption(QWidget* parent, const QString& text, const char* name = "muted") {
    auto* result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setObjectName(QLatin1String(name));
    return result;
}

QVBoxLayout* group(QVBoxLayout* page) {
    auto* body = new QWidget(page->parentWidget());
    body->setObjectName(QLatin1String("settingsGroup"));
    body->setAttribute(Qt::WA_StyledBackground);
    auto* rows = new QVBoxLayout(body);
    rows->setContentsMargins(8, 7, 8, 7);
    rows->setSpacing(5);
    page->addWidget(body);
    return rows;
}

QPushButton* action(QBoxLayout* layout, const QString& text, const char* name, int operation = 0) {
    auto* result = new QPushButton(text, layout->parentWidget());
    result->setObjectName(QLatin1String(name));
    result->setMinimumHeight(44);
    if (operation)
        result->setProperty("backupOperation", operation);
    layout->addWidget(result);
    return result;
}

void scrollTo(QWidget* item) {
    for (QWidget* parent = item->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(parent)) {
            scroll->ensureWidgetVisible(item, 0, 12);
            return;
        }
    }
}

QByteArray readCopy(const QString& path, int maximum, QString& error) {
    QFile file(path);
    if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly)) {
        error = ui("Не удалось открыть выбранный файл.");
        return {};
    }
    const QByteArray content = file.read(maximum + 1);
    if (content.isEmpty() || content.size() > maximum || !file.atEnd() || file.error() != QFile::NoError) {
        error = ui("Файл пустой, слишком большой или не читается.");
        return {};
    }
    return content;
}

QString fileName(bool onlyKey, const QDir& directory, const std::string& radioName) {
    const QDateTime now = QDateTime::currentDateTime();
    const QLatin1String extension(".json");
    if (!onlyKey)
        return QLatin1String("ommesh-backup-") + now.toString(QLatin1String("yyyyMMdd-HHmmss-zzz")) + extension;

    QString name = QString::fromUtf8(radioName.data(), static_cast<int>(radioName.size()));
    name.replace(QRegExp(QLatin1String("[\\\\/:*?\"<>|\\s.\\x0000-\\x001f]+")), QLatin1String("-"));
    name.remove(QRegExp(QLatin1String("^-+|-+$")));
    if (name.isEmpty())
        name = QLatin1String("radio");
    const QString stem = QLatin1String("ommesh-key-") + name + QLatin1Char('-') +
                         now.toString(QLatin1String("yyyy-MM-dd"));
    if (!QFileInfo(directory.filePath(stem + extension)).exists())
        return stem + extension;
    return stem + QLatin1Char('-') + now.toString(QLatin1String("HHmmss-zzz")) + extension;
}

} // namespace

struct RadioBackupPanel::State {
    explicit State(Client& radio)
        : client(radio), directory(SettingsBackup::defaultDirectory()) {}

    bool online() { return client.connected() && !demoRadioActive(client); }

    Client& client;
    QString directory;
    QString expectedTarget;
    Action pickerAction = SaveRadio;
    QPointer<SettingsBackupFilePicker> picker;
    int request = 0;
    Action currentAction = SaveRadio;
    QString exportPath;
    std::optional<RadioBackupSnapshot> exporting;

    QByteArray reviewedBytes;
    QByteArray reviewedBelleHistory;
    RadioBackupRestore reviewed;
    bool reviewReady = false;
    bool applying = false;

    QLabel* radioName = nullptr;
    QLabel* saveInfo = nullptr;
    QLabel* feedback = nullptr;
    QLabel* reviewSummary = nullptr;
    QWidget* review = nullptr;
    QWidget* keyInput = nullptr;
    QLineEdit* keyText = nullptr;
    QPushButton* confirm = nullptr;
    QVector<QPushButton*> radioActions;
};

RadioBackupPanel::RadioBackupPanel(Client& client, QWidget* parent)
    : QWidget(parent), state_(new State(client)) {
    State& s = *state_;
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(0, 0, 0, 0);
    page->setSpacing(9);
    s.radioName = caption(this, QString(), "settingsSectionTitle");
    page->addWidget(s.radioName);

    auto* copy = group(page);
    s.radioActions.append(action(copy, ui("Сохранить копию"), "radioBackupSave", SaveRadio));
    s.saveInfo = caption(copy->parentWidget(), QString());
    copy->addWidget(s.saveInfo);
    s.radioActions.append(action(copy, ui("Вернуть из копии"), "radioBackupRestore", RestoreRadio));
    copy->addWidget(caption(copy->parentWidget(), ui("На подключённое радио, из файла копии")));
    page->addWidget(caption(this, ui("Копия может содержать закрытый ключ радио, ключи каналов и сообщения. "
                                     "Храните файл как пароль. Пароли входа на другие узлы в копию не попадают.")));

    page->addWidget(caption(this, ui("ТОЛЬКО ЧАСТЬ"), "settingsSectionTitle"));
    auto* partial = group(page);
    s.radioActions.append(action(partial, ui("Сохранить ключ в файл"), "radioBackupSaveKey", SaveKey));
    s.radioActions.append(action(partial, ui("Вернуть ключ"), "radioBackupRestoreKey", RestoreKey));

    s.keyInput = new QWidget(partial->parentWidget());
    auto* keyLayout = new QVBoxLayout(s.keyInput);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->addWidget(caption(s.keyInput, ui("Выберите файл ключа или вставьте закрытый ключ.")));
    auto* open = action(keyLayout, ui("Открыть файл ключа"), "radioBackupOpenKey");
    s.keyText = new QLineEdit(s.keyInput);
    s.keyText->setObjectName(QLatin1String("radioBackupKeyText"));
    s.keyText->setEchoMode(QLineEdit::Password);
    s.keyText->setMaxLength(8192);
    s.keyText->setPlaceholderText(ui("Закрытый ключ"));
    keyLayout->addWidget(s.keyText);
    auto* check = action(keyLayout, ui("Проверить ключ"), "radioBackupCheckKey");
    connect(open, SIGNAL(clicked()), this, SLOT(openKeyFile()), Qt::QueuedConnection);
    connect(check, SIGNAL(clicked()), this, SLOT(reviewKeyText()), Qt::QueuedConnection);
    partial->addWidget(s.keyInput);
    s.keyInput->hide();

    auto* history = action(partial, ui("История чатов"), "radioBackupHistory", History);
    partial->addWidget(caption(partial->parentWidget(), ui("Локальная копия Belle и импорт истории OMMesh")));
    connect(history, SIGNAL(clicked()), this, SLOT(activateAction()), Qt::QueuedConnection);
    for (QPushButton* row : s.radioActions)
        connect(row, SIGNAL(clicked()), this, SLOT(activateAction()), Qt::QueuedConnection);

    s.feedback = caption(this, QString());
    page->addWidget(s.feedback);
    s.feedback->hide();

    s.review = new QWidget(this);
    auto* reviewLayout = new QVBoxLayout(s.review);
    reviewLayout->setContentsMargins(0, 0, 0, 0);
    s.reviewSummary = caption(s.review, QString());
    reviewLayout->addWidget(s.reviewSummary);
    auto* buttons = new QHBoxLayout;
    reviewLayout->addLayout(buttons);
    s.confirm = action(buttons, ui("Восстановить"), "radioBackupConfirm");
    auto* cancel = action(buttons, ui("Отмена"), "radioBackupCancelReview");
    connect(s.confirm, SIGNAL(clicked()), this, SLOT(applyRestore()), Qt::QueuedConnection);
    connect(cancel, SIGNAL(clicked()), this, SLOT(cancelReview()), Qt::QueuedConnection);
    page->addWidget(s.review);
    s.review->hide();
    page->addStretch(1);

    connect(&client, SIGNAL(changed()), this, SLOT(updateAvailability()), Qt::QueuedConnection);
    connect(&client, SIGNAL(radioBackupKeyResult(int,int,QByteArray,QString)),
            this, SLOT(keyResult(int,int,QByteArray,QString)), Qt::QueuedConnection);
    connect(&client, SIGNAL(radioBackupProgress(int,QString,int,int)),
            this, SLOT(progress(int,QString,int,int)), Qt::QueuedConnection);
    updateAvailability();
}

RadioBackupPanel::~RadioBackupPanel() {
    state_->reviewed.privateKey.fill(0);
    state_->reviewedBytes.fill(0);
}

void RadioBackupPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (state_->reviewReady)
        QTimer::singleShot(0, this, SLOT(revealReview()));
}

void RadioBackupPanel::revealReview() {
    if (state_->reviewReady)
        scrollTo(state_->review);
}

void RadioBackupPanel::activateAction() {
    State& s = *state_;
    const auto operation = static_cast<Action>(sender()->property("backupOperation").toInt());
    if (operation == History) {
        showHistory();
    } else if (s.online() && !s.request) {
        if (operation == RestoreKey) {
            cancelReview();
            s.keyInput->setVisible(!s.keyInput->isVisible());
            if (s.keyInput->isVisible())
                scrollTo(s.keyInput);
        } else {
            s.keyText->clear();
            s.keyInput->hide();
            openPicker(operation);
        }
    }
}

void RadioBackupPanel::openKeyFile() { openPicker(RestoreKey); }

void RadioBackupPanel::reviewKeyText() {
    State& s = *state_;
    if (!s.online() || s.request)
        return;
    const auto snapshot = s.client.radioBackupSnapshot();
    if (!snapshot)
        return;
    QByteArray data = s.keyText->text().toUtf8();
    s.keyText->clear();
    cancelReview();
    s.expectedTarget = snapshot->publicKey;
    preview(data, RestoreKey);
    data.fill(0);
}

void RadioBackupPanel::openPicker(Action operation) {
    State& s = *state_;
    if (!s.online() || s.request)
        return;
    const auto snapshot = s.client.radioBackupSnapshot();
    if (!snapshot) {
        report(ui("Дождитесь окончания загрузки данных радио."));
        return;
    }
    cancelReview();
    s.expectedTarget = snapshot->publicKey;
    s.pickerAction = operation;
    const bool saving = operation == SaveRadio || operation == SaveKey;
    QStringList filters;
    if (operation == RestoreKey)
        filters << QLatin1String("*.txt") << QLatin1String("*.key") << QLatin1String("*.json");
    else
        filters << QLatin1String("*.json");
    s.picker = new SettingsBackupFilePicker(saving, s.directory, this, filters);
    connect(s.picker.data(), SIGNAL(pathSelected(QString)), this, SLOT(choosePath(QString)));
    connect(s.picker.data(), SIGNAL(closeRequested(QWidget*)), this, SIGNAL(detailCloseRequested(QWidget*)));
    emit detailRequested(s.picker.data(), saving ? ui("Папка для копии") :
                                operation == RestoreKey ? ui("Выберите файл ключа") : ui("Выберите копию радио"));
}

void RadioBackupPanel::choosePath(const QString& path) {
    State& s = *state_;
    const auto snapshot = s.client.radioBackupSnapshot();
    if (!s.online() || !snapshot || snapshot->publicKey != s.expectedTarget) {
        report(ui("Радио сменилось или отключилось. Откройте раздел снова для текущего радио."));
        return;
    }
    const Action operation = s.pickerAction;
    if (operation == SaveRadio || operation == SaveKey) {
        s.directory = path;
        s.exporting = snapshot;
        const QDir directory(path);
        s.exportPath = directory.filePath(fileName(operation == SaveKey, directory, snapshot->identity.name));
        const int request = s.client.nextConsoleRequestId();
        if (!s.client.requestRadioBackupKey(request, s.expectedTarget)) {
            s.exporting.reset();
            report(ui("Радио занято. Повторите сохранение чуть позже."));
            return;
        }
        s.request = request;
        s.currentAction = operation;
        report(ui("Читаем ключ подключённого радио…"));
        updateAvailability();
        return;
    }
    s.directory = QFileInfo(path).absolutePath();
    QString error;
    QByteArray bytes = readCopy(path, operation == RestoreKey ? 8192 : RadioBackupArchive::MaxFileBytes, error);
    if (!bytes.isEmpty()) {
        preview(bytes, operation);
        bytes.fill(0);
    } else {
        report(error);
    }
}

void RadioBackupPanel::cancelReview() {
    State& s = *state_;
    if (s.applying)
        return;
    s.reviewed.privateKey.fill(0);
    s.reviewed = RadioBackupRestore();
    s.reviewedBytes.fill(0);
    s.reviewedBytes.clear();
    s.reviewedBelleHistory.clear();
    s.reviewReady = false;
    s.review->hide();
}

void RadioBackupPanel::updateAvailability() {
    State& s = *state_;
    const bool online = s.online();
    if (s.reviewReady && (!online || s.client.selfNode().publicKey != s.expectedTarget)) {
        cancelReview();
        report(ui("Подключение сменилось. Выберите копию заново."));
    }
    const bool ready = online && !s.client.busy() && !s.request;
    for (QPushButton* entry : s.radioActions)
        entry->setEnabled(ready);
    s.keyInput->setEnabled(ready);
    if (!online && s.keyInput->isVisible()) {
        s.keyText->clear();
        s.keyInput->hide();
    }
    s.confirm->setEnabled(ready && s.reviewReady);
    s.radioName->setText(online ? s.client.radioName() : ui("Радио не подключено"));
    if (online) {
        int channels = 0;
        for (const RadioChannel& channel : s.client.radioChannels())
            channels += !channel.key.isEmpty();
        s.saveInfo->setText(ui("Ключ при поддержке прошивки, имя и настройки. Каналов: %1 · контактов: %2 · сообщений: %3")
                            .arg(channels).arg(s.client.nodes().size()).arg(s.client.radioBackupMessageCount()));
    } else {
        s.saveInfo->setText(ui("Подключите радио для копирования его настроек, ключа и каналов."));
    }
}

void RadioBackupPanel::showHistory() {
    auto* panel = new SettingsBackupPanel(state_->client, this);
    connect(panel, SIGNAL(pickerRequested(QWidget*,QString)), this, SIGNAL(detailRequested(QWidget*,QString)));
    connect(panel, SIGNAL(pickerClosed(QWidget*)), this, SIGNAL(detailCloseRequested(QWidget*)));
    auto* scroll = makeTouchScrollArea(this, panel, true);
    panel->setObjectName(QLatin1String("pane"));
    panel->setAttribute(Qt::WA_StyledBackground);
    scroll->viewport()->setObjectName(QLatin1String("pane"));
    scroll->viewport()->setAttribute(Qt::WA_StyledBackground);
    emit detailRequested(scroll, ui("История чатов"));
}

void RadioBackupPanel::report(const QString& text) {
    state_->feedback->setText(text);
    state_->feedback->setVisible(!text.isEmpty());
}

void RadioBackupPanel::keyResult(int request, int result, const QByteArray& key, const QString& detail) {
    State& s = *state_;
    if (request != s.request || !s.exporting)
        return;
    const RadioBackupSnapshot snapshot = *s.exporting;
    s.exporting.reset();
    s.request = 0;
    const bool onlyKey = s.currentAction == SaveKey;
    const bool withoutKey = result == Client::BackupKeyUnavailable;
    QString message = detail;
    if ((result == Client::BackupKeyRead || (!onlyKey && withoutKey)) &&
            s.online() && s.client.selfNode().publicKey == snapshot.publicKey) {
        QString error;
        QByteArray history;
        int count = 0;
        QSettings settings;
        settings.setFallbacksEnabled(false);
        if (!onlyKey && !SettingsBackup::readRadioHistory(settings, snapshot.publicKey, history, count)) {
            report(ui("Локальная история повреждена. Копия не записана."));
            updateAvailability();
            return;
        }
        QByteArray content = onlyKey ? RadioBackupArchive::encodeKey(snapshot, key, error) :
                                     RadioBackupArchive::encode(snapshot, key, history, error);
        if (!content.isEmpty() && SettingsBackup::saveBytes(s.exportPath, content, error)) {
            message = withoutKey ? ui("Копия сохранена без закрытого ключа. Прошивка не разрешила его экспорт.") :
                                   onlyKey ? ui("Ключ сохранён.") : ui("Копия радио сохранена.");
            message += QLatin1Char('\n') + QDir::toNativeSeparators(s.exportPath);
        } else {
            message = error;
        }
        content.fill(0);
    } else if (message.isEmpty()) {
        message = ui("Радио не передало ключ. Проверьте подключение и поддержку прошивки.");
    }
    report(message);
    updateAvailability();
    if (s.feedback->isVisible())
        scrollTo(s.feedback);
}

void RadioBackupPanel::preview(const QByteArray& bytes, Action operation) {
    State& s = *state_;
    const auto target = s.client.radioBackupSnapshot();
    if (!target || target->publicKey != s.expectedTarget) {
        report(ui("Радио сменилось. Откройте выбор файла заново."));
        return;
    }
    RadioBackupRestore plan;
    QString error;
    QString sourceName;
    QString historyNote;
    QByteArray belleHistory, androidHistory;
    if (operation == RestoreKey) {
        RadioBackupArchive::KeyFile selected;
        if (!RadioBackupArchive::decodeKey(bytes, selected, error) ||
                !RadioBackupArchive::planKeyRestore(selected, *target, plan, error)) {
            report(error);
            return;
        }
        sourceName = selected.name;
        if (plan.sourcePublicKey == target->publicKey) {
            report(ui("На подключённом радио уже используется этот ключ."));
            s.keyInput->hide();
            scrollTo(s.feedback);
            return;
        }
    } else {
        RadioBackupArchive::Archive selected;
        int messages = 0;
        if (!RadioBackupArchive::decode(bytes, selected, error) ||
                !RadioBackupArchive::planRestore(selected, *target, RadioBackupArchive::All, plan, error)) {
            report(error);
            return;
        }
        if (!SettingsBackup::validRadioHistory(selected.belleHistory, messages)) {
            report(ui("История Belle в файле копии повреждена."));
            return;
        }
        sourceName = QString::fromUtf8(selected.source.identity.name.data(),
                                       static_cast<int>(selected.source.identity.name.size()));
        belleHistory = selected.belleHistory;
        androidHistory = selected.androidHistory;
        if (!belleHistory.isEmpty())
            historyNote = ui("\nИстория этого радио в Belle будет заменена (%1 сообщений).").arg(messages);
        else if (!androidHistory.isEmpty())
            historyNote = ui("\nИз файла будет добавлена история OMMesh (%1 сообщений до проверки совпадений).")
                          .arg(selected.messageCount);
        if (plan.privateKey.isEmpty() && target->publicKey != plan.sourcePublicKey) {
            belleHistory.clear();
            androidHistory.clear();
            historyNote = ui("\nИстория из копии относится к другому ключу радио.");
        }
    }
    const bool keyChanges = !plan.privateKey.isEmpty() && plan.sourcePublicKey != target->publicKey;
    const QString identity = sourceName.isEmpty() ? plan.sourcePublicKey.left(12) : sourceName;
    QString summary = ui("Подключённое радио: %1 (%2)\nКопия: %3 (%4)")
        .arg(s.client.radioName()).arg(target->publicKey.left(12)).arg(identity).arg(plan.sourcePublicKey.left(12));
    if (operation == RestoreRadio) {
        summary += ui("\nКаналов: %1 · контактов: %2").arg(plan.channels.size()).arg(int(plan.contacts.size()));
    }
    if (keyChanges)
        summary += ui("\nКлюч подключённого радио будет заменён. После записи радио перезапустится.");
    else if (plan.privateKey.isEmpty())
        summary += ui("\nКлюч подключённого радио сохранится.");
    summary += historyNote;

    s.reviewed = std::move(plan);
    s.reviewedBytes = androidHistory;
    s.reviewedBelleHistory = belleHistory;
    s.reviewSummary->setText(summary);
    s.reviewReady = true;
    s.keyText->clear();
    s.keyInput->hide();
    s.review->show();
    report(QString());
    updateAvailability();
    if (isVisible())
        revealReview();
}

void RadioBackupPanel::applyRestore() {
    State& s = *state_;
    if (!s.reviewReady || s.request || !s.online())
        return;
    const int request = s.client.nextConsoleRequestId();
    auto* completion = new RadioBackupCompletion(s.client, request, s.reviewed.sourcePublicKey,
                                                 s.reviewedBelleHistory, s.reviewedBytes);
    connect(completion, SIGNAL(completed(int,QString)), this, SLOT(restoreResult(int,QString)), Qt::QueuedConnection);
    if (!s.client.restoreRadioBackup(request, s.reviewed)) {
        completion->deleteLater();
        report(ui("Не удалось начать восстановление. Убедитесь, что подключено выбранное радио и оно свободно."));
        return;
    }
    s.request = request;
    s.applying = true;
    s.reviewReady = false;
    s.reviewed.privateKey.fill(0);
    s.reviewed = RadioBackupRestore();
    s.reviewedBytes.clear();
    s.reviewedBelleHistory.clear();
    s.review->hide();
    report(ui("Записываем копию на подключённое радио…"));
    updateAvailability();
    scrollTo(s.feedback);
}

void RadioBackupPanel::progress(int request, const QString& stage, int done, int total) {
    if (request != state_->request || !state_->applying)
        return;
    report(total > 0 ? ui("%1 · %2 из %3").arg(stage).arg(done).arg(total) : stage);
}

void RadioBackupPanel::restoreResult(int request, const QString& detail) {
    State& s = *state_;
    if (request != s.request || !s.applying)
        return;
    s.request = 0;
    s.applying = false;
    report(detail);
    updateAvailability();
    scrollTo(s.feedback);
}

RadioBackupCompletion::RadioBackupCompletion(Client& client, int request, const QString& sourceKey,
                                             const QByteArray& belleHistory, const QByteArray& androidHistory)
    : QObject(&client), client_(client), request_(request), sourceKey_(sourceKey),
      belleHistory_(belleHistory), androidHistory_(androidHistory) {
    connect(&client, SIGNAL(radioBackupRestored(int,bool,QString,QString)),
            this, SLOT(finish(int,bool,QString,QString)), Qt::QueuedConnection);
}

void RadioBackupCompletion::finish(int request, bool success, const QString& publicKey, const QString& detail) {
    if (request != request_)
        return;
    QString text = detail;
    if (success && (!belleHistory_.isEmpty() || !androidHistory_.isEmpty())) {
        QString error;
        bool applied = false;
        int added = 0;
        if (publicKey == sourceKey_ && client_.canLoadRadioBackupHistory(publicKey)) {
            QSettings settings;
            settings.setFallbacksEnabled(false);
            if (!belleHistory_.isEmpty()) {
                applied = SettingsBackup::restoreRadioHistory(settings, publicKey, belleHistory_, error);
            } else {
                BackupHistoryImport::Plan plan;
                if (BackupHistoryImport::preview(androidHistory_, settings, client_.radioChannels(), plan, error) &&
                        plan.radioKey == publicKey) {
                    added = plan.added;
                    applied = BackupHistoryImport::apply(plan, settings, error);
                }
            }
            if (applied)
                applied = client_.reloadRadioBackupHistory(publicKey);
        }
        text += QLatin1Char('\n');
        text += applied ? (belleHistory_.isEmpty() ? ui("Добавлено сообщений: %1").arg(added) :
                                                    ui("История радио восстановлена.")) :
                          (error.isEmpty() ? ui("Историю применить не удалось. Проверьте ключ и дождитесь окончания отправки.") : error);
    }
    emit completed(request, text);
    deleteLater();
}
