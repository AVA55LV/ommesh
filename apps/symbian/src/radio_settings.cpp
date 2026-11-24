#include "radio_settings.hpp"
#include "node_chat_options.hpp"

#include "appearance.hpp"
#include "client.hpp"
#include "demo_radio.hpp"
#include "native_echo.hpp"
#include "native_keyboard.hpp"
#include "native_vibration.hpp"
#include "node_map.hpp"
#include "phone_location.hpp"
#include "settings_attachments.hpp"
#include "settings_backup.hpp"
#include "settings_icons.hpp"
#include "settings_radio_backup.hpp"
#include "settings_radio_editor.hpp"
#include "settings_radio_tools.hpp"
#include "window.hpp"

#include <QtCore/QDateTime>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QApplication>
#include <QtGui/QButtonGroup>
#include <QtGui/QComboBox>
#include <QtGui/QFocusEvent>
#include <QtGui/QGridLayout>
#include <QtGui/QHBoxLayout>
#include <QtGui/QLabel>
#include <QtGui/QLineEdit>
#include <QtGui/QMessageBox>
#include <QtGui/QPainter>
#include <QtGui/QPushButton>
#include <QtGui/QScrollArea>
#include <QtGui/QSlider>
#include <QtGui/QTextCursor>
#include <QtGui/QTextEdit>
#include <QtGui/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QString ui(const char* utf8) {
    return QString::fromUtf8(utf8);
}

QPushButton* addSettingsButton(QBoxLayout* layout, const QString& text, QWidget* parent, const char* name = nullptr) {
    auto* button = new QPushButton(text, parent);
    if (name)
        button->setObjectName(QLatin1String(name));
    button->setMinimumHeight(40);
    layout->addWidget(button);
    return button;
}

QLabel* addText(QBoxLayout* layout, const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    layout->addWidget(label);
    return label;
}

using SettingGlyph = SettingsIcons::Kind;

class SettingRow : public QPushButton {
public:
    SettingRow(const QString& title, QWidget* parent, SettingGlyph glyph, bool navigate,
               const QString& hint = QString(), bool choice = false)
        : QPushButton(title, parent), glyph_(glyph), navigate_(navigate), hint_(hint), choice_(choice) {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(48);
        QSizePolicy policy(QSizePolicy::Ignored, hint_.isEmpty() ? QSizePolicy::Fixed : QSizePolicy::Preferred);
        policy.setHeightForWidth(!hint_.isEmpty());
        setSizePolicy(policy);
        setAutoDefault(false);
    }

    QSize sizeHint() const override { return QSize(200, 48); }
    QSize minimumSizeHint() const override { return QSize(94, 48); }
    int heightForWidth(int width) const override {
        if (hint_.isEmpty())
            return 48;
        const int available = textWidth(width);
        return qMax(48, 20 + wrappedHeight(text(), rowFont(15), available) +
                            wrappedHeight(hint_, rowFont(13), available));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const Appearance::Palette colors = Appearance::paletteFor(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const int middle = height() / 2;
        if (isDown() || (hasFocus() && focusReason_ != Qt::MouseFocusReason)) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.selection);
            painter.drawRoundedRect(rect().adjusted(2, 1, -2, -1), 8, 8);
        }
        const QColor iconColor = glyph_ == SettingGlyph::Power ? colors.danger : colors.muted;
        const QColor titleColor = isEnabled() ? colors.text :
            glyph_ == SettingGlyph::Power ? colors.danger : colors.muted;
        SettingsIcons::draw(painter, glyph_, QRect(13, middle - 10, 20, 20), iconColor);

        int right = width() - 10;
        if (isCheckable()) {
            if (choice_) {
                const QPoint indicator(right - 8, middle);
                painter.setPen(QPen(isChecked() ? colors.accent : colors.muted, 1.4));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(indicator, 8, 8);
                if (isChecked()) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(colors.accent);
                    painter.drawEllipse(indicator, 4, 4);
                }
            } else {
                painter.setPen(Qt::NoPen);
                painter.setBrush(isChecked() ? colors.accent : Appearance::mix(colors.surface, colors.muted, 45));
                painter.drawRoundedRect(QRect(right - 31, middle - 8, 32, 16), 8, 8);
                painter.setBrush(isChecked() ? colors.onAccent : colors.text);
                painter.drawEllipse(QPoint(right - (isChecked() ? 8 : 23), middle), 5, 5);
            }
            right -= choice_ ? 25 : 39;
        } else if (navigate_) {
            SettingsIcons::draw(painter, SettingGlyph::Chevron, QRect(right - 12, middle - 7, 14, 14),
                                colors.muted);
            right -= 17;
        }

        const QFont titleFont = rowFont(15);
        const int available = qMax(1, right - TextInset);
        if (!hint_.isEmpty()) {
            const int titleHeight = wrappedHeight(text(), titleFont, available);
            const int hintTop = 9 + titleHeight + 3;
            painter.setFont(titleFont);
            painter.setPen(titleColor);
            painter.drawText(QRect(TextInset, 9, available, titleHeight), Qt::TextWordWrap, text());
            painter.setFont(rowFont(13));
            painter.setPen(colors.muted);
            painter.drawText(QRect(TextInset, hintTop, available, height() - hintTop), Qt::TextWordWrap, hint_);
            drawSeparator(painter, colors);
            return;
        }
        const QString value = property("settingValue").toString();
        const QFontMetrics titleMetrics(titleFont);
        const QFont small = rowFont(12);
        const QFontMetrics smallMetrics(small);
        const int naturalValueWidth = value.isEmpty() ? 0 : smallMetrics.width(value) + 5;
        const bool stacked = naturalValueWidth && titleMetrics.width(text()) + naturalValueWidth + 7 > available;
        const int valueWidth = qMin(naturalValueWidth, available);
        if (!value.isEmpty()) {
            const QRect valueRect = stacked ? QRect(TextInset, 24, available, height() - 24)
                                            : QRect(right - valueWidth, 0, valueWidth, height());
            painter.setFont(small);
            painter.setPen(colors.muted);
            painter.drawText(valueRect, (stacked ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                             smallMetrics.elidedText(value, Qt::ElideRight, valueRect.width()));
        }
        const int titleWidth = stacked || !valueWidth ? available : qMax(0, available - valueWidth - 7);
        painter.setFont(titleFont);
        painter.setPen(titleColor);
        painter.drawText(QRect(TextInset, 0, titleWidth, stacked ? 29 : height()), Qt::AlignLeft | Qt::AlignVCenter,
                         titleMetrics.elidedText(text(), Qt::ElideRight, titleWidth));
        drawSeparator(painter, colors);
    }

    void focusInEvent(QFocusEvent* event) override {
        focusReason_ = event->reason();
        QPushButton::focusInEvent(event);
    }

private:
    enum { TextInset = 44 };

    void drawSeparator(QPainter& painter, const Appearance::Palette& colors) const {
        if (geometry().bottom() + 4 >= parentWidget()->height())
            return;
        painter.setPen(Appearance::mix(colors.surface, colors.border, 26));
        painter.drawLine(9, height() - 1, width() - 9, height() - 1);
    }

    static int wrappedHeight(const QString& text, const QFont& font, int width) {
        return QFontMetrics(font).boundingRect(0, 0, width, 0, Qt::TextWordWrap, text).height();
    }

    QFont rowFont(int pixels) const {
        QFont result = font();
        result.setPixelSize(pixels);
        return result;
    }

    int textWidth(int width) const {
        const int control = isCheckable() ? (choice_ ? 25 : 39) : navigate_ ? 17 : 0;
        return qMax(1, width - TextInset - 10 - control);
    }

    SettingGlyph glyph_;
    bool navigate_;
    QString hint_;
    bool choice_;
    Qt::FocusReason focusReason_ = Qt::OtherFocusReason;
};

class ThemeSwatch : public QPushButton {
public:
    ThemeSwatch(Appearance::Theme theme, QWidget* parent)
        : QPushButton(Appearance::themeName(theme), parent), theme_(theme) {
        setCheckable(true);
        setAutoDefault(false);
        setMinimumHeight(62);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override { return QSize(125, 62); }

protected:
    void paintEvent(QPaintEvent*) override {
        const Appearance::Palette current = Appearance::paletteFor(this);
        const Appearance::Palette swatch = Appearance::palette(theme_);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(isChecked() || hasFocus() ? current.accent : current.border, isChecked() ? 2 : 1));
        p.setBrush(isDown() || isChecked() ? current.selection : current.surface);
        p.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 10, 10);
        const int radius = 9;
        const int center = width() / 2;
        const QColor samples[] = {swatch.canvas, swatch.selection, swatch.accent};
        for (int i = 0; i < 3; ++i) {
            p.setPen(QPen(current.border, 1));
            p.setBrush(samples[i]);
            p.drawEllipse(QPoint(center + (i - 1) * 24, 19), radius, radius);
        }
        QFont caption = font();
        caption.setPixelSize(14);
        caption.setBold(isChecked());
        p.setFont(caption);
        p.setPen(current.text);
        p.drawText(QRect(5, 33, width() - 10, 24), Qt::AlignCenter,
                   QFontMetrics(caption).elidedText(text(), Qt::ElideRight, width() - 10));
    }

private:
    Appearance::Theme theme_;
};

QVBoxLayout* settingsGroup(QVBoxLayout* page, QWidget* parent, const QString& title) {
    auto* section = new QWidget(parent);
    section->setObjectName(QLatin1String("settingsSection"));
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    if (!title.isEmpty()) {
        QLabel* heading = addText(layout, title, section);
        heading->setObjectName(QLatin1String("settingsSectionTitle"));
        heading->setContentsMargins(12, 0, 0, 2);
    }
    auto* card = new QWidget(section);
    card->setObjectName(QLatin1String("settingsGroup"));
    card->setAttribute(Qt::WA_StyledBackground);
    auto* rows = new QVBoxLayout(card);
    rows->setContentsMargins(1, 2, 1, 2);
    rows->setSpacing(0);
    layout->addWidget(card);
    page->addWidget(section);
    return rows;
}

QPushButton* addSettingsRow(QBoxLayout* layout, const char* title, const char* name,
                            SettingGlyph glyph, bool navigate = true,
                            const QString& hint = QString(), bool choice = false) {
    auto* row = new SettingRow(ui(title), layout->parentWidget(), glyph, navigate, hint, choice);
    if (name)
        row->setObjectName(QLatin1String(name));
    layout->addWidget(row);
    return row;
}

void setRowValue(QPushButton* row, const QString& value) {
    if (row->property("settingValue").toString() == value)
        return;
    row->setProperty("settingValue", value);
    row->setProperty("accessibleDescription", value);
    row->update();
}

void showToggle(QPushButton* button, const char* title, bool enabled) {
    button->setText(ui(title));
    button->setProperty("accessibleDescription", enabled ? ui("Включено") : ui("Выключено"));
}

QPushButton* preferenceToggle(QBoxLayout* layout, const char* title,
                             const char* key, bool defaultValue = false,
                             SettingGlyph glyph = SettingGlyph::Notifications, const QString& hint = QString()) {
    auto* button = addSettingsRow(layout, title, key, glyph, false, hint);
    button->setCheckable(true);
    button->setChecked(QSettings().value(QLatin1String(key), defaultValue).toBool());
    showToggle(button, title, button->isChecked());
    return button;
}

QScrollArea* settingsScroll(QWidget* page, QWidget* content) {
    auto* scroll = makeTouchScrollArea(page, content);
    const QColor canvas = Appearance::paletteFor(page).canvas;
    for (QWidget* background : {content, scroll->viewport()}) {
        background->setObjectName(QLatin1String("pane"));
        background->setAttribute(Qt::WA_StyledBackground);
        QPalette palette = background->palette();
        palette.setColor(QPalette::Window, canvas);
        palette.setColor(QPalette::Base, canvas);
        background->setPalette(palette);
        background->setAutoFillBackground(true);
    }
    return scroll;
}

void connectCancel(QPushButton* button, QDialog* dialog) {
    QObject::connect(button, SIGNAL(clicked()), dialog, SLOT(reject()));
}

void showConsole(Client& client, const QString& id, const QString& title, QWidget* parent) {
    if (!client.connected()) {
        QMessageBox::information(parent, title, ui("Подключитесь к рации MeshCore."));
        return;
    }
    RadioConsoleDialog dialog(client, id, title, parent);
    dialog.exec();
}

bool hasPosition(const NetworkNode& node) {
    return (node.latitude != 0 || node.longitude != 0) &&
           node.latitude >= -90000000 && node.latitude <= 90000000 &&
           node.longitude >= -180000000 && node.longitude <= 180000000;
}

}  // namespace

class ChatAppearancePreview : public QWidget {
public:
    explicit ChatAppearancePreview(QWidget* parent) : QWidget(parent) {
        setMinimumHeight(64);
        setProperty("accessibleName", ui("Пример текста сообщения"));
    }

    void setPreview(Appearance::Theme theme, int size) {
        theme_ = theme;
        size_ = size;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const Appearance::Palette colors = Appearance::palette(theme_);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(colors.chrome);
        p.drawRoundedRect(rect(), 9, 9);
        const int innerWidth = qMax(0, width() - 36);
        QFont message = font();
        message.setPixelSize(size_);
        const QFontMetrics metrics(message);
        const QString sample = metrics.elidedText(ui("Привет! Связь есть."), Qt::ElideRight, innerWidth);
        const int bubbleWidth = qMin(width() - 18, metrics.width(sample) + 23);
        const int bubbleHeight = qMin(height() - 14, metrics.height() + 14);
        const QRect bubble(9, (height() - bubbleHeight) / 2, bubbleWidth, bubbleHeight);
        p.setBrush(colors.selection);
        p.drawRoundedRect(bubble, 9, 9);
        p.setFont(message);
        p.setPen(colors.text);
        p.drawText(bubble.adjusted(11, 0, -7, 0), Qt::AlignLeft | Qt::AlignVCenter, sample);
    }

private:
    Appearance::Theme theme_ = Appearance::Graphite;
    int size_ = Appearance::DefaultChatTextSize;
};

AppearancePanel::AppearancePanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QLatin1String("appearancePanel"));
    const Appearance::Preferences saved = Appearance::preferences();
    theme_ = saved.theme;
    chatTextSize_ = saved.chatTextSize;

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* sizeHeading = new QHBoxLayout;
    auto* title = new QLabel(ui("Размер текста в чатах"), this);
    title->setObjectName(QLatin1String("settingsSectionTitle"));
    sizeHeading->addWidget(title, 1);
    sizeValue_ = new QLabel(QString::number(chatTextSize_), this);
    sizeValue_->setObjectName(QLatin1String("appearanceSizeValue"));
    sizeHeading->addWidget(sizeValue_);
    layout->addLayout(sizeHeading);
    auto* slider = new QSlider(Qt::Horizontal, this);
    slider->setObjectName(QLatin1String("chatTextSize"));
    slider->setProperty("accessibleName", title->text());
    slider->setRange(Appearance::MinimumChatTextSize, Appearance::MaximumChatTextSize);
    slider->setPageStep(1);
    slider->setMinimumHeight(36);
    slider->setValue(chatTextSize_);
    layout->addWidget(slider);
    preview_ = new ChatAppearancePreview(this);
    preview_->setObjectName(QLatin1String("chatTextPreview"));
    preview_->setPreview(saved.theme, chatTextSize_);
    layout->addWidget(preview_);

    auto* background = settingsGroup(layout, this, QString());
    backgroundRow_ = addSettingsRow(background, "Фон чата", "defaultChatBackground", SettingGlyph::Appearance);
    setRowValue(backgroundRow_, NodeChatOptions::defaultLabel(NodeChatOptions::Background));
    connect(backgroundRow_, SIGNAL(clicked()), this, SLOT(chooseChatBackground()), Qt::QueuedConnection);

    addText(layout, ui("Темы"), this)->setObjectName(QLatin1String("settingsSectionTitle"));
    auto* content = new QWidget(this);
    content->setObjectName(QLatin1String("appearanceSwatches"));
    content->setAttribute(Qt::WA_StyledBackground);
    auto* choices = new QButtonGroup(content);
    auto* grid = new QGridLayout(content);
    grid->setContentsMargins(0, 0, 6, 0);
    grid->setHorizontalSpacing(7);
    grid->setVerticalSpacing(7);
    const Appearance::Theme themes[] = {
        Appearance::System, Appearance::Graphite, Appearance::Apricot, Appearance::Grape,
        Appearance::Pebble, Appearance::Lagoon, Appearance::Mint, Appearance::Olive,
        Appearance::Onyx, Appearance::Twilight, Appearance::Berry, Appearance::Catppuccin,
        Appearance::OneDark, Appearance::OneLight
    };
    for (int i = 0; i < int(sizeof(themes) / sizeof(themes[0])); ++i) {
        auto* entry = new ThemeSwatch(themes[i], content);
        entry->setObjectName(QLatin1String("themeOption") + QString::number(themes[i]));
        entry->setChecked(themes[i] == saved.theme);
        choices->addButton(entry, themes[i]);
        grid->addWidget(entry, i / 2, i % 2);
    }
    QScrollArea* scroll = makeTouchScrollArea(this, content);
    scroll->viewport()->setObjectName(QLatin1String("appearanceSwatches"));
    scroll->viewport()->setAttribute(Qt::WA_StyledBackground);
    layout->addWidget(scroll, 1);
    connect(choices, SIGNAL(buttonClicked(int)), this, SLOT(selectTheme(int)), Qt::QueuedConnection);
    connect(slider, SIGNAL(valueChanged(int)), this, SLOT(setChatSize(int)));
    connect(slider, SIGNAL(sliderReleased()), this, SLOT(syncPreferences()));
}

void AppearancePanel::selectTheme(int theme) {
    if (theme_ == theme)
        return;
    theme_ = theme;
    settings_.setValue(QLatin1String("ui/theme"), theme_);
    settings_.sync();
    preview_->setPreview(Appearance::checkedTheme(theme_), chatTextSize_);
    emit appearanceChanged(theme_, chatTextSize_);
}

void AppearancePanel::setChatSize(int pixels) {
    chatTextSize_ = Appearance::checkedChatTextSize(pixels);
    sizeValue_->setText(QString::number(chatTextSize_));
    preview_->setPreview(Appearance::checkedTheme(theme_), chatTextSize_);
    settings_.setValue(QLatin1String("ui/chatTextSize"), chatTextSize_);
    emit appearanceChanged(theme_, chatTextSize_);
}

void AppearancePanel::syncPreferences() {
    settings_.sync();
}

void AppearancePanel::chooseChatBackground() {
    if (NodeChatOptions::chooseDefault(NodeChatOptions::Background, this))
        setRowValue(backgroundRow_, NodeChatOptions::defaultLabel(NodeChatOptions::Background));
}

RadioSettingsPanel::RadioSettingsPanel(Client& client, QWidget* parent)
    : QWidget(parent), client_(client), vibration_(new NativeVibration), phoneLocation_(new PhoneLocation(client_, this)) {
    vibrationRetryTimer_.setSingleShot(true);
    connect(&vibrationRetryTimer_, SIGNAL(timeout()), this, SLOT(retryTestVibration()));
    connect(phoneLocation_, SIGNAL(changed()), this, SLOT(updatePrivacyControls()));
    connect(phoneLocation_, SIGNAL(positionWritten(bool)), this, SLOT(phonePositionWritten(bool)));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    auto* radio = settingsGroup(layout, this, ui("ЭТО РАДИО"));
    nameRow_ = addSettingsRow(radio, "Имя", "radioNameSetting", SettingGlyph::Name);
    frequencyRow_ = addSettingsRow(radio, "Частота и мощность", "radioParameters", SettingGlyph::Frequency);
    batteryRow_ = addSettingsRow(radio, "Показания", "radioBattery", SettingGlyph::Readings);
    contactsRow_ = addSettingsRow(radio, "Контакты", "radioContacts", SettingGlyph::Contacts);
    locationRow_ = addSettingsRow(radio, "Приватность", "radioLocation", SettingGlyph::Privacy);
    locationRow_->setProperty("settingKeywords", ui("местоположение координаты"));
    auto* advanced = addSettingsRow(radio, "Дополнительно", "advancedSettings", SettingGlyph::Advanced);
    advanced->setProperty("settingKeywords",
        ui("консоль ноды каналы карта диагностика сведения обновить узлы получить сообщения демо выход"));
    connect(nameRow_, SIGNAL(clicked()), this, SLOT(showIdentity()), Qt::QueuedConnection);
    connect(frequencyRow_, SIGNAL(clicked()), this, SLOT(showParameters()), Qt::QueuedConnection);
    connect(batteryRow_, SIGNAL(clicked()), this, SLOT(showBattery()), Qt::QueuedConnection);
    connect(contactsRow_, SIGNAL(clicked()), this, SLOT(showContacts()), Qt::QueuedConnection);
    connect(locationRow_, SIGNAL(clicked()), this, SLOT(showLocation()), Qt::QueuedConnection);
    connect(advanced, SIGNAL(clicked()), this, SLOT(showAdvanced()), Qt::QueuedConnection);

    auto* app = settingsGroup(layout, this, ui("ЭТО ПРИЛОЖЕНИЕ"));
    notificationsRow_ = addSettingsRow(app, "Уведомления", "notificationSettings", SettingGlyph::Notifications);
    notificationsRow_->setProperty("settingKeywords", ui("не беспокоить вибрация проверить вибрацию"));
    messagesRow_ = addSettingsRow(app, "Сообщения и маршруты", "messagePreferences", SettingGlyph::Messages);
    messagesRow_->setProperty("settingKeywords",
        ui("клавиатура ввод непрочитанные заблокированные буквы кириллица превью ссылки повторы попытки отправка"));
    attachmentsRow_ = addSettingsRow(app, "Вложения", "attachmentsSettings", SettingGlyph::Attachments);
    connect(attachmentsRow_, SIGNAL(clicked()), this, SLOT(showAttachments()), Qt::QueuedConnection);
    auto* backup = addSettingsRow(app, "Копия и перенос", "backupSettings", SettingGlyph::Backup);
    connect(backup, SIGNAL(clicked()), this, SLOT(showBackup()), Qt::QueuedConnection);
    appearanceRow_ = addSettingsRow(app, "Оформление", "appearanceSettings", SettingGlyph::Appearance);
    appearanceRow_->setProperty("settingKeywords", ui("темы тема цвет размер текста шрифт"));
    connectionRow_ = addSettingsRow(app, "Подключение", "connectionSettings", SettingGlyph::Connection);
    connectionRow_->setProperty("settingKeywords", ui("автоподключение восстанавливать wifi wi-fi wi‑fi tcp мост"));
    connect(notificationsRow_, SIGNAL(clicked()), this, SLOT(showNotifications()), Qt::QueuedConnection);
    connect(messagesRow_, SIGNAL(clicked()), this, SLOT(showMessagePreferences()), Qt::QueuedConnection);
    connect(appearanceRow_, SIGNAL(clicked()), this, SLOT(showAppearance()), Qt::QueuedConnection);
    connect(connectionRow_, SIGNAL(clicked()), this, SIGNAL(connectionSettingsRequested()));

    auto* tools = settingsGroup(layout, this, QString());
    auto* air = addSettingsRow(tools, "В эфире", "airSettings", SettingGlyph::Air);
    setRowValue(air, ui("Слушать"));
    connect(air, SIGNAL(clicked()), this, SLOT(showAir()), Qt::QueuedConnection);
    auto* journal = addSettingsRow(tools, "Журнал", "radioLog", SettingGlyph::Log);
    connect(journal, SIGNAL(clicked()), this, SLOT(showLog()), Qt::QueuedConnection);
    auto* maintenance = addSettingsRow(tools, "Перезагрузка, сброс, стирание", "maintenanceSettings",
                                      SettingGlyph::Power);
    connect(maintenance, SIGNAL(clicked()), this, SLOT(showMaintenance()), Qt::QueuedConnection);
    auto* about = addSettingsRow(tools, "О приложении", "aboutSettings", SettingGlyph::About);
    setRowValue(about, QApplication::applicationVersion());
    connect(about, SIGNAL(clicked()), this, SLOT(showAbout()), Qt::QueuedConnection);

    const QSettings preferences;
    client_.setAutoReconnect(preferences.value(QLatin1String("ui/autoReconnect"), true).toBool());
    client_.setWlanRecovery(preferences.value(QLatin1String("ui/wlanRecovery"), false).toBool());
    vibrateOnMessage_ = preferences.value(QLatin1String("ui/vibration"), false).toBool();
    connect(&client_, SIGNAL(changed()), this, SLOT(updateStatus()), Qt::QueuedConnection);
    connect(&client_, SIGNAL(radioSettingsResult(int,bool,QString)),
            this, SLOT(finishPrivacySettings(int,bool,QString)), Qt::QueuedConnection);
    connect(&client_, SIGNAL(radioSettingsResult(int,bool,QString)),
            this, SLOT(clockResult(int,bool,QString)), Qt::QueuedConnection);
    connect(&client_, SIGNAL(incomingChatMessage(QString,QString,bool)),
            this, SLOT(onIncomingMessage(QString,QString,bool)), Qt::QueuedConnection);
    updateStatus();
}

RadioSettingsPanel::~RadioSettingsPanel() {
    delete vibration_;
}

bool RadioSettingsPanel::filter(const QString& query) {
    bool any = false;
    const QList<QWidget*> sections = findChildren<QWidget*>(QLatin1String("settingsSection"));
    for (QWidget* section : sections) {
        const QLabel* heading = section->findChild<QLabel*>(QLatin1String("settingsSectionTitle"));
        bool visible = false;
        const bool groupMatches = heading && heading->text().contains(query, Qt::CaseInsensitive);
        const QList<QPushButton*> rows = section->findChildren<QPushButton*>();
        for (QPushButton* row : rows) {
            const bool match = groupMatches || row->text().contains(query, Qt::CaseInsensitive) ||
                row->property("settingKeywords").toString().contains(query, Qt::CaseInsensitive) ||
                row->property("settingValue").toString().contains(query, Qt::CaseInsensitive);
            row->setVisible(match);
            visible |= match;
        }
        section->setVisible(visible);
        any |= visible;
    }
    return any;
}

QWidget* RadioSettingsPanel::makeConnectionPreferences(QWidget* parent) {
    auto* panel = new QWidget(parent);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* rows = settingsGroup(layout, panel, ui("ПЕРЕПОДКЛЮЧЕНИЕ"));
    auto* reconnect = preferenceToggle(rows, "Автоподключение", "ui/autoReconnect", true,
                                       SettingGlyph::Reconnect);
    reconnect->setObjectName(QLatin1String("autoReconnect"));
    connect(reconnect, SIGNAL(toggled(bool)), this, SLOT(setReconnect(bool)), Qt::QueuedConnection);
    auto* wlan = preferenceToggle(rows, "Восстанавливать Wi‑Fi", "ui/wlanRecovery", false,
                                  SettingGlyph::Wlan,
                                  ui("При автоподключении Nokia запрашивает сохранённую точку доступа."));
    connect(wlan, SIGNAL(toggled(bool)), this, SLOT(setUserPreference(bool)), Qt::QueuedConnection);
    connectionFeedback_ = addText(layout, QString(), panel);
    connectionFeedback_->setObjectName(QLatin1String("error"));
    connectionFeedback_->hide();
    return panel;
}

void RadioSettingsPanel::setReconnect(bool enabled) {
    if (auto* row = qobject_cast<QPushButton*>(sender()))
        showToggle(row, "Автоподключение", enabled);
    saveToggle("ui/autoReconnect", enabled, connectionFeedback_);
    client_.setAutoReconnect(enabled);
}

void RadioSettingsPanel::setVibration(bool enabled) {
    vibrateOnMessage_ = enabled;
    if (auto* row = qobject_cast<QPushButton*>(sender()))
        showToggle(row, "Вибрация при сообщении", enabled);
    saveToggle("ui/vibration", enabled, notificationFeedback_);
}

void RadioSettingsPanel::saveToggle(const char* key, bool enabled, QLabel* feedback) {
    client_.traceConnection(key, enabled);
    QSettings preferences;
    preferences.setValue(QLatin1String(key), enabled);
    preferences.sync();
    client_.traceConnection("UI: settings saved, status", preferences.status());
    if (feedback) {
        const bool failed = preferences.status() != QSettings::NoError;
        if (failed)
            feedback->setText(ui("Настройка действует до выхода. Сохранить её на телефоне не удалось."));
        feedback->setVisible(failed);
    }
    updateStatus();
}

void RadioSettingsPanel::onIncomingMessage(const QString& target, const QString& text, bool room) {
    if (!QApplication::activeWindow() || (lastNotification_.isValid() && lastNotification_.elapsed() < 1200))
        return;
    const QSettings settings;
    if (!NodeChatOptions::allowsNotification(settings, target, room, text, client_.radioName()))
        return;
    lastNotification_.start();
    if (vibrateOnMessage_)
        vibration_->pulse();
    NodeChatOptions::playNotification(settings, target);
}

void RadioSettingsPanel::testVibration() {
    if (!notificationFeedback_ || vibrationRetryTimer_.isActive())
        return;
    client_.traceConnection("UI: test vibration");
    lastNotification_.start();
    const int result = vibration_->pulse();
    client_.traceConnection("UI: vibration result", result);
    if (result != NativeVibration::SystemLocked) {
        showVibrationResult(notificationFeedback_.data(), result);
        return;
    }
    vibrationRetryFeedback_ = notificationFeedback_;
    notificationFeedback_->setText(ui("Телефон временно заблокировал вибрацию. Повторяю проверку…"));
    notificationFeedback_->show();
    vibrationRetryTimer_.start(1400);
}

void RadioSettingsPanel::retryTestVibration() {
    const QPointer<QLabel> feedback = vibrationRetryFeedback_;
    vibrationRetryFeedback_ = nullptr;
    if (!feedback || feedback.data() != notificationFeedback_.data() || !feedback->isVisible())
        return;
    if (!QApplication::activeWindow()) {
        showVibrationResult(feedback.data(), NativeVibration::SystemLocked);
        return;
    }
    lastNotification_.start();
    const int result = vibration_->pulse();
    client_.traceConnection("UI: vibration retry result", result);
    showVibrationResult(feedback.data(), result);
}

void RadioSettingsPanel::showVibrationResult(QLabel* feedback, int result) {
    QString text;
    if (result == 0)
        text = ui("Телефон принял запрос на вибрацию.");
    else if (result == NativeVibration::SystemLocked)
        text = ui("Телефон блокирует вибрацию (−22). Попробуйте ещё раз чуть позже, без подключённых аксессуаров или зарядки.");
    else if (result == NativeVibration::ProfileDisabled)
        text = ui("Включите вибросигнал в активном профиле телефона (−21).");
    else
        text = ui("Телефон не принял запрос на вибрацию (%1).").arg(result);
    feedback->setText(text);
    feedback->show();
}

void RadioSettingsPanel::refreshMessages() {
    if (client_.connected())
        client_.requestMessages();
    else
        QMessageBox::information(this, ui("Сообщения"), ui("Подключитесь к рации MeshCore."));
}

void RadioSettingsPanel::refreshInventory() {
    if (client_.connected() && !client_.busy())
        client_.refresh();
}

void RadioSettingsPanel::showInfo() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    liveReadout_ = addText(layout, client_.radioInfo(), page);
    liveReadout_->setObjectName(QLatin1String("radioReadout"));
    layout->addStretch(1);
    emit detailRequested(settingsScroll(this, page), ui("Сведения о ноде"));
}
void RadioSettingsPanel::showIdentity() {
    emit detailRequested(settingsScroll(this, new RadioEditorPanel(client_, RadioEditorPanel::Name, this)), ui("Имя"));
}

void RadioSettingsPanel::showParameters() {
    auto* panel = new RadioEditorPanel(client_, RadioEditorPanel::Frequency, this);
    emit detailRequested(settingsScroll(this, panel), ui("Частота и мощность"));
}

void RadioSettingsPanel::showContacts() {
    auto* panel = new RadioEditorPanel(client_, RadioEditorPanel::Contacts, this);
    emit detailRequested(settingsScroll(this, panel), ui("Контакты"));
}

void RadioSettingsPanel::showAttachments() {
    auto* panel = new AttachmentSettingsPanel(this);
    connect(panel, SIGNAL(saved()), this, SLOT(updateStatus()), Qt::QueuedConnection);
    emit detailRequested(settingsScroll(this, panel), ui("Вложения"));
}

void RadioSettingsPanel::showBackup() {
    auto* panel = new RadioBackupPanel(client_, this);
    connect(panel, SIGNAL(detailRequested(QWidget*,QString)), this, SIGNAL(detailRequested(QWidget*,QString)));
    connect(panel, SIGNAL(detailCloseRequested(QWidget*)), this, SIGNAL(detailCloseRequested(QWidget*)));
    emit detailRequested(settingsScroll(this, panel), ui("Копия и перенос"));
}

void RadioSettingsPanel::showAir() {
    emit detailRequested(settingsScroll(this, new RadioAirPanel(client_, this)), ui("В эфире"));
}

void RadioSettingsPanel::showMaintenance() {
    auto* panel = new RadioMaintenancePanel(client_, this);
    emit detailRequested(settingsScroll(this, panel), ui("Перезагрузка, сброс, стирание"));
}
namespace {

QString accessLabel(int mode);

std::optional<qint32> coordinate(const QLineEdit* field, int limit) {
    bool valid = false;
    const double value = field->text().trimmed().replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&valid);
    if (!valid || !std::isfinite(value) || value < -limit || value > limit)
        return std::nullopt;
    return static_cast<qint32>(qRound64(value * 1000000.0));
}

template <class Field>
Field* privacyField(QVBoxLayout* group, const char* text, const char* name) {
    auto* field = new Field(group->parentWidget());
    addText(group, ui(text), group->parentWidget())->setContentsMargins(8, 6, 8, 2);
    field->setObjectName(QLatin1String(name));
    field->setMinimumHeight(40);
    group->addWidget(field);
    return field;
}

} // namespace

void RadioSettingsPanel::showLocation() {
    auto* page = new QWidget(this);
    privacyPhonePage_ = page;
    page->installEventFilter(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* coords = settingsGroup(layout, page, ui("МЕСТОПОЛОЖЕНИЕ"));
    privacyPhoneFollow_ = addSettingsRow(coords, "Брать с телефона", "privacyPhoneFollow", SettingGlyph::Map, false,
        ui("При смещении больше 50 м. Не чаще раза в 5 минут, пока приложение на экране."));
    privacyPhoneFollow_->setCheckable(true);
    connect(privacyPhoneFollow_.data(), SIGNAL(clicked(bool)), this, SLOT(setPhoneFollow(bool)));
    privacyLatitude_ = privacyField<QLineEdit>(coords, "Широта", "privacyLatitude");
    privacyLongitude_ = privacyField<QLineEdit>(coords, "Долгота", "privacyLongitude");
    for (QLineEdit* field : {privacyLatitude_.data(), privacyLongitude_.data()}) {
        field->setMaxLength(16);
        field->setInputMethodHints(Qt::ImhFormattedNumbersOnly);
        connect(field, SIGNAL(textChanged(QString)), this, SLOT(updatePrivacyControls()));
    }
    privacyPhoneLocate_ = addSettingsRow(coords, "Взять координаты телефона", "privacyPhoneLocate", SettingGlyph::Map, false);
    connect(privacyPhoneLocate_.data(), SIGNAL(clicked()), this, SLOT(locatePhone()));
    privacyPhoneFeedback_ = addText(coords, QString(), coords->parentWidget());
    privacyPhoneFeedback_->setObjectName(QLatin1String("privacyPhoneFeedback"));
    privacyPhoneFeedback_->setContentsMargins(12, 4, 12, 6);
    privacyShare_ = addSettingsRow(coords, "Место на карте у других", "privacyShare", SettingGlyph::Map, false);
    privacyShare_->setCheckable(true);
    connect(privacyShare_.data(), SIGNAL(toggled(bool)), this, SLOT(updatePrivacyControls()));
    auto* access = settingsGroup(layout, page, ui("ПО ЗАПРОСУ"));
    privacyBase_ = privacyField<QComboBox>(access, "Кто может запросить показания", "privacyReadingsAccess");
    privacySensors_ = privacyField<QComboBox>(access, "Кто может запросить датчики", "privacySensorsAccess");
    for (int mode = 0; mode < 3; ++mode) {
        privacyBase_->addItem(accessLabel(mode));
        privacySensors_->addItem(accessLabel(mode));
    }
    connect(privacyBase_.data(), SIGNAL(currentIndexChanged(int)), this, SLOT(updatePrivacyControls()));
    connect(privacySensors_.data(), SIGNAL(currentIndexChanged(int)), this, SLOT(updatePrivacyControls()));
    privacyTrusted_ = addSettingsRow(access, "Доверенные", "privacyTrusted", SettingGlyph::Contacts);
    connect(privacyTrusted_.data(), SIGNAL(clicked()), this, SLOT(showTrustedContacts()), Qt::QueuedConnection);
    privacySave_ = addSettingsButton(layout, ui("Сохранить на рации"), page, "privacyApply");
    privacyFeedback_ = addText(layout, QString(), page);
    privacyFeedback_->setObjectName(QLatin1String("privacyFeedback"));
    connect(privacySave_.data(), SIGNAL(clicked()), this, SLOT(applyPrivacySettings()), Qt::QueuedConnection);
    privacyRadioKey_.clear();
    updatePrivacyControls();
    layout->addStretch(1);
    emit detailRequested(settingsScroll(this, page), ui("Приватность"));
}

bool RadioSettingsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == privacyPhonePage_.data() && event->type() == QEvent::Hide)
        phoneLocation_->cancelUserRequest();
    return QWidget::eventFilter(watched, event);
}

void RadioSettingsPanel::locatePhone() { phoneLocation_->locate(); }

void RadioSettingsPanel::setPhoneFollow(bool enabled) {
    phoneLocation_->setFollowing(enabled);
    updatePrivacyControls();
}

void RadioSettingsPanel::displayPrivacyPosition(qint32 latitude, qint32 longitude) {
    displayedLatitude_ = latitude;
    displayedLongitude_ = longitude;
    const bool latitudeBlocked = privacyLatitude_->blockSignals(true);
    const bool longitudeBlocked = privacyLongitude_->blockSignals(true);
    privacyLatitude_->setText(QString::number(latitude / 1000000.0, 'f', 6));
    privacyLongitude_->setText(QString::number(longitude / 1000000.0, 'f', 6));
    privacyLatitude_->blockSignals(latitudeBlocked);
    privacyLongitude_->blockSignals(longitudeBlocked);
}

void RadioSettingsPanel::phonePositionWritten(bool manual) {
    if (!privacySave_ || !privacyLatitude_ || !privacyLongitude_ || privacyRadioKey_ != client_.selfNode().publicKey)
        return;
    if (manual || (coordinate(privacyLatitude_, 90) == displayedLatitude_ &&
                   coordinate(privacyLongitude_, 180) == displayedLongitude_)) {
        const auto& identity = client_.radioIdentity();
        displayPrivacyPosition(identity.latitude, identity.longitude);
    }
    updatePrivacyControls();
}

void RadioSettingsPanel::updatePrivacyControls() {
    if (!privacySave_)
        return;
    const bool online = client_.connected();
    const bool supported = client_.radioProtocolVersion() >= 5;
    const auto& identity = client_.radioIdentity();
    if (!online)
        privacyRadioKey_.clear();
    const QString key = client_.selfNode().publicKey;
    if (online && privacyRadioKey_ != key) {
        privacyRadioKey_ = key;
        QWidget* fields[] = {privacyShare_.data(), privacyBase_.data(), privacySensors_.data()};
        for (QWidget* field : fields)
            field->blockSignals(true);
        displayPrivacyPosition(identity.latitude, identity.longitude);
        privacyShare_->setChecked(identity.advertLocPolicy != 0);
        privacyBase_->setCurrentIndex(qMin<int>(2, identity.telemetryModes & 3));
        privacySensors_->clear();
        const int base = privacyBase_->currentIndex();
        for (int mode = 0; mode <= base; ++mode)
            privacySensors_->addItem(accessLabel(mode));
        privacySensors_->setCurrentIndex(qMin<int>(base, (identity.telemetryModes >> 4) & 3));
        for (QWidget* field : fields)
            field->blockSignals(false);
    }
    const int base = privacyBase_->currentIndex();
    if (privacySensors_->count() != base + 1) {
        const int old = privacySensors_->currentIndex();
        privacySensors_->blockSignals(true);
        privacySensors_->clear();
        for (int mode = 0; mode <= base; ++mode)
            privacySensors_->addItem(accessLabel(mode));
        privacySensors_->setCurrentIndex(qBound(0, old, base));
        privacySensors_->blockSignals(false);
    }
    const auto latitude = coordinate(privacyLatitude_, 90);
    const auto longitude = coordinate(privacyLongitude_, 180);
    const bool changedPosition = latitude && longitude && (*latitude != identity.latitude || *longitude != identity.longitude);
    const bool changedAccess = supported && (privacyShare_->isChecked() != (identity.advertLocPolicy != 0) ||
        base != qMin<int>(2, identity.telemetryModes & 3) ||
        privacySensors_->currentIndex() != qMin<int>(base, (identity.telemetryModes >> 4) & 3));
    const bool phonePending = phoneLocation_->userRequestPending();
    const bool idle = online && !privacyRequest_ && !phonePending;
    const bool usePhone = phoneLocation_->available() && !privacyRequest_;
    const bool following = phoneLocation_->following();
    const bool ownGps = phoneLocation_->radioGpsEnabled();
    privacyPhoneFollow_->setChecked(following);
    showToggle(privacyPhoneFollow_, "Брать с телефона", following);
    privacyPhoneFollow_->setEnabled(usePhone && (following || (!phonePending && !ownGps)));
    privacyPhoneLocate_->setEnabled(usePhone && !phonePending);
    const QString phoneFeedback = ownGps && phoneLocation_->message().isEmpty()
        ? ui("Нода использует свой GPS.") : phoneLocation_->message();
    privacyPhoneFeedback_->setText(phoneFeedback);
    privacyPhoneFeedback_->setVisible(!phoneFeedback.isEmpty());
    privacyLatitude_->setEnabled(idle);
    privacyLongitude_->setEnabled(idle);
    privacyShare_->setEnabled(idle && supported);
    privacyBase_->setEnabled(idle && supported);
    privacySensors_->setEnabled(idle && supported && base > 0);
    privacyTrusted_->setEnabled(idle && supported && !changedPosition && !changedAccess);
    setRowValue(privacyTrusted_, supported ? QString::number(client_.trustedContactKeys().size()) : ui("—"));
    privacySave_->setEnabled(idle && latitude && longitude && (changedPosition || changedAccess));
    privacyFeedback_->setText(!online ? ui("Подключите рацию.") :
        privacyRequest_ ? ui("Сохраняю на рации…") :
        !latitude || !longitude ? ui("Допустимые координаты: широта ±90°, долгота ±180°.") :
        !supported ? ui("Эта прошивка не поддерживает настройки доступа к показаниям.") :
        privacyFeedback_->property("result").toString());
}

void RadioSettingsPanel::applyPrivacySettings() {
    if (!privacySave_ || !privacySave_->isEnabled())
        return;
    const auto latitude = coordinate(privacyLatitude_, 90);
    const auto longitude = coordinate(privacyLongitude_, 180);
    if (!latitude || !longitude)
        return;
    const auto& identity = client_.radioIdentity();
    std::vector<meshcore::Bytes> commands;
    if (client_.radioProtocolVersion() >= 5) {
        const bool share = privacyShare_->isChecked();
        const int base = privacyBase_->currentIndex();
        const int sensors = privacySensors_->currentIndex();
        const bool changedAccess = base != qMin<int>(2, identity.telemetryModes & 3) ||
                                   sensors != qMin<int>(base, (identity.telemetryModes >> 4) & 3);
        const auto modes = changedAccess
            ? static_cast<std::uint8_t>((identity.telemetryModes & 0xc0) | (sensors << 4) |
                (qMin<int>(base, (identity.telemetryModes >> 2) & 3) << 2) | base)
            : identity.telemetryModes;
        const auto visibility = share == (identity.advertLocPolicy != 0)
            ? identity.advertLocPolicy : static_cast<std::uint8_t>(share ? 1 : 0);
        if (modes != identity.telemetryModes || visibility != identity.advertLocPolicy)
            commands.push_back({38, identity.manualAddContacts, modes, visibility, identity.multiAcks});
    }
    if (*latitude != identity.latitude || *longitude != identity.longitude)
        commands.push_back(meshcore::setLocation(*latitude, *longitude));
    const int request = client_.nextConsoleRequestId();
    if (!commands.empty() && client_.applyRadioSettings(request, commands)) {
        privacyRequest_ = request;
        privacyFeedback_->setProperty("result", QString());
    } else {
        privacyFeedback_->setProperty("result", ui("Не удалось поставить изменение в очередь."));
    }
    updatePrivacyControls();
}

void RadioSettingsPanel::showTrustedContacts() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* rows = settingsGroup(layout, page, ui("ДОВЕРЕННЫЕ"));
    trustedContacts_ = privacyField<QComboBox>(rows, "Контакт", "privacyContact");
    trustedChange_ = addSettingsButton(rows, QString(), rows->parentWidget(), "privacyTrustChange");
    trustedFeedback_ = addText(layout, QString(), page);
    connect(trustedContacts_.data(), SIGNAL(currentIndexChanged(int)), this, SLOT(updateTrustedContactSelection()));
    connect(trustedChange_.data(), SIGNAL(clicked()), this, SLOT(setSelectedTrustedContact()), Qt::QueuedConnection);
    connect(addSettingsButton(layout, ui("К приватности"), page, "privacyReturn"), SIGNAL(clicked()),
            this, SLOT(showLocation()), Qt::QueuedConnection);
    refreshTrustedContacts();
    layout->addStretch(1);
    emit detailRequested(settingsScroll(this, page), ui("Доверенные"));
}

void RadioSettingsPanel::refreshTrustedContacts() {
    if (!trustedContacts_)
        return;
    const QSet<QString> trusted = client_.trustedContactKeys();
    const QString current = trustedContacts_->itemData(trustedContacts_->currentIndex()).toString();
    QStringList keys;
    QStringList labels;
    if (client_.connected()) {
        const QVector<NetworkNode> nodes = client_.nodes();
        for (const NetworkNode& node : nodes) {
            const bool allowed = trusted.contains(node.publicKey);
            if (node.publicKey.isEmpty() || (node.role != 1 && !allowed))
                continue;
            keys.append(node.publicKey);
            labels.append((allowed ? ui("✓ ") : QString()) + node.title + ui(" · ") + node.publicKey.left(8));
        }
    }
    if (keys != trustedContacts_->property("keys").toStringList() ||
        labels != trustedContacts_->property("labels").toStringList()) {
        trustedContacts_->blockSignals(true);
        trustedContacts_->clear();
        for (int index = 0; index < keys.size(); ++index)
            trustedContacts_->addItem(labels[index], keys[index]);
        trustedContacts_->setCurrentIndex(qMax(0, keys.indexOf(current)));
        trustedContacts_->setProperty("keys", keys);
        trustedContacts_->setProperty("labels", labels);
        trustedContacts_->blockSignals(false);
    }
    updateTrustedContactSelection();
}

void RadioSettingsPanel::updateTrustedContactSelection() {
    if (!trustedChange_)
        return;
    const QString key = trustedContacts_->itemData(trustedContacts_->currentIndex()).toString();
    const bool allowed = client_.trustedContactKeys().contains(key);
    const bool active = client_.radioProtocolVersion() >= 5 && !key.isEmpty() && !trustRequest_;
    trustedChange_->setText(allowed ? ui("Убрать из доверенных") : ui("Добавить в доверенные"));
    trustedChange_->setEnabled(active);
    trustedContacts_->setEnabled(active);
    trustedFeedback_->setText(trustRequest_ ? ui("Сохраняю на рации…") :
        !client_.connected() ? ui("Подключите рацию.") :
        key.isEmpty() ? ui("Подходящих контактов пока нет.") : trustedFeedback_->property("result").toString());
}

void RadioSettingsPanel::setSelectedTrustedContact() {
    if (!trustedChange_ || !trustedChange_->isEnabled())
        return;
    const QString key = trustedContacts_->itemData(trustedContacts_->currentIndex()).toString();
    const int id = client_.nextConsoleRequestId();
    if (client_.setTrustedContact(id, key, !client_.trustedContactKeys().contains(key)))
        trustRequest_ = id;
    else
        trustedFeedback_->setProperty("result", ui("Не удалось поставить изменение в очередь."));
    updateTrustedContactSelection();
}

void RadioSettingsPanel::finishPrivacySettings(int requestId, bool success, const QString& status) {
    if (requestId == privacyRequest_) {
        privacyRequest_ = 0;
        if (privacyFeedback_) {
            if (success)
                privacyRadioKey_.clear();
            privacyFeedback_->setProperty("result", status);
            updatePrivacyControls();
        }
    } else if (requestId == trustRequest_) {
        trustRequest_ = 0;
        if (trustedFeedback_) {
            trustedFeedback_->setProperty("result", status);
            refreshTrustedContacts();
        }
    }
}
void RadioSettingsPanel::showBattery() {
    auto* page = new QWidget(this);
    batteryPage_ = page;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    batteryState_ = addText(layout, QString(), page);
    batteryState_->setObjectName(QLatin1String("readingsStatus"));
    auto* tiles = new QGridLayout;
    tiles->setSpacing(6);
    layout->addLayout(tiles);
    const char* titles[] = {"Батарея", "Шум", "В эфире", "Работает", "Температура платы", "Передача / приём"};
    const char* names[] = {"readingsBattery", "readingsNoise", "readingsAir", "readingsUptime",
                           "readingsTemperature", "readingsTxRx"};
    for (int i = 0; i < 6; ++i) {
        auto* tile = new QWidget(page);
        tile->setObjectName(QLatin1String("settingsGroup"));
        tile->setAttribute(Qt::WA_StyledBackground);
        auto* fields = new QVBoxLayout(tile);
        fields->setContentsMargins(10, 8, 10, 9);
        addText(fields, ui(titles[i]), tile)->setObjectName(QLatin1String("muted"));
        QLabel* value = addText(fields, ui("—"), tile);
        value->setObjectName(QLatin1String(names[i]));
        value->setStyleSheet(QLatin1String("font-size: 21px; font-weight: bold;"));
        tiles->addWidget(tile, i / 2, i % 2);
    }
    batteryRefresh_ = addSettingsButton(layout, ui("Обновить"), page, "refreshBattery");
    connect(batteryRefresh_.data(), SIGNAL(clicked()), this, SLOT(refreshBatteryReadout()), Qt::QueuedConnection);
    auto* privacy = settingsGroup(layout, page, QString());
    batteryPrivacy_ = addSettingsRow(privacy, "Кто может запросить показания", "readingsPrivacy", SettingGlyph::Privacy);
    connect(batteryPrivacy_.data(), SIGNAL(clicked()), this, SLOT(showLocation()), Qt::QueuedConnection);
    layout->addStretch(1);
    auto* timer = new QTimer(page);
    connect(timer, SIGNAL(timeout()), this, SLOT(refreshBatteryReadout()));
    timer->start(30000);
    refreshBatteryReadout();
    emit detailRequested(settingsScroll(this, page), ui("Показания"));
}

void RadioSettingsPanel::refreshBatteryReadout() {
    if (client_.connected())
        client_.requestRadioReadings();
    updateBatteryPage();
}

namespace {

QString durationReading(quint32 seconds) {
    if (seconds < 60)
        return ui("%1 с").arg(seconds);
    if (seconds < 3600)
        return ui("%1 мин").arg(seconds / 60);
    return ui("%1 ч %2 мин").arg(seconds / 3600).arg(seconds / 60 % 60);
}

QString accessLabel(int mode) {
    const char* labels[] = {"Никто", "Доверенные", "Все"};
    return ui(labels[qBound(0, mode, 2)]);
}

} // namespace

void RadioSettingsPanel::updateBatteryPage() {
    if (!batteryPage_)
        return;
    const bool online = client_.connected();
    const bool pending = client_.radioReadingsPending();
    const auto& values = client_.radioReadings();
    const QString empty = ui("—");
    const auto set = [&](const char* name, const QString& value) {
        if (auto* label = batteryPage_->findChild<QLabel*>(QLatin1String(name)))
            label->setText(value);
    };
    const auto decimal = [](double value, int digits) {
        return QString::number(value, 'f', digits).replace(QLatin1Char('.'), QLatin1Char(','));
    };
    batteryState_->setText(!online ? ui("Подключите рацию.") :
        pending ? ui("Запрашиваю показания…") : values.capturedAt
            ? ui("Обновлено в %1").arg(QDateTime::fromTime_t(values.capturedAt).toString(QLatin1String("HH:mm:ss")))
            : ui("Рация пока не вернула показания."));
    set("readingsBattery", values.batteryMillivolts ? ui("%1 В").arg(decimal(*values.batteryMillivolts / 1000.0, 2)) : empty);
    set("readingsNoise", values.noiseDbm ? ui("%1 дБм").arg(*values.noiseDbm) : empty);
    set("readingsAir", values.noiseDbm && values.uptimeSeconds && *values.uptimeSeconds > 0
        ? ui("%1%").arg(decimal(values.transmitSeconds * 100.0 / *values.uptimeSeconds, 1)) : empty);
    set("readingsUptime", values.uptimeSeconds ? durationReading(*values.uptimeSeconds) : empty);
    set("readingsTemperature", values.temperatureTenthsC ? ui("%1 °C").arg(decimal(*values.temperatureTenthsC / 10.0, 1)) : empty);
    set("readingsTxRx", values.noiseDbm
        ? ui("%1 / %2").arg(durationReading(values.transmitSeconds), durationReading(values.receiveSeconds)) : empty);
    batteryRefresh_->setEnabled(online && !pending);
    batteryRefresh_->setText(pending ? ui("Обновляю…") : ui("Обновить"));
    setRowValue(batteryPrivacy_, client_.radioProtocolVersion() >= 5 ? accessLabel(client_.radioIdentity().telemetryModes & 3) : empty);
}

void RadioSettingsPanel::showAdvanced() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* radio = settingsGroup(layout, page, ui("УПРАВЛЕНИЕ РАДИО"));
    connect(addSettingsRow(radio, "Консоль ноды", "radioConsole", SettingGlyph::Advanced),
            SIGNAL(clicked()), this, SLOT(openConsole()), Qt::QueuedConnection);
    channelsRow_ = addSettingsRow(radio, "Каналы ноды", "radioChannels", SettingGlyph::Channel);
    connect(channelsRow_.data(), SIGNAL(clicked()), this, SLOT(editChannels()), Qt::QueuedConnection);
    refreshRow_ = addSettingsRow(radio, "Обновить список узлов", "refreshRadioNodes", SettingGlyph::Contacts, false);
    connect(refreshRow_.data(), SIGNAL(clicked()), this, SLOT(refreshInventory()), Qt::QueuedConnection);
    connect(addSettingsRow(radio, "Получить сообщения", "radioReceive", SettingGlyph::Messages, false),
            SIGNAL(clicked()), this, SLOT(refreshMessages()), Qt::QueuedConnection);
    connect(addSettingsRow(radio, "Сведения и диагностика", "radioInfo", SettingGlyph::About),
            SIGNAL(clicked()), this, SLOT(showInfo()), Qt::QueuedConnection);
    auto* clock = settingsGroup(layout, page, ui("ЧАСЫ"));
    clockRow_ = addSettingsRow(clock, "Выставить часы радио по телефону", "localRadioClock", SettingGlyph::Readings, false);
    connect(clockRow_.data(), SIGNAL(clicked()), this, SLOT(setRadioClock()), Qt::QueuedConnection);
    clockFeedback_ = addText(layout, QString(), page);
    clockFeedback_->hide();
    auto* app = settingsGroup(layout, page, ui("ИНСТРУМЕНТЫ BELLE"));
    mapsRow_ = addSettingsRow(app, "Карты", "onlineMap", SettingGlyph::Map);
    connect(mapsRow_.data(), SIGNAL(clicked()), this, SIGNAL(mapSettingsRequested()));
    demo_ = addSettingsRow(app, "Демо", "demoMode", SettingGlyph::Advanced, false);
    connect(demo_.data(), SIGNAL(clicked()), this, SLOT(toggleDemo()));
    connect(addSettingsRow(app, "Выйти из OMMesh", "exitFromSettings", SettingGlyph::Power, false),
            SIGNAL(clicked()), this, SIGNAL(exitRequested()));
    layout->addStretch(1);
    updateStatus();
    emit detailRequested(settingsScroll(this, page), ui("Дополнительно"));
}

void RadioSettingsPanel::setRadioClock() {
    if (!client_.connected() || clockRequest_)
        return;
    const quint32 now = QDateTime::currentDateTime().toTime_t();
    meshcore::Bytes command{6};
    for (int shift = 0; shift < 32; shift += 8)
        command.push_back(static_cast<std::uint8_t>(now >> shift));
    const int request = client_.nextConsoleRequestId();
    const bool queued = client_.applyRadioSettings(request, {command}, false);
    if (queued)
        clockRequest_ = request;
    if (clockFeedback_) {
        clockFeedback_->setText(queued ? ui("Выставляем время…") : ui("Дождитесь окончания текущего запроса."));
        clockFeedback_->show();
    }
    updateStatus();
}

void RadioSettingsPanel::clockResult(int request, bool success, const QString& status) {
    if (request != clockRequest_)
        return;
    clockRequest_ = 0;
    if (clockFeedback_) {
        clockFeedback_->setText(success ? ui("Часы радио выставлены.") : status);
        clockFeedback_->show();
    }
    updateStatus();
}

void RadioSettingsPanel::showNotifications() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* quiet = settingsGroup(layout, page, QString());
    connect(preferenceToggle(quiet, "Не беспокоить", "ui/doNotDisturb"),
            SIGNAL(toggled(bool)), this, SLOT(setUserPreference(bool)), Qt::QueuedConnection);
    auto* messages = settingsGroup(layout, page, ui("СООБЩЕНИЯ"));
    connect(preferenceToggle(messages, "Личные сообщения и комнаты", "ui/notifyDirect", true,
                             SettingGlyph::Messages),
            SIGNAL(toggled(bool)), this, SLOT(setUserPreference(bool)), Qt::QueuedConnection);
    connect(preferenceToggle(messages, "Сообщения каналов", "ui/notifyChannels", true,
                             SettingGlyph::Channel),
            SIGNAL(toggled(bool)), this, SLOT(setUserPreference(bool)), Qt::QueuedConnection);
    auto* sound = settingsGroup(layout, page, ui("ЗВУК"));
    notificationSoundRow_ = addSettingsRow(sound, "Звук", "defaultNotificationSound", SettingGlyph::Sound);
    setRowValue(notificationSoundRow_.data(), NodeChatOptions::defaultLabel(NodeChatOptions::Sound));
    connect(notificationSoundRow_.data(), SIGNAL(clicked()), this, SLOT(chooseNotificationSound()), Qt::QueuedConnection);
    auto* haptics = settingsGroup(layout, page, ui("ВИБРАЦИЯ"));
    auto* vibration = preferenceToggle(haptics, "Вибрация при сообщении", "ui/vibration", false,
                                       SettingGlyph::Notifications, ui("Пока приложение открыто."));
    vibration->setObjectName(QLatin1String("vibrateOnMessage"));
    vibration->setChecked(vibrateOnMessage_);
    showToggle(vibration, "Вибрация при сообщении", vibrateOnMessage_);
    connect(vibration, SIGNAL(toggled(bool)), this, SLOT(setVibration(bool)), Qt::QueuedConnection);
    connect(addSettingsRow(haptics, "Проверить вибрацию", "testVibration", SettingGlyph::Notifications, false),
            SIGNAL(clicked()), this, SLOT(testVibration()), Qt::QueuedConnection);
    notificationFeedback_ = addText(layout, QString(), page);
    notificationFeedback_->setObjectName(QLatin1String("vibrationResult"));
    notificationFeedback_->hide();
    layout->addStretch(1);
    emit detailRequested(settingsScroll(this, page), ui("Уведомления"));
}

void RadioSettingsPanel::chooseNotificationSound() {
    if (notificationSoundRow_ && NodeChatOptions::chooseDefault(NodeChatOptions::Sound, this))
        setRowValue(notificationSoundRow_.data(), NodeChatOptions::defaultLabel(NodeChatOptions::Sound));
}

void RadioSettingsPanel::showLog() {
    auto* journal = new QTextEdit(this);
    journal->setReadOnly(true);
    QFile log(QLatin1String("C:/Data/OMMeshBelle-startup.txt"));
    if (log.open(QIODevice::ReadOnly)) {
        constexpr qint64 maximumBytes = 48 * 1024;
        if (log.size() > maximumBytes && log.seek(log.size() - maximumBytes))
            log.readLine();
        journal->setPlainText(QString::fromUtf8(log.read(maximumBytes).constData()));
        journal->moveCursor(QTextCursor::End);
    } else {
        journal->setPlainText(ui("Журнал запуска доступен на Nokia в C:\\Data\\OMMeshBelle-startup.txt."));
    }
    emit detailRequested(journal, ui("Журнал"));
}

void RadioSettingsPanel::showAbout() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    addText(layout, ui("OMMesh Belle %1").arg(QApplication::applicationVersion()), page);
    addText(layout, ui("Клиент MeshCore для Nokia 700 и Symbian Belle."), page);
    layout->addStretch(1);
    emit detailRequested(page, ui("О приложении"));
}

void RadioSettingsPanel::showMessagePreferences() {
    auto* page = new QWidget(this);
    page->setObjectName(QLatin1String("messagePreferencesPanel"));
    auto* content = new QWidget(page);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 4, 4);
    layout->setSpacing(12);
    const auto option = [this](QBoxLayout* rows, const char* title, const char* key,
                                  const char* hint, bool defaultValue = false) {
        QPushButton* row = preferenceToggle(rows, title, key, defaultValue, SettingGlyph::Messages, ui(hint));
        connect(row, SIGNAL(toggled(bool)), this, SLOT(setUserPreference(bool)));
        return row;
    };

    auto* opening = settingsGroup(layout, content, QString());
    option(opening, "Открывать с первого непрочитанного", "ui/openAtUnread",
           "В чате с новыми сообщениями вы сразу увидите первое непрочитанное.", true);
    auto* blocked = addSettingsRow(opening, "Заблокированные", "blockedSenders", SettingGlyph::Name);
    const int blockedCount = client_.blockedContacts().size() + client_.blockedChannelNames().size();
    if (blockedCount)
        setRowValue(blocked, QString::number(blockedCount));
    connect(blocked, SIGNAL(clicked()), this, SLOT(showBlockedSenders()), Qt::QueuedConnection);

    auto* keyboard = settingsGroup(layout, content, ui("Системная клавиатура"));
    option(keyboard, "Полноэкранный ввод", NativeKeyboard::FullScreenPreference,
           "Выбирает клавиатуру во всём приложении. Выключено — клавиатура снизу.");

    auto* compactRows = settingsGroup(layout, content, QString());
    auto* compact = option(compactRows, "Заменять похожие буквы", "ui/compactCyrillic",
                           "а е о р с х А В Е К М Н О Р С Т Х заменяются латинскими двойниками "
                           "и занимают один байт вместо двух.", true);
    auto* near = option(compactRows, "Почти такие же", "ui/compactNear",
                        "Ещё у → y, ё → e, З → 3, Ь → b. Над ё пропадут точки.");
    auto* cursive = option(compactRows, "Как в рукописном", "ui/compactCursive",
                           "Ещё т → m, п → n, и → u. В обычном шрифте разница заметна.");
    near->setVisible(compact->isChecked());
    cursive->setVisible(compact->isChecked());
    connect(compact, SIGNAL(toggled(bool)), this, SLOT(setLookalikeDetailsVisible(bool)));

    auto* reading = settingsGroup(layout, content, QString());
    option(reading, "Читать русскими буквами", "ui/restoreCyrillic",
           "Латинские двойники среди русских букв показываются русскими. "
           "Bcmpeчa y мocma → Встреча у моста.", true);

    auto* previews = settingsGroup(layout, content, ui("Превью ссылок"));
    auto* modes = new QButtonGroup(content);
    const int selected = QSettings().value(QLatin1String("ui/linkPreviewMode"), 0).toInt();
    const struct { const char* title; const char* hint; } choices[] = {
        {"Выключено", "Ссылки остаются ссылками."},
        {"По нажатию", "Нажмите на значок рядом со ссылкой, чтобы показать или свернуть превью."},
        {"Сразу", "Превью появляется само, когда сообщение на экране."},
    };
    for (int mode = 0; mode < int(sizeof(choices) / sizeof(choices[0])); ++mode) {
        auto* choice = addSettingsRow(previews, choices[mode].title, nullptr, SettingGlyph::Connection,
                                      false, ui(choices[mode].hint), true);
        choice->setObjectName(QLatin1String("linkPreviewMode") + QString::number(mode));
        choice->setCheckable(true);
        choice->setChecked((selected >= 0 && selected <= 2 ? selected : 0) == mode);
        modes->addButton(choice, mode);
    }
    connect(modes, SIGNAL(buttonClicked(int)), this, SLOT(setLinkPreviewMode(int)));
    addText(layout, ui("Превью запрашивается у preview.ommesh.ru. Сервер получает адрес ссылки "
                       "и сам открывает сайт. По радиосети превью не передаётся."), content)
        ->setObjectName(QLatin1String("muted"));

    auto* attempts = settingsGroup(layout, content, QString());
    auto* controls = new QWidget(attempts->parentWidget());
    auto* controlsLayout = new QVBoxLayout(controls);
    controlsLayout->setContentsMargins(6, 6, 6, 7);
    controlsLayout->setSpacing(3);
    auto* stepper = new QHBoxLayout;
    stepper->setSpacing(4);
    auto* heading = new QLabel(ui("Попытки отправки"), controls);
    heading->setWordWrap(true);
    stepper->addWidget(heading, 1);
    auto* fewer = new QPushButton(ui("−"), controls);
    auto* more = new QPushButton(ui("+"), controls);
    fewer->setObjectName(QLatin1String("sendTriesFewer"));
    more->setObjectName(QLatin1String("sendTriesMore"));
    fewer->setProperty("accessibleName", ui("Уменьшить число попыток"));
    more->setProperty("accessibleName", ui("Увеличить число попыток"));
    for (auto* button : {fewer, more}) {
        button->setFixedSize(40, 40);
        connect(button, SIGNAL(clicked()), this, SLOT(changeSendTries()));
    }
    fewer->setProperty("sendTriesDelta", -1);
    more->setProperty("sendTriesDelta", 1);
    auto* value = new QLabel(controls);
    value->setObjectName(QLatin1String("sendTriesValue"));
    value->setAlignment(Qt::AlignCenter);
    value->setMinimumWidth(62);
    stepper->addWidget(fewer);
    stepper->addWidget(value);
    stepper->addWidget(more);
    controlsLayout->addLayout(stepper);
    addText(controlsLayout, QString(), controls)->setObjectName(QLatin1String("muted"));
    attempts->addWidget(controls);
    updateSendTriesControls(controls);
    addText(layout, ui("При нескольких попытках неподтверждённое личное сообщение отправляется "
                       "повторно. Первая попытка идёт по известному маршруту, следующие — по сети. "
                       "В комнатах повторную отправку запускаете вы."), content)
        ->setObjectName(QLatin1String("muted"));
    layout->addStretch(1);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(settingsScroll(page, content), 1);
    emit detailRequested(page, ui("Сообщения и маршруты"));
}

void RadioSettingsPanel::setLookalikeDetailsVisible(bool visible) {
    auto* toggle = qobject_cast<QPushButton*>(sender());
    if (!toggle)
        return;
    QWidget* rows = toggle->parentWidget();
    for (const char* key : {"ui/compactNear", "ui/compactCursive"})
        rows->findChild<QPushButton*>(QLatin1String(key))->setVisible(visible);
}

void RadioSettingsPanel::setLinkPreviewMode(int mode) {
    QSettings preferences;
    preferences.setValue(QLatin1String("ui/linkPreviewMode"), qBound(0, mode, 2));
    preferences.sync();
}

void RadioSettingsPanel::changeSendTries() {
    auto* button = qobject_cast<QPushButton*>(sender());
    if (!button)
        return;
    client_.setSendTries(client_.sendTries() + button->property("sendTriesDelta").toInt());
    updateSendTriesControls(button->parentWidget());
}

void RadioSettingsPanel::updateSendTriesControls(QWidget* controls) {
    const int tries = client_.sendTries();
    controls->findChild<QPushButton*>(QLatin1String("sendTriesFewer"))
        ->setEnabled(tries > Client::MinimumSendTries);
    controls->findChild<QPushButton*>(QLatin1String("sendTriesMore"))
        ->setEnabled(tries < Client::MaximumSendTries);
    controls->findChild<QLabel*>(QLatin1String("sendTriesValue"))
        ->setText(tries == 1 ? ui("Один раз") : QString::number(tries));
    QString hint = ui("При отсутствии подтверждения можно повторить отправку вручную.");
    if (tries > 1) {
        const int minutes = qMax(1, qRound(Client::sendTriesSpanMs(tries) / 60000.0));
        const QString count = ui(tries < 5 ? "%1 попытки" : "%1 попыток").arg(tries);
        hint = ui("%1 примерно за %2 мин. Время ожидания подтверждений добавляется.").arg(count).arg(minutes);
    }
    controls->findChild<QLabel*>(QLatin1String("muted"))->setText(hint);
}

