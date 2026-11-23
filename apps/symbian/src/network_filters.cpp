#include "network_filters.hpp"

#include <QtCore/QSettings>
#include <QtCore/QVariant>
#include <QtGui/QButtonGroup>
#include <QtGui/QCheckBox>
#include <QtGui/QComboBox>
#include <QtGui/QHBoxLayout>
#include <QtGui/QLabel>
#include <QtGui/QPushButton>
#include <QtGui/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace {

using namespace NetworkFilters;

constexpr int silencePeriods[] = {0, 7, 30, 90};
const char displayKey[] = "network/display";
const char orderKey[] = "network/order";
const char silenceKey[] = "network/silenceDays";
const char favoritesFirstKey[] = "network/favoritesFirst";

QString ui(const char* text) { return QString::fromUtf8(text); }

int checkedSilence(int days) {
    for (const int period : silencePeriods) {
        if (period == days)
            return days;
    }
    return 0;
}

bool infrastructure(const NetworkNode& node) {
    return node.role >= 2 && node.role <= 4;
}

bool shows(NetworkFilters::Display display, const NetworkNode& node, bool favorite, bool yours) {
    switch (display) {
    case Favorites: return favorite;
    case Yours: return yours;
    case People: return node.role == 1;
    case Repeaters: return node.role == 2;
    case Rooms: return node.role == 3;
    case Sensors: return node.role == 4;
    case All: return true;
    }
    return true;
}

double distanceOrder(const NetworkNode& origin, const NetworkNode& node) {
    constexpr double radiansPerMicrodegree = 3.14159265358979323846 / 180000000.0;
    const double latitude = origin.latitude * radiansPerMicrodegree;
    const double nodeLatitude = node.latitude * radiansPerMicrodegree;
    const double dLat = std::sin((nodeLatitude - latitude) / 2);
    const double dLon = std::sin((node.longitude - double(origin.longitude)) * radiansPerMicrodegree / 2);
    // The haversine is monotonic with great-circle distance and handles the date line.
    return dLat * dLat + std::cos(latitude) * std::cos(nodeLatitude) * dLon * dLon;
}

struct RankedNode {
    NetworkNode node;
    double distanceOrHops = std::numeric_limits<double>::infinity();
    bool favorite = false;
};

QLabel* label(const char* text, const char* name, QWidget* parent) {
    auto* result = new QLabel(ui(text), parent);
    result->setObjectName(QLatin1String(name));
    result->setTextFormat(Qt::PlainText);
    return result;
}

QVBoxLayout* group(QVBoxLayout* page) {
    auto* surface = new QWidget(page->parentWidget());
    surface->setObjectName(QLatin1String("settingsGroup"));
    surface->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(surface);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(5);
    page->addWidget(surface);
    return layout;
}

struct Choice {
    int id;
    const char* label;
    const char* name;
};

void addChoices(QVBoxLayout* layout, QButtonGroup* group, std::initializer_list<Choice> choices) {
    auto* row = new QHBoxLayout;
    row->setSpacing(5);
    for (const Choice& choice : choices) {
        auto* button = new QPushButton(ui(choice.label), layout->parentWidget());
        button->setObjectName(QLatin1String(choice.name));
        button->setProperty("networkFilterChip", true);
        button->setCheckable(true);
        button->setMinimumHeight(34);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        group->addButton(button, choice.id);
        row->addWidget(button, 1);
    }
    layout->addLayout(row);
}

}  // namespace

namespace NetworkFilters {

Options load() {
    const QSettings settings;
    const int display = settings.value(QLatin1String(displayKey), int(All)).toInt();
    const int order = settings.value(QLatin1String(orderKey), int(Recent)).toInt();
    return {display >= All && display <= Sensors ? static_cast<Display>(display) : All,
            order >= Recent && order <= FewestHops ? static_cast<Order>(order) : Recent,
            checkedSilence(settings.value(QLatin1String(silenceKey), 0).toInt()),
            settings.value(QLatin1String(favoritesFirstKey), false).toBool()};
}

void save(const Options& options) {
    QSettings settings;
    settings.setValue(QLatin1String(displayKey), int(options.display));
    settings.setValue(QLatin1String(orderKey), int(options.order));
    settings.setValue(QLatin1String(silenceKey), checkedSilence(options.silenceDays));
    settings.setValue(QLatin1String(favoritesFirstKey), options.favoritesFirst);
}

bool isDefault(const Options& options) {
    return options.display == All && options.order == Recent &&
           options.silenceDays == 0 && !options.favoritesFirst;
}

bool hasPosition(const NetworkNode& node) {
    return (node.latitude != 0 || node.longitude != 0) &&
           node.latitude >= -90000000 && node.latitude <= 90000000 &&
           node.longitude >= -180000000 && node.longitude <= 180000000;
}

QString countLabel(int nodes) {
    const int lastDigit = nodes % 10;
    const int lastTwo = nodes % 100;
    const char* unit = lastTwo >= 11 && lastTwo <= 14 ? "узлов" :
                       lastDigit == 1 ? "узел" : lastDigit >= 2 && lastDigit <= 4 ? "узла" : "узлов";
    return QString::number(nodes) + QLatin1Char(' ') + ui(unit);
}

QVector<NetworkNode> filter(const QVector<NetworkNode>& nodes, const NetworkNode& self,
                            const QSet<QString>& favorites, const QString& query,
                            const Options& options, quint32 now) {
    const QString needle = query.trimmed();
    const Order order = options.order == Nearest && !hasPosition(self) ? Recent : options.order;
    const quint32 silentSeconds = static_cast<quint32>(checkedSilence(options.silenceDays)) * 86400U;
    const bool hasSelf = !self.id.isEmpty() && !self.title.isEmpty();
    const bool includeSelf = hasSelf && (options.display == Yours ||
        (options.display == Favorites && favorites.contains(self.id)));

    QVector<RankedNode> ranked;
    ranked.reserve(nodes.size() + (includeSelf ? 1 : 0));
    bool foundSelf = false;
    const auto add = [&](const NetworkNode& node, bool isSelf) {
        const bool favorite = favorites.contains(node.id);
        const bool yours = isSelf || (infrastructure(node) && node.consoleAuthorized);
        if (!shows(options.display, node, favorite, yours))
            return;
        if (!needle.isEmpty() && !node.title.contains(needle, Qt::CaseInsensitive) &&
            !node.id.contains(needle, Qt::CaseInsensitive) &&
            !node.publicKey.contains(needle, Qt::CaseInsensitive))
            return;
        if (silentSeconds && needle.isEmpty() && infrastructure(node) && !favorite && !yours &&
            node.lastAdvert && now > node.lastAdvert && now - node.lastAdvert >= silentSeconds)
            return;
        RankedNode rankedNode{node};
        rankedNode.favorite = favorite;
        if (order == Nearest && hasPosition(node))
            rankedNode.distanceOrHops = distanceOrder(self, node);
        else if (order == FewestHops && node.path >= 0)
            rankedNode.distanceOrHops = node.path;
        ranked.append(rankedNode);
    };

    for (const NetworkNode& node : nodes) {
        const bool isSelf = hasSelf && node.id == self.id;
        foundSelf |= isSelf;
        add(node, isSelf);
    }
    if (includeSelf && !foundSelf)
        add(self, true);

    std::stable_sort(ranked.begin(), ranked.end(), [&](const RankedNode& a, const RankedNode& b) {
        if (options.favoritesFirst && a.favorite != b.favorite)
            return a.favorite;
        if (a.distanceOrHops != b.distanceOrHops)
            return a.distanceOrHops < b.distanceOrHops;
        if (order != Name && a.node.lastAdvert != b.node.lastAdvert)
            return a.node.lastAdvert > b.node.lastAdvert;
        const int byName = QString::compare(a.node.title, b.node.title, Qt::CaseInsensitive);
        return byName == 0 ? a.node.id < b.node.id : byName < 0;
    });

    QVector<NetworkNode> result;
    result.reserve(ranked.size());
    for (const RankedNode& node : ranked)
        result.append(node.node);
    return result;
}

}  // namespace NetworkFilters

NetworkFilterPanel::NetworkFilterPanel(const NetworkFilters::Options& options, QWidget* parent)
    : QWidget(parent), options_(options), displays_(new QButtonGroup(this)), orders_(new QButtonGroup(this)) {
    setObjectName(QLatin1String("networkFilterPanel"));
    setStyleSheet(QLatin1String(
        "QPushButton[networkFilterChip=\"true\"] { padding: 2px 3px; border-radius: 11px; font-size: 13px; }"
        "QPushButton#networkRefreshContacts { padding: 4px 6px; font-size: 14px; }"
        "QPushButton#networkFilterReset { padding: 4px 9px; font-size: 13px; }"));

    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(10, 0, 10, 8);
    page->setSpacing(6);
    page->addWidget(label("ПОКАЗАТЬ", "settingsSectionTitle", this));
    auto* display = group(page);
    addChoices(display, displays_, {
        {All, "Все", "networkFilterAll"}, {Favorites, "Избранные", "networkFilterFavorites"},
        {Yours, "Ваши", "networkFilterYours"}
    });
    addChoices(display, displays_, {
        {People, "Люди", "networkFilterPeople"}, {Repeaters, "Репитеры", "networkFilterRepeaters"},
        {Rooms, "Комнаты", "networkFilterRooms"}, {Sensors, "Датчики", "networkFilterSensors"}
    });

    auto* silenceRow = new QHBoxLayout;
    auto* silenceTitle = label("Скрывать молчащих", "networkSilenceTitle", display->parentWidget());
    silenceTitle->setWordWrap(true);
    silenceRow->addWidget(silenceTitle, 1);
    silence_ = new QComboBox(display->parentWidget());
    silence_->setObjectName(QLatin1String("networkSilence"));
    silence_->setProperty("accessibleName", silenceTitle->text());
    silence_->setMinimumHeight(38);
    silence_->setMinimumWidth(118);
    const char* silenceLabels[] = {"Не скрывать", "Неделю", "Месяц", "3 месяца"};
    for (int index = 0; index < int(sizeof(silencePeriods) / sizeof(silencePeriods[0])); ++index)
        silence_->addItem(ui(silenceLabels[index]), silencePeriods[index]);
    silenceRow->addWidget(silence_);
    display->addLayout(silenceRow);
    auto* silenceHint = label("Репитеры, комнаты и датчики. Ваши и избранные узлы остаются.",
                             "muted", display->parentWidget());
    silenceHint->setWordWrap(true);
    display->addWidget(silenceHint);

    page->addWidget(label("ПОРЯДОК", "settingsSectionTitle", this));
    auto* order = group(page);
    addChoices(order, orders_, {
        {Recent, "Недавние", "networkOrderRecent"}, {Name, "Имя", "networkOrderName"}
    });
    addChoices(order, orders_, {
        {Nearest, "Ближайшие", "networkOrderNearest"}, {FewestHops, "Меньше хопов", "networkOrderHops"}
    });
    orders_->button(Nearest)->setToolTip(ui("Для сортировки нужны координаты вашей ноды."));

    auto* actions = group(page);
    favoritesFirst_ = new QCheckBox(ui("Избранные сверху"), actions->parentWidget());
    favoritesFirst_->setObjectName(QLatin1String("networkFavoritesFirst"));
    favoritesFirst_->setMinimumHeight(34);
    actions->addWidget(favoritesFirst_);
    refresh_ = new QPushButton(ui("Загрузить все контакты из радио"), actions->parentWidget());
    refresh_->setObjectName(QLatin1String("networkRefreshContacts"));
    refresh_->setMinimumHeight(38);
    refresh_->setEnabled(false);
    actions->addWidget(refresh_);

    auto* summary = new QHBoxLayout;
    count_ = label("", "networkFilterCount", this);
    count_->setWordWrap(true);
    summary->addWidget(count_, 1);
    reset_ = new QPushButton(ui("Сбросить"), this);
    reset_->setObjectName(QLatin1String("networkFilterReset"));
    reset_->setMinimumHeight(34);
    summary->addWidget(reset_);
    page->addLayout(summary);
    page->addStretch(1);
    setContext(false, false, 0, 0);
    syncControls();

    connect(displays_, SIGNAL(buttonClicked(int)), this, SLOT(setDisplay(int)));
    connect(orders_, SIGNAL(buttonClicked(int)), this, SLOT(setOrder(int)));
    connect(silence_, SIGNAL(activated(int)), this, SLOT(setSilence(int)));
    connect(favoritesFirst_, SIGNAL(clicked(bool)), this, SLOT(setFavoritesFirst(bool)));
    connect(reset_, SIGNAL(clicked()), this, SLOT(reset()));
    connect(refresh_, SIGNAL(clicked()), this, SIGNAL(refreshRequested()));
}

NetworkFilters::Options NetworkFilterPanel::options() const { return options_; }

void NetworkFilterPanel::setContext(bool hasOwnPosition, bool canRefresh, int nodes, int missingPosition) {
    hasOwnPosition_ = hasOwnPosition;
    orders_->button(Nearest)->setEnabled(hasOwnPosition_);
    orders_->button(options_.order == Nearest && !hasOwnPosition_ ? Recent : options_.order)->setChecked(true);
    refresh_->setEnabled(canRefresh);
    QString summary = countLabel(nodes);
    if (missingPosition > 0)
        summary += ui(" · %1 без координат").arg(missingPosition);
    if (count_->text() != summary)
        count_->setText(summary);
}

void NetworkFilterPanel::syncControls() {
    displays_->button(options_.display)->setChecked(true);
    orders_->button(options_.order == Nearest && !hasOwnPosition_ ? Recent : options_.order)->setChecked(true);
    silence_->setCurrentIndex(silence_->findData(checkedSilence(options_.silenceDays)));
    favoritesFirst_->setChecked(options_.favoritesFirst);
    reset_->setVisible(!isDefault(options_));
}

void NetworkFilterPanel::notifyChanged() {
    reset_->setVisible(!isDefault(options_));
    emit optionsChanged();
}

void NetworkFilterPanel::setDisplay(int id) {
    const auto display = static_cast<NetworkFilters::Display>(id);
    if (options_.display == display)
        return;
    options_.display = display;
    notifyChanged();
}

void NetworkFilterPanel::setOrder(int id) {
    const Order order = static_cast<Order>(id);
    if (options_.order == order || (order == Nearest && !hasOwnPosition_))
        return;
    options_.order = order;
    notifyChanged();
}

void NetworkFilterPanel::setSilence(int index) {
    const int days = silence_->itemData(index).toInt();
    if (options_.silenceDays == days)
        return;
    options_.silenceDays = days;
    notifyChanged();
}

void NetworkFilterPanel::setFavoritesFirst(bool enabled) {
    options_.favoritesFirst = enabled;
    notifyChanged();
}

void NetworkFilterPanel::reset() {
    options_ = Options{};
    syncControls();
    emit optionsChanged();
}
