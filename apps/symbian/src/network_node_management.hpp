#ifndef OMMESH_BELLE_NETWORK_NODE_MANAGEMENT_HPP
#define OMMESH_BELLE_NETWORK_NODE_MANAGEMENT_HPP

#include <QtGui/QWidget>
#include <memory>

class Client;
struct NetworkNode;

class NetworkNodeManagementPanel : public QWidget {
    Q_OBJECT

public:
    enum Page { Neighbours, Settings, Access };

    NetworkNodeManagementPanel(Client& client, const NetworkNode& node, Page page,
                               QWidget* parent = nullptr);
    ~NetworkNodeManagementPanel() override;

signals:
    void openNode(const QString& id);

protected:
    void hideEvent(QHideEvent* event) override;

private slots:
    void refresh();
    void loadMore();
    void changeGroup(int group);
    void applySettings();
    void keepRadioSettings();
    void clearDraft();
    void changePassword();
    void searchNeighbours();
    void resetNeighbours();
    void finishNeighbourSearch();
    void chooseNeighbour();
    void chooseAccessRow(int index);
    void grantRole();
    void changeRole();
    void removeRole();
    void confirmAction();
    void dismissAction();
    void updateAvailability();
    void settingEdited();
    void onBinaryResult(int requestId, int operation, bool success,
                        const QByteArray& data, const QString& error);
    void onConsoleResult(int requestId, int operation, bool success, const QString& text);

private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif
