#ifndef OMMESH_BELLE_SETTINGS_RADIO_EDITOR_HPP
#define OMMESH_BELLE_SETTINGS_RADIO_EDITOR_HPP

#include <QtGui/QWidget>

class Client;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

class RadioEditorPanel : public QWidget {
    Q_OBJECT

public:
    enum Page { Name, Frequency, Contacts };
    RadioEditorPanel(Client& client, Page page, QWidget* parent = nullptr);

private slots:
    void choosePreset(int index);
    void save();
    void refreshContacts();
    void updateStatus();
    void receiveResult(int request, bool success, const QString& status);

private:
    void loadValues();
    void setFeedback(const QString& feedback);

    Client& client_;
    const Page page_;
    QWidget* inputs_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* frequency_ = nullptr;
    QComboBox* bandwidth_ = nullptr;
    QComboBox* spreading_ = nullptr;
    QComboBox* coding_ = nullptr;
    QLineEdit* power_ = nullptr;
    QComboBox* repeat_ = nullptr;
    QComboBox* pathHash_ = nullptr;
    QLabel* contactCount_ = nullptr;
    QComboBox* addContacts_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* save_ = nullptr;
    QLabel* feedback_ = nullptr;
    bool identified_ = false;
    bool refreshing_ = false;
    int request_ = 0;
};

#endif