void RadioSettingsPanel::showBlockedSenders() {
    QPointer<QPushButton> opener = qobject_cast<QPushButton*>(sender());
    QDialog dialog(this);
    dialog.setObjectName(QLatin1String("blockedSendersDialog"));
    dialog.setWindowTitle(ui("Заблокированные"));
    auto* outer = new QVBoxLayout(&dialog);
    addText(outer, dialog.windowTitle(), &dialog)->setObjectName(QLatin1String("subtitle"));
    auto* content = new QWidget(&dialog);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    const QStringList names = client_.blockedChannelNames();
    const QMap<QString, QString> contacts = client_.blockedContacts();
    const auto addEntry = [this](QVBoxLayout* group, const QString& id, const QString& name,
                                bool room, int hidden) {
        auto* entry = new QWidget(group->parentWidget());
        auto* box = new QHBoxLayout(entry);
        box->setContentsMargins(6, 4, 6, 4);
        auto* words = new QVBoxLayout;
        words->setSpacing(2);
        QLabel* senderName = addText(words, name, entry);
        senderName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        senderName->setToolTip(name);
        addText(words, ui(room ? "По ключу · скрыто %1" : "По имени · скрыто %1").arg(hidden), entry)
            ->setObjectName(QLatin1String("muted"));
        box->addLayout(words, 1);
        auto* unblock = new QPushButton(ui("Разблокировать"), entry);
        unblock->setObjectName(QLatin1String("unblockSender"));
        unblock->setProperty("blockedSenderKey", id);
        unblock->setProperty("blockedSenderRoom", room);
        unblock->setMinimumHeight(40);
        connect(unblock, SIGNAL(clicked()), this, SLOT(unblockSender()));
        box->addWidget(unblock);
        group->addWidget(entry);
    };
    if (!names.isEmpty()) {
        auto* channels = settingsGroup(layout, content, ui("В каналах"));
        for (const QString& name : names)
            addEntry(channels, name, name, false, client_.blockedChannelMessageCount(name));
    }
    if (!contacts.isEmpty()) {
        auto* rooms = settingsGroup(layout, content, ui("В комнатах"));
        for (auto it = contacts.constBegin(); it != contacts.constEnd(); ++it)
            addEntry(rooms, it.key(), it.value(), true, client_.blockedContactMessageCount(it.key()));
    }
    auto* empty = addText(layout, ui("Никто не заблокирован. Удерживайте сообщение в канале "
                                     "или комнате, чтобы заблокировать отправителя."), content);
    empty->setObjectName(QLatin1String("blockedSendersEmpty"));
    empty->setVisible(names.isEmpty() && contacts.isEmpty());
    layout->addStretch(1);
    outer->addWidget(settingsScroll(&dialog, content), 1);
    connectCancel(addSettingsButton(outer, ui("Назад"), &dialog, "closeBlockedSenders"), &dialog);
    const QSize available = window()->size();
    dialog.resize(qMax(240, available.width() - 22), qMax(270, available.height() - 100));
    dialog.exec();
    if (opener) {
        const int count = client_.blockedContacts().size() + client_.blockedChannelNames().size();
        setRowValue(opener, count ? QString::number(count) : QString());
    }
}

void RadioSettingsPanel::unblockSender() {
    auto* button = qobject_cast<QPushButton*>(sender());
    if (!button)
        return;
    const QString key = button->property("blockedSenderKey").toString();
    if (button->property("blockedSenderRoom").toBool())
        client_.unblockContact(key);
    else
        client_.unblockChannelName(key);
    QWidget* entry = button->parentWidget();
    entry->hide();
    QWidget* group = entry->parentWidget();
    bool groupEmpty = true;
    for (QPushButton* action : group->findChildren<QPushButton*>(QLatin1String("unblockSender")))
        groupEmpty = groupEmpty && !action->isVisible();
    if (groupEmpty)
        group->parentWidget()->hide();
    if (client_.blockedContacts().isEmpty() && client_.blockedChannelNames().isEmpty()) {
        QWidget* dialog = button->window();
        dialog->findChild<QLabel*>(QLatin1String("blockedSendersEmpty"))->show();
    }
}

void RadioSettingsPanel::showAppearance() {
    auto* page = new AppearancePanel(this);
    connect(page, SIGNAL(appearanceChanged(int,int)), this, SIGNAL(appearanceChanged(int,int)));
    connect(page, SIGNAL(appearanceChanged(int,int)), this, SLOT(updateAppearanceSummary(int,int)));
    emit detailRequested(page, ui("Оформление"));
}

void RadioSettingsPanel::updateAppearanceSummary(int theme, int chatTextSize) {
    setRowValue(appearanceRow_, ui("%1 · %2").arg(Appearance::themeName(Appearance::checkedTheme(theme)))
                .arg(Appearance::checkedChatTextSize(chatTextSize)));
}

void RadioSettingsPanel::setUserPreference(bool enabled) {
    auto* button = qobject_cast<QPushButton*>(sender());
    if (!button)
        return;
    const QByteArray key = button->objectName().toLatin1();
    button->setProperty("accessibleDescription", enabled ? ui("Включено") : ui("Выключено"));
    if (key == "ui/wlanRecovery") {
        client_.setWlanRecovery(enabled);
        saveToggle(key.constData(), enabled, connectionFeedback_);
    } else {
        saveToggle(key.constData(), enabled,
                   key == "ui/doNotDisturb" || key == "ui/notifyDirect" || key == "ui/notifyChannels"
                       ? notificationFeedback_.data() : nullptr);
    }
}

void RadioSettingsPanel::updateStatus() {
    const bool online = client_.connected();
    const QString empty = ui("—");
    const QString name = client_.radioName();
    setRowValue(nameRow_, name.isEmpty() ? empty : name);
    const QString parameters = client_.radioParametersSummary();
    setRowValue(frequencyRow_, parameters.isEmpty() ? empty : parameters);
    const int millivolts = online ? client_.batteryMillivolts() : 0;
    const QString voltage = millivolts > 0
        ? ui("%1 В").arg(QString::number(millivolts / 1000.0, 'f', 2).replace(QLatin1Char('.'), QLatin1Char(',')))
        : empty;
    setRowValue(batteryRow_, voltage);
    const QVector<NetworkNode> nodes = client_.nodes();
    setRowValue(contactsRow_, QString::number(nodes.size()));
    const bool positioned = online && hasPosition(client_.selfNode());
    const QString location = client_.radioProtocolVersion() >= 5
        ? client_.radioIdentity().advertLocPolicy ? ui("Место видно всем") : ui("Место скрыто")
        : positioned ? ui("Есть координаты") : ui("Не указано");
    setRowValue(locationRow_, online ? location : empty);
    if (channelsRow_) {
        QString slotsUsed = empty;
        if (online) {
            const QVector<RadioChannel> channels = client_.radioChannels();
            int used = 0;
            for (const RadioChannel& channel : channels)
                used += !channel.title.isEmpty() || !channel.key.isEmpty();
            slotsUsed = ui("%1 из %2").arg(used).arg(channels.size());
        }
        setRowValue(channelsRow_, slotsUsed);
    }
    setRowValue(connectionRow_, ui("Wi‑Fi / TCP"));
    setRowValue(attachmentsRow_, Attachments::statusText());
    if (clockRow_)
        clockRow_->setEnabled(online && !clockRequest_ && !client_.busy());
    const QSettings preferences;
    const bool quiet = preferences.value(QLatin1String("ui/doNotDisturb"), false).toBool();
    setRowValue(notificationsRow_, quiet ? ui("Не беспокоить") :
                vibrateOnMessage_ ? ui("Вибрация вкл.") : ui("Вибрация выкл."));
    const bool compact = preferences.value(QLatin1String("ui/compactCyrillic"), true).toBool();
    const bool restore = preferences.value(QLatin1String("ui/restoreCyrillic"), true).toBool();
    setRowValue(messagesRow_, compact ? ui("Похожие буквы") : restore ? ui("Кириллица") : QString());
    const Appearance::Preferences appearance = Appearance::preferences(preferences);
    updateAppearanceSummary(appearance.theme, appearance.chatTextSize);
    if (mapsRow_) {
        const NodeMap::Source mapMode = NodeMap::sourceFromPreference(
            preferences.value(QLatin1String("map/source"), 0).toInt());
        setRowValue(mapsRow_, mapMode == NodeMap::Loopback ? ui("На телефоне") : ui("Офлайн"));
    }
    if (refreshRow_)
        refreshRow_->setEnabled(online && !client_.busy());
    if (demo_)
        demo_->setText(demoRadioActive(client_) ? ui("Выйти из демо") : ui("Демо"));
    updateBatteryPage();
    updatePrivacyControls();
    refreshTrustedContacts();
    if (liveReadout_)
        liveReadout_->setText(client_.radioInfo());
}

