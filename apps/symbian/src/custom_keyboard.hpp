#ifndef OMMESH_BELLE_CUSTOM_KEYBOARD_HPP
#define OMMESH_BELLE_CUSTOM_KEYBOARD_HPP

#include "native_vibration.hpp"

#include <QtCore/QBasicTimer>
#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QVector>
#include <QtGui/QWidget>

class QLabel;
class QPainter;

namespace NativeKeyboard {

class CustomKeyboard : public QWidget {
    Q_OBJECT

public:
    explicit CustomKeyboard(QWidget* parent);
    ~CustomKeyboard() override;
    void configure(const QString& theme, bool vibration, bool numeric, bool multiline);
    QSize sizeHint() const override;

signals:
    void textEntered(const QString& text);
    void keyEntered(int key);
    void hideRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void timerEvent(QTimerEvent* event) override;

private:
    enum class Kind { Letter, Text, Shift, Layout, Space, Backspace, Enter, Hide };
    struct Key {
        Kind kind;
        QString text;
        int weight;
        QRect area;
        QString holdText{};
    };

    void buildKeys();
    void addRow(int row, QVector<Key> keys, int inset = 0);
    QVector<Key> textKeys(const QString& characters, Kind kind = Kind::Text) const;
    int keyAt(const QPoint& point) const;
    void drawKey(QPainter& painter, int index) const;
    QString keyLabel(const Key& key, const QString& text) const;
    void showHoldPreview(const Key& key);
    void clearHold();
    void activate(const Key& key);
    void switchLanguage();
    void checkSpaceSwipe(const QPoint& point);
    void stopAnimations();

    QVector<Key> keys_;
    QString theme_;
    NativeVibration vibration_;
    QBasicTimer repeatTimer_;
    QBasicTimer releaseTimer_;
    QBasicTimer holdTimer_;
    QElapsedTimer releaseClock_;
    QPointer<QLabel> holdPreview_;
    QPoint pressPoint_;
    int pressed_ = -1;
    int released_ = -1;
    bool repeatStarted_ = false;
    bool holdReady_ = false;
    bool insidePress_ = false;
    bool spaceSwiped_ = false;
    bool russian_ = true;
    bool shift_ = false;
    bool vibrationEnabled_ = false;
    bool numeric_ = false;
    bool multiline_ = false;
    int symbols_ = 0;
};

} // namespace NativeKeyboard

#endif
