#include "settings_radio_editor.hpp"

#include "client.hpp"

#include <QtCore/QVariant>
#include <QtGui/QComboBox>
#include <QtGui/QLabel>
#include <QtGui/QLineEdit>
#include <QtGui/QPushButton>
#include <QtGui/QVBoxLayout>

#include <cmath>

namespace {

QString ui(const char* text) { return QString::fromUtf8(text); }

struct Preset {
    const char* name;
    int frequencyKhz;
    int bandwidthHz;
    int spreading;
    int coding;
};

constexpr Preset presets[] = {
    {"EU/UK", 869525, 250000, 11, 5},
    {"EU узкий", 869618, 62500, 8, 8},
    {"US", 910525, 62500, 7, 5},
    {"ANZ", 915800, 250000, 10, 5},
    {"OMS", 868731, 62500, 7, 7}
};

constexpr int bandwidths[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};

void field(QVBoxLayout* layout, const QString& caption, QWidget* input, const char* id) {
    auto* label = new QLabel(caption, layout->parentWidget());
    label->setObjectName(QLatin1String("muted"));
    label->setWordWrap(true);
    input->setObjectName(QLatin1String(id));
    layout->addWidget(label);
    layout->addWidget(input);
}

void setChoice(QComboBox* combo, int value, const QString& caption) {
    int position = combo->findData(value);
    if (position < 0) {
        combo->addItem(caption, value);
        position = combo->count() - 1;
    }
    combo->setCurrentIndex(position);
}

int selected(QComboBox* combo) { return combo->itemData(combo->currentIndex()).toInt(); }

void append32(meshcore::Bytes& frame, quint32 value) {
    for (int shift = 0; shift < 32; shift += 8)
        frame.push_back(static_cast<std::uint8_t>(value >> shift));
}

QString bandwidthText(int bandwidth) {
    return QString::number(bandwidth / 1000.0, 'g', 6);
}

} // namespace

RadioEditorPanel::RadioEditorPanel(Client& client, Page page, QWidget* parent)
    : QWidget(parent), client_(client), page_(page) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 6);
    layout->setSpacing(7);
    inputs_ = new QWidget(this);
    auto* form = new QVBoxLayout(inputs_);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(5);
    layout->addWidget(inputs_);
    if (page_ == Name) {
        name_ = new QLineEdit(inputs_);
        name_->setMaxLength(31);
        field(form, ui("Имя в эфире"), name_, "localRadioName");
        auto* note = new QLabel(ui("До 31 байта UTF-8. Имя появится у других после следующего анонса."), inputs_);
        note->setObjectName(QLatin1String("muted"));
        note->setWordWrap(true);
        form->addWidget(note);
    } else if (page_ == Frequency) {
        auto* presetBox = new QComboBox(inputs_);
        presetBox->addItem(ui("Выбрать пресет"));
        for (const Preset& preset : presets) {
            presetBox->addItem(ui("%1 · %2 МГц").arg(ui(preset.name),
                              QString::number(preset.frequencyKhz / 1000.0, 'f', 3)));
        }
        field(form, ui("Пресет"), presetBox, "localRadioPreset");
        connect(presetBox, SIGNAL(activated(int)), this, SLOT(choosePreset(int)));

        frequency_ = new QLineEdit(inputs_);
        field(form, ui("Частота, МГц"), frequency_, "localRadioFrequency");
        bandwidth_ = new QComboBox(inputs_);
        for (int bandwidth : bandwidths)
            bandwidth_->addItem(bandwidthText(bandwidth), bandwidth);
        field(form, ui("Полоса, кГц"), bandwidth_, "localRadioBandwidth");
        spreading_ = new QComboBox(inputs_);
        for (int spreading = 5; spreading <= 12; ++spreading)
            spreading_->addItem(ui("SF%1").arg(spreading), spreading);
        field(form, ui("SF"), spreading_, "localRadioSpreadingFactor");
        coding_ = new QComboBox(inputs_);
        for (int coding = 5; coding <= 8; ++coding)
            coding_->addItem(ui("4/%1").arg(coding), coding);
        field(form, ui("CR"), coding_, "localRadioCodingRate");
        power_ = new QLineEdit(inputs_);
        field(form, ui("Мощность передачи, дБм"), power_, "localRadioPower");
        repeat_ = new QComboBox(inputs_);
        repeat_->addItem(ui("Выкл."), 0);
        repeat_->addItem(ui("Вкл."), 1);
        field(form, ui("Ретрансляция"), repeat_, "localRadioRepeat");
        auto* repeatHint = new QLabel(ui("Для ретрансляции нужна частота, разрешённая прошивкой ноды."), inputs_);
        repeatHint->setObjectName(QLatin1String("muted"));
        repeatHint->setWordWrap(true);
        form->addWidget(repeatHint);
        pathHash_ = new QComboBox(inputs_);
        pathHash_->addItem(ui("1 байт"), 0);
        pathHash_->addItem(ui("2 байта"), 1);
        pathHash_->addItem(ui("3 байта"), 2);
        field(form, ui("Хеш маршрута"), pathHash_, "localRadioPathHash");
    } else {
        contactCount_ = new QLabel(inputs_);
        field(form, ui("Контакты в памяти ноды"), contactCount_, "localRadioContactCount");
        addContacts_ = new QComboBox(inputs_);
        addContacts_->addItem(ui("Все узлы"), 0);
        addContacts_->addItem(ui("По правилам ноды"), 1);
        field(form, ui("Сохранять новые узлы"), addContacts_, "localRadioSaveContacts");
        refresh_ = new QPushButton(ui("Загрузить все контакты из радио"), inputs_);
        refresh_->setObjectName(QLatin1String("localRadioReloadContacts"));
        form->addWidget(refresh_);
        connect(refresh_, SIGNAL(clicked()), this, SLOT(refreshContacts()), Qt::QueuedConnection);
    }
    save_ = new QPushButton(ui("Сохранить"), this);
    save_->setObjectName(QLatin1String("saveLocalRadioSettings"));
    layout->addWidget(save_);
    feedback_ = new QLabel(this);
    feedback_->setWordWrap(true);
    feedback_->setObjectName(QLatin1String("muted"));
    layout->addWidget(feedback_);
    layout->addStretch(1);
    connect(save_, SIGNAL(clicked()), this, SLOT(save()), Qt::QueuedConnection);
    connect(&client_, SIGNAL(changed()), this, SLOT(updateStatus()), Qt::QueuedConnection);
    connect(&client_, SIGNAL(radioSettingsResult(int,bool,QString)),
            this, SLOT(receiveResult(int,bool,QString)), Qt::QueuedConnection);
    updateStatus();
}

void RadioEditorPanel::loadValues() {
    const auto& radio = client_.radioIdentity();
    if (page_ == Name) {
        name_->setText(client_.radioName());
    } else if (page_ == Frequency) {
        frequency_->setText(QString::number(radio.frequencyKhz / 1000.0, 'f', 3));
        setChoice(bandwidth_, radio.bandwidthHz, bandwidthText(radio.bandwidthHz));
        setChoice(spreading_, radio.spreadingFactor, ui("SF%1").arg(radio.spreadingFactor));
        setChoice(coding_, radio.codingRate, ui("4/%1").arg(radio.codingRate));
        power_->setText(QString::number(radio.powerDbm));
        const auto repeated = client_.radioRepeatEnabled();
        repeat_->setCurrentIndex(repeated.value_or(false) ? 1 : 0);
        repeat_->setEnabled(repeated.has_value());
        const int width = client_.radioPathHashMode();
        setChoice(pathHash_, width, ui("Режим %1").arg(width));
        pathHash_->setEnabled(width >= 0);
    } else {
        addContacts_->setCurrentIndex((radio.manualAddContacts & 1) ? 1 : 0);
    }
}

void RadioEditorPanel::choosePreset(int index) {
    if (index < 1 || index > static_cast<int>(sizeof(presets) / sizeof(presets[0])))
        return;
    const Preset& preset = presets[index - 1];
    frequency_->setText(QString::number(preset.frequencyKhz / 1000.0, 'f', 3));
    setChoice(bandwidth_, preset.bandwidthHz, bandwidthText(preset.bandwidthHz));
    setChoice(spreading_, preset.spreading, ui("SF%1").arg(preset.spreading));
    setChoice(coding_, preset.coding, ui("4/%1").arg(preset.coding));
}

void RadioEditorPanel::save() {
    if (!client_.connected() || request_)
        return;
    const auto& radio = client_.radioIdentity();
    std::vector<meshcore::Bytes> commands;
    if (page_ == Name) {
        const QString name = name_->text().trimmed();
        const QByteArray bytes = name.toUtf8();
        if (bytes.isEmpty() || bytes.size() > 31) {
            setFeedback(ui("Введите имя до 31 байта UTF-8."));
            return;
        }
        if (name != client_.radioName()) {
            meshcore::Bytes frame{8};
            frame.insert(frame.end(), bytes.constData(), bytes.constData() + bytes.size());
            commands.push_back(std::move(frame));
        }
    } else if (page_ == Frequency) {
        bool frequencyValid = false;
        bool powerValid = false;
        QString frequency = frequency_->text().trimmed();
        frequency.replace(QLatin1Char(','), QLatin1Char('.'));
        const double mhz = frequency.toDouble(&frequencyValid);
        const int power = power_->text().trimmed().toInt(&powerValid);
        const int bandwidth = selected(bandwidth_);
        const int spreading = selected(spreading_);
        const int coding = selected(coding_);
        if (!frequencyValid || !std::isfinite(mhz) || mhz < 150 || mhz > 2500 ||
                !powerValid || power < -9 || power > radio.maxPowerDbm ||
                bandwidth < 7000 || bandwidth > 500000 || spreading < 5 || spreading > 12 ||
                coding < 5 || coding > 8) {
            setFeedback(ui("Проверьте частоту, полосу, SF, CR и мощность от −9 до %1 дБм.").arg(radio.maxPowerDbm));
            return;
        }
        const quint32 frequencyKhz = qRound(mhz * 1000);
        const auto radioRepeat = client_.radioRepeatEnabled();
        const bool repeat = selected(repeat_) != 0;
        if (frequencyKhz != radio.frequencyKhz || bandwidth != int(radio.bandwidthHz) ||
                spreading != radio.spreadingFactor || coding != radio.codingRate ||
                (radioRepeat && repeat != *radioRepeat)) {
            meshcore::Bytes frame{11};
            append32(frame, frequencyKhz);
            append32(frame, bandwidth);
            frame.push_back(static_cast<std::uint8_t>(spreading));
            frame.push_back(static_cast<std::uint8_t>(coding));
            if (radioRepeat)
                frame.push_back(repeat ? 1 : 0);
            commands.push_back(std::move(frame));
        }
        if (power != radio.powerDbm)
            commands.push_back({12, static_cast<std::uint8_t>(power)});
        const int initialHash = client_.radioPathHashMode();
        const int selectedHash = selected(pathHash_);
        if (initialHash >= 0 && selectedHash >= 0 && selectedHash <= 2 && selectedHash != initialHash)
            commands.push_back({61, 0, static_cast<std::uint8_t>(selectedHash)});
    } else {
        const int addMode = (radio.manualAddContacts & ~1) | selected(addContacts_);
        if (addMode != radio.manualAddContacts)
            commands.push_back({38, static_cast<std::uint8_t>(addMode)});
    }
    if (commands.empty()) {
        setFeedback(ui("Изменений нет."));
        return;
    }
    const int request = client_.nextConsoleRequestId();
    if (client_.applyRadioSettings(request, commands)) {
        request_ = request;
        setFeedback(ui("Сохраняем на радио…"));
        updateStatus();
    } else {
        setFeedback(ui("Дождитесь окончания текущего запроса."));
    }
}

void RadioEditorPanel::refreshContacts() {
    if (client_.connected() && !client_.busy()) {
        refreshing_ = true;
        client_.refresh();
        setFeedback(ui("Загружаем контакты с радио…"));
        updateStatus();
    }
}

void RadioEditorPanel::updateStatus() {
    const bool connected = client_.connected();
    if (connected && !identified_) {
        identified_ = true;
        loadValues();
        setFeedback(QString());
    } else if (!connected && (!identified_ || !request_)) {
        identified_ = false;
        setFeedback(ui("Подключитесь к ноде, чтобы изменить параметры."));
    }
    inputs_->setEnabled(connected && !request_);
    save_->setEnabled(connected && !request_ && !client_.busy());
    if (contactCount_)
        contactCount_->setText(QString::number(client_.nodes().size()));
    if (refresh_)
        refresh_->setEnabled(connected && !client_.busy() && !request_);
    if (refreshing_ && (!connected || !client_.busy())) {
        refreshing_ = false;
        setFeedback(connected ? ui("Загружено узлов: %1").arg(client_.nodes().size()) :
                                ui("Связь с радио прервана."));
    }
}

void RadioEditorPanel::receiveResult(int request, bool success, const QString& status) {
    if (request != request_)
        return;
    request_ = 0;
    if (success && client_.connected())
        loadValues();
    updateStatus();
    setFeedback(status);
}

void RadioEditorPanel::setFeedback(const QString& feedback) {
    feedback_->setText(feedback);
    feedback_->setVisible(!feedback.isEmpty());
}