void RadioSettingsPanel::advertise() {
    if (!client_.connected()) {
        QMessageBox::information(this, ui("Анонс"), ui("Подключитесь к рации MeshCore."));
        return;
    }
    if (QMessageBox::question(this, ui("Анонс в эфир"), ui("Передать анонс в сеть MeshCore?"),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes &&
        !client_.advertise()) {
        QMessageBox::information(this, ui("Анонс"), ui("Дождитесь окончания текущего запроса."));
    }
}

void RadioSettingsPanel::openConsole() { showConsole(client_, QString(), ui("Консоль рации"), this); }
void RadioSettingsPanel::toggleDemo() {
    if (demoRadioActive(client_))
        stopDemoRadio(client_);
    else if (!startDemoRadio(client_))
        QMessageBox::information(this, ui("Демо"), ui("Демо недоступно. Отключите активное подключение и повторите."));
    updateStatus();
}
void RadioSettingsPanel::editChannels() {
    openChannels(false);
}

void RadioSettingsPanel::addChannel() {
    openChannels(true);
}

void RadioSettingsPanel::openChannels(bool adding) {
    if (!client_.connected()) {
        QMessageBox::information(this, ui("Каналы рации"), ui("Подключитесь к рации MeshCore."));
        return;
    }
    RadioChannelsDialog dialog(client_, this, adding);
    dialog.exec();
}

void configureNodeMap(NodeMap* map) {
    if (!map)
        return;
    QSettings preferences;
    const int mode = preferences.value(QLatin1String("map/source"), 0).toInt();
    const int region = preferences.value(QLatin1String("map/region"), NodeMap::OmskCity).toInt();
    map->setOfflineRegion(static_cast<NodeMap::OfflineRegion>(
        qBound(int(NodeMap::OmskCity), region, int(NodeMap::World))));
    const NodeMap::Source source = NodeMap::sourceFromPreference(mode);
    if (mode == 1 || mode == 2)
        preferences.setValue(QLatin1String("map/source"), static_cast<int>(source));
    map->setSource(source);
}

RadioConsoleDialog::RadioConsoleDialog(Client& client, const QString& nodeId, const QString& title, QWidget* parent,
                                       bool loginOnly)
    : QDialog(parent), client_(client), nodeId_(nodeId), loginOnly_(loginOnly) {
    setWindowTitle(title);
    resize(338, loginOnly_ ? 170 : 425);
    auto* layout = new QVBoxLayout(this);
    state_ = addText(layout, loginOnly_ ? ui("Введите пароль узла.") : nodeId_.isEmpty()
        ? ui("Команды подключённой рации") : ui("Войдите как администратор узла."), this);
    auto* signIn = new QHBoxLayout;
    password_ = new QLineEdit(this);
    password_->setObjectName(QLatin1String("nodePassword"));
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(ui("Пароль узла"));
    password_->setMaxLength(15);
    signIn->addWidget(password_, 1);
    login_ = addSettingsButton(signIn, ui("Войти"), this, "consoleLogin");
    layout->addLayout(signIn);
    if (!loginOnly_) {
        output_ = new QTextEdit(this);
        output_->setObjectName(QLatin1String("consoleOutput"));
        output_->setReadOnly(true);
        layout->addWidget(output_, 1);
        command_ = new QLineEdit(this);
        command_->setObjectName(QLatin1String("consoleCommand"));
        command_->setPlaceholderText(ui("Команда"));
        command_->setMaxLength(172);
        command_->setMinimumHeight(40);
        command_->setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
        layout->addWidget(command_);
    }
    auto* commands = new QHBoxLayout;
    connectCancel(addSettingsButton(commands, ui("Назад"), this), this);
    if (!loginOnly_) {
        send_ = addSettingsButton(commands, ui("Отправить"), this, "consoleSend");
        connect(send_, SIGNAL(clicked()), this, SLOT(submit()));
        connect(command_, SIGNAL(returnPressed()), this, SLOT(submit()));
    }
    layout->addLayout(commands);
    connect(login_, SIGNAL(clicked()), this, SLOT(login()));
    connect(password_, SIGNAL(returnPressed()), this, SLOT(login()));
    connect(&client_, SIGNAL(changed()), this, SLOT(readyChanged()));
    connect(&client_, SIGNAL(consoleResult(int,int,bool,QString)), this, SLOT(result(int,int,bool,QString)));
    connected_ = client_.connected();
    authorized_ = client_.nodeConsoleAuthorized(nodeId_);
    if (authorized_ && !loginOnly_)
        state_->setText(ui("Вход выполнен."));
    updateControls();
}

void RadioConsoleDialog::readyChanged() {
    const bool connected = client_.connected();
    if (connected_ && !connected) {
        authorized_ = false;
        password_->clear();
        state_->setText(ui("Связь с рацией прервана."));
    } else if (!connected_ && connected) {
        state_->setText(nodeId_.isEmpty() ? ui("Связь восстановлена.") : ui("Связь восстановлена. Войдите на узел."));
    }
    connected_ = connected;
    updateControls();
}

void RadioConsoleDialog::updateControls() {
    const bool requiresLogin = !nodeId_.isEmpty() && (loginOnly_ || !authorized_);
    password_->setVisible(requiresLogin);
    login_->setVisible(requiresLogin);
    password_->setEnabled(connected_ && request_ == 0);
    login_->setEnabled(connected_ && request_ == 0);
    if (command_) {
        command_->setEnabled(connected_ && request_ == 0 && !requiresLogin);
        send_->setEnabled(command_->isEnabled());
    }
}

void RadioConsoleDialog::login() {
    if (!login_->isEnabled() || nodeId_.isEmpty() || (authorized_ && !loginOnly_))
        return;
    const QString password = password_->text();
    password_->clear();
    const int id = client_.nextConsoleRequestId();
    request_ = id;
    state_->setText(ui("Выполняется вход на узел…"));
    if (!client_.loginConsole(id, nodeId_, password)) {
        request_ = 0;
        state_->setText(ui("Вход недоступен. Проверьте связь и длину пароля."));
        return;
    }
    updateControls();
}

void RadioConsoleDialog::submit() {
    if (!send_ || !send_->isEnabled())
        return;
    const QString command = command_->text().trimmed();
    if (command.isEmpty()) {
        state_->setText(ui("Введите команду."));
        return;
    }
    const int id = client_.nextConsoleRequestId();
    request_ = id;
    if (!client_.runConsole(id, nodeId_, command)) {
        request_ = 0;
        state_->setText(ui("Запрос недоступен. Проверьте связь и длину команды."));
        return;
    }
    command_->clear();
    state_->setText(ui("Ожидание ответа…"));
    updateControls();
}

void RadioConsoleDialog::result(int requestId, int operation, bool success, const QString& text) {
    if (request_ != requestId)
        return;
    request_ = 0;
    if (operation == meshcore::RadioConsole::Login) {
        authorized_ = client_.nodeConsoleAuthorized(nodeId_);
        if (success && loginOnly_) {
            accept();
            return;
        }
        state_->setText(success && !authorized_ ? ui("Вход выполнен без прав администратора.") : text);
    } else {
        state_->setText(success ? ui("Ответ получен.") : ui("Команда не подтверждена."));
    }
    append(text);
    updateControls();
}

void RadioConsoleDialog::append(const QString& text) {
    if (!output_ || text.isEmpty())
        return;
    if (!transcript_.isEmpty())
        transcript_.append(QLatin1Char('\n'));
    transcript_.append(text);
    transcript_ = transcript_.right(4000);
    output_->setPlainText(transcript_);
    output_->moveCursor(QTextCursor::End);
}

RadioChannelsDialog::RadioChannelsDialog(Client& client, QWidget* parent, bool adding, const QString& targetId)
    : QDialog(parent), client_(client), adding_(adding && targetId.isEmpty()), targetId_(targetId),
      targetSlot_(targetId.isEmpty() ? -1 : client.radioChannelSlot(targetId)),
      targetSession_(client.radioSessionToken()) {
    if (targetId_.isEmpty())
        setWindowTitle(adding_ ? ui("Добавить канал") : ui("Каналы рации"));
    else
        setWindowTitle(ui("Изменить канал"));
    resize(336, 330);
    auto* layout = new QVBoxLayout(this);
    slots_ = new QComboBox(this);
    slots_->setObjectName(QLatin1String("radioChannelSlot"));
    slots_->setMinimumHeight(40);
    layout->addWidget(slots_);
    name_ = new QLineEdit(this);
    name_->setObjectName(QLatin1String("radioChannelName"));
    name_->setPlaceholderText(ui("Имя канала"));
    name_->setMaxLength(31);
    name_->setMinimumHeight(40);
    layout->addWidget(name_);
    key_ = new QLineEdit(this);
    key_->setObjectName(QLatin1String("radioChannelKey"));
    key_->setPlaceholderText(ui("Ключ · 32 шестнадцатеричных знака"));
    key_->setMaxLength(32);
    key_->setMinimumHeight(40);
    key_->setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
    layout->addWidget(key_);
    state_ = addText(layout, ui("Для открытого канала введите #имя. Для закрытого введите имя и ключ."), this);
    layout->addStretch(1);
    save_ = addSettingsButton(layout, ui("Сохранить канал"), this, "radioChannelSave");
    clear_ = addSettingsButton(layout, ui("Очистить слот"), this, "radioChannelClear");
    clear_->setVisible(!adding_);
    connectCancel(addSettingsButton(layout, ui("Назад"), this), this);
    connect(slots_, SIGNAL(currentIndexChanged(int)), this, SLOT(selectSlot(int)));
    connect(save_, SIGNAL(clicked()), this, SLOT(saveChannel()));
    connect(clear_, SIGNAL(clicked()), this, SLOT(clearChannel()));
    connect(&client_, SIGNAL(channelResult(int,bool,QString)), this, SLOT(receiveResult(int,bool,QString)));
    populate(targetSlot_);
}

bool RadioChannelsDialog::targetIsCurrent() const {
    return targetSlot_ >= 0 && client_.radioSessionToken() == targetSession_ &&
        client_.radioChannelSlot(targetId_) == targetSlot_;
}

void RadioChannelsDialog::populate(int preferredSlot) {
    const auto channels = client_.radioChannels();
    const bool targeted = !targetId_.isEmpty();
    const bool targetAvailable = !targeted || targetIsCurrent();
    slots_->blockSignals(true);
    slots_->clear();
    int current = 0;
    for (const auto& channel : channels) {
        if (targeted && (!targetAvailable || channel.slot != targetSlot_))
            continue;
        if (adding_ && (!channel.title.isEmpty() || !channel.key.isEmpty()))
            continue;
        if (channel.slot == preferredSlot)
            current = slots_->count();
        slots_->addItem(ui("Слот %1 · %2").arg(channel.slot).arg(channel.title.isEmpty() ? ui("Пусто") : channel.title),
                        channel.slot);
    }
    slots_->setCurrentIndex(current);
    slots_->blockSignals(false);
    selectSlot(slots_->currentIndex());
    if (targeted && !targetAvailable)
        state_->setText(ui("Канал изменился или рация отключена. Откройте настройки канала заново."));
    else if (adding_ && slots_->count() == 0)
        state_->setText(ui("В рации нет доступных свободных слотов."));
}

void RadioChannelsDialog::selectSlot(int index) {
    const int slot = index >= 0 ? slots_->itemData(index).toInt() : -1;
    const auto channels = client_.radioChannels();
    bool configured = false;
    name_->clear();
    key_->clear();
    for (const auto& channel : channels) {
        if (channel.slot == slot) {
            name_->setText(channel.title);
            key_->setText(channel.key);
            configured = !channel.title.isEmpty() || !channel.key.isEmpty();
            break;
        }
    }
    slots_->setEnabled(targetId_.isEmpty() && pendingSlot_ < 0);
    save_->setEnabled(slot >= 0 && pendingSlot_ < 0);
    clear_->setEnabled(configured && pendingSlot_ < 0);
}

void RadioChannelsDialog::saveChannel() {
    QString name = name_->text().trimmed();
    QString hex = key_->text().trimmed().toLower();
    if (hex.isEmpty() && name.startsWith(QLatin1Char('#'))) {
        int first = 0;
        while (first < name.size() && name.at(first) == QLatin1Char('#'))
            ++first;
        QString hashtag(QLatin1Char('#'));
        for (int index = first; index < name.size(); ++index) {
            if (!name.at(index).isSpace())
                hashtag.append(name.at(index));
        }
        name = hashtag.toLower();
        if (name.size() > 1) {
            const auto utf8 = name.toUtf8();
            const auto key = meshcore::hashtagChannelKey({utf8.constData(), static_cast<std::size_t>(utf8.size())});
            hex = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(key.data()), key.size()).toHex().constData());
        }
    }
    bool valid = hex.size() == 32;
    for (int i = 0; valid && i < hex.size(); ++i) {
        const ushort c = hex.at(i).unicode();
        valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }
    if (name.isEmpty() || name.toUtf8().size() > 31 || !valid) {
        state_->setText(ui("Имя должно занимать 1–31 байт UTF-8. В ключе нужны ровно 32 шестнадцатеричных знака."));
        return;
    }
    submit(name, hex);
}

void RadioChannelsDialog::clearChannel() {
    if (QMessageBox::question(this, ui("Очистить слот"), ui("Удалить канал из выбранного слота рации?"),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        submit(QString(), QString());
}

void RadioChannelsDialog::submit(const QString& name, const QString& hex) {
    if (slots_->currentIndex() < 0 || pendingSlot_ >= 0)
        return;
    const int slot = slots_->itemData(slots_->currentIndex()).toInt();
    if (!targetId_.isEmpty() && !targetIsCurrent()) {
        populate(targetSlot_);
        return;
    }
    if (adding_) {
        const auto channels = client_.radioChannels();
        const bool stillFree = std::any_of(channels.begin(), channels.end(), [slot](const RadioChannel& channel) {
            return channel.slot == slot && channel.title.isEmpty() && channel.key.isEmpty();
        });
        if (!stillFree) {
            state_->setText(ui("Слот уже занят. Выберите другой."));
            populate(-1);
            return;
        }
    }
    pendingSlot_ = slot;
    if (!client_.setRadioChannel(slot, name, hex)) {
        pendingSlot_ = -1;
        state_->setText(ui("Не удалось отправить запрос. Проверьте связь с рацией."));
        return;
    }
    slots_->setEnabled(false);
    save_->setEnabled(false);
    clear_->setEnabled(false);
    state_->setText(ui("Ожидание ответа рации…"));
}

void RadioChannelsDialog::receiveResult(int slot, bool success, const QString& text) {
    if (pendingSlot_ != slot)
        return;
    pendingSlot_ = -1;
    if (success && (adding_ || !targetId_.isEmpty())) {
        accept();
        return;
    }
    state_->setText(text.isEmpty() ? (success ? ui("Канал обновлён.") : ui("Рация отклонила изменение.")) : text);
    populate(slot);
}
