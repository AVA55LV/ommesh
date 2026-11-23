#ifndef OMMESH_BELLE_NODE_MAP_HPP
#define OMMESH_BELLE_NODE_MAP_HPP

#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtGui/QWidget>
#include <functional>

class QButtonGroup;
class QHideEvent;
class QKeyEvent;
class QLabel;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
struct NetworkNode;

class NodeMap : public QWidget {
    Q_OBJECT

public:
    enum Source { Offline = 0, Loopback = 3 };
    enum OfflineRegion { OmskCity, OmskOblast, Siberia, World };
    explicit NodeMap(QWidget* parent = nullptr);
    ~NodeMap() override;

    void setTrace(const std::function<void(const char*, int)>& trace);
    void setNodes(const QVector<NetworkNode>& nodes);
    void setSource(Source source);
    void setOfflineRegion(OfflineRegion region);
    static Source sourceFromPreference(int value);
    QString statusText() const;

signals:
    void nodeSelected(const QString& id);
    void settingsRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void loadVisibleTiles();
    void loadVisibleBuildings();
    void readTileData();
    void finishCurrentReply();
    void finishTile();
    void tileTimedOut();
    void abortFailedTile();

private:
    struct State;
    State* state_;
};

class MapSettingsPanel : public QWidget {
    Q_OBJECT

public:
    explicit MapSettingsPanel(QWidget* parent = nullptr);
    void reload();

signals:
    void saved();

private slots:
    void selectMode(int mode);
    void scheduleSave();
    void save();

private:
    QButtonGroup* modes_;
    QButtonGroup* regions_;
    QWidget* regionChoices_;
    QLabel* instructions_;
    QLabel* validation_;
    bool saveQueued_ = false;
};

#endif
