#ifndef OMMESH_BELLE_NETWORK_FILTERS_HPP
#define OMMESH_BELLE_NETWORK_FILTERS_HPP

#include "client.hpp"

#include <QtCore/QSet>
#include <QtGui/QWidget>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

namespace NetworkFilters {

enum Display { All, Favorites, Yours, People, Repeaters, Rooms, Sensors };
enum Order { Recent, Name, Nearest, FewestHops };

struct Options {
    Display display = All;
    Order order = Recent;
    int silenceDays = 0;
    bool favoritesFirst = false;
};

Options load();
void save(const Options& options);
bool isDefault(const Options& options);
bool hasPosition(const NetworkNode& node);
QString countLabel(int nodes);
QVector<NetworkNode> filter(const QVector<NetworkNode>& nodes, const NetworkNode& self,
                            const QSet<QString>& favorites, const QString& query,
                            const Options& options, quint32 now);

}  // namespace NetworkFilters

class NetworkFilterPanel : public QWidget {
    Q_OBJECT

public:
    explicit NetworkFilterPanel(const NetworkFilters::Options& options, QWidget* parent = nullptr);

    NetworkFilters::Options options() const;
    void setContext(bool hasOwnPosition, bool canRefresh, int nodes, int missingPosition);

signals:
    void optionsChanged();
    void refreshRequested();

private slots:
    void setDisplay(int id);
    void setOrder(int id);
    void setSilence(int index);
    void setFavoritesFirst(bool enabled);
    void reset();

private:
    void syncControls();
    void notifyChanged();

    NetworkFilters::Options options_;
    QButtonGroup* displays_;
    QButtonGroup* orders_;
    QComboBox* silence_;
    QCheckBox* favoritesFirst_;
    QLabel* count_;
    QPushButton* refresh_;
    QPushButton* reset_;
    bool hasOwnPosition_ = false;
};

#endif
