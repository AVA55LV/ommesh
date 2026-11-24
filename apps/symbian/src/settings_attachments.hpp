#ifndef OMMESH_BELLE_SETTINGS_ATTACHMENTS_HPP
#define OMMESH_BELLE_SETTINGS_ATTACHMENTS_HPP

#include <QtGui/QDialog>
#include <QtGui/QWidget>
#include <memory>

namespace Attachments {
bool ready();
QString statusText();
}

class AttachmentSettingsPanel : public QWidget {
    Q_OBJECT

public:
    explicit AttachmentSettingsPanel(QWidget* parent = nullptr);
    ~AttachmentSettingsPanel() override;

signals:
    void saved();

private slots:
    void chooseCertificate();
    void certificateSelected(const QString& path);
    void save();
    void testBucket();
    void transferFinished();

private:
    struct State;
    std::unique_ptr<State> state_;
};

class AttachmentUploadPanel : public QDialog {
    Q_OBJECT

public:
    AttachmentUploadPanel(const QString& draft, int maximumUtf8Bytes, QWidget* parent = nullptr);
    ~AttachmentUploadPanel() override;

signals:
    void linkReady(const QString& link);

private slots:
    void chooseFile();
    void fileSelected(const QString& path);
    void upload();
    void transferFinished();

private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif
