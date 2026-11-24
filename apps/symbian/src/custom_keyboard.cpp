#include "custom_keyboard.hpp"

#include <QtCore/QSettings>
#include <QtCore/QTimerEvent>
#include <QtGui/QLabel>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

namespace NativeKeyboard {
namespace {

const char languageSetting[] = "ui/customKeyboardLanguage";
const int rowCount = 4;
const int pulseMs = 22;
const int firstRepeatMs = 410;
const int repeatMs = 84;
const int releaseMs = 87;
const int holdMs = 450;

struct Colors {
    QRgb panel, key, function, stroke, text, accent, accentText;
};

QString utf8(const char* text) { return QString::fromUtf8(text); }

Colors colors(const QString& theme) {
    if (theme == QLatin1String("belle"))
        return {0xdae1e7, 0xf5f7f8, 0xbacbd9, 0x9baebb, 0x152a39, 0x22739e, 0xffffff};
    if (theme == QLatin1String("midnight"))
        return {0x1e1932, 0x322a4b, 0x493b66, 0x5a4e7d, 0xf5edff, 0xc2a8ff, 0x2d2051};
    if (theme == QLatin1String("sand"))
        return {0xeadbbf, 0xfff5e4, 0xddc6a2, 0xb99c74, 0x39291e, 0x904b30, 0xffffff};
    if (theme == QLatin1String("forest"))
        return {0x182d28, 0x26423a, 0x33564a, 0x547567, 0xedf7ef, 0xa5d7a2, 0x203b2d};
    if (theme == QLatin1String("black"))
        return {0x0c0c0c, 0x2b2b2b, 0x1b1b1b, 0x242424, 0xf5f5f5, 0x62abed, 0x0d1620};
    return {0x182228, 0x293a42, 0x354b51, 0x49616a, 0xeff5f6, 0x68d5be, 0x18342f};
}

QColor blend(QRgb base, QRgb accent, int percent) {
    const int remaining = 100 - percent;
    return QColor((qRed(base) * remaining + qRed(accent) * percent) / 100,
                  (qGreen(base) * remaining + qGreen(accent) * percent) / 100,
                  (qBlue(base) * remaining + qBlue(accent) * percent) / 100);
}

void strokePath(QPainter& painter, const QPointF& point, const QPointF* offsets, int length) {
    QPainterPath shape;
    shape.moveTo(point + offsets[0]);
    for (int i = 1; i < length; ++i)
        shape.lineTo(point + offsets[i]);
    painter.drawPath(shape);
}

} // namespace

CustomKeyboard::CustomKeyboard(QWidget* parent) : QWidget(parent) {
    setObjectName(QLatin1String("customKeyboard"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoMousePropagation);
    russian_ = QSettings().value(QLatin1String(languageSetting)).toString() != QLatin1String("en");
    buildKeys();
}

CustomKeyboard::~CustomKeyboard() {
    delete holdPreview_.data();
}

void CustomKeyboard::configure(const QString& theme, bool vibration, bool numeric, bool multiline) {
    if (numeric_ != numeric) {
        stopAnimations();
        numeric_ = numeric;
        symbols_ = 0;
        shift_ = false;
        buildKeys();
    }
    theme_ = theme;
    vibrationEnabled_ = vibration;
    multiline_ = multiline;
    update();
}

QSize CustomKeyboard::sizeHint() const { return QSize(360, 240); }

QVector<CustomKeyboard::Key> CustomKeyboard::textKeys(const QString& characters, Kind kind) const {
    QVector<Key> row;
    row.reserve(characters.size());
    for (int i = 0; i < characters.size(); ++i) {
        const QChar letter = characters.at(i);
        Key key = {kind, QString(letter), 10, QRect()};
        if (kind == Kind::Letter) {
            if (letter == QChar(0x0435))
                key.holdText = QChar(0x0451); // е → ё
            else if (letter == QChar(0x044c))
                key.holdText = QChar(0x044a); // ь → ъ
        }
        row.append(key);
    }
    return row;
}

void CustomKeyboard::addRow(int row, QVector<Key> keys, int inset) {
    const int side = 4 + inset;
    const int interior = qMax(0, width() - 2 * side);
    const int rowsHeight = qMax(0, height() - 9);
    const int top = 5 + rowsHeight * row / rowCount;
    const int bottom = 5 + rowsHeight * (row + 1) / rowCount;
    int total = 0;
    for (const Key& key : keys)
        total += key.weight;
    int units = 0;
    for (Key& key : keys) {
        const int x = side + interior * units / total;
        units += key.weight;
        key.area = QRect(x, top, side + interior * units / total - x, bottom - top);
        keys_.append(key);
    }
}

void CustomKeyboard::buildKeys() {
    keys_.clear();
    const Key enter = {Kind::Enter, QString(), 14, QRect()};
    const Key hide = {Kind::Hide, QString(), 11, QRect()};
    const Key backspace = {Kind::Backspace, QString(), 13, QRect()};
    if (numeric_) {
        addRow(0, textKeys(QLatin1String("123")) << Key{Kind::Backspace, QString(), 10, QRect()});
        addRow(1, textKeys(QLatin1String("456-")));
        addRow(2, textKeys(QLatin1String("789")) << Key{Kind::Hide, QString(), 10, QRect()});
        addRow(3, textKeys(QLatin1String(",0.")) << Key{Kind::Enter, QString(), 10, QRect()});
        return;
    }

    QVector<Key> last;
    if (symbols_ != 0) {
        const bool more = symbols_ == 2;
        addRow(0, textKeys(more ? utf8("~^€₽$£<>[]") : QString::fromLatin1("1234567890")));
        addRow(1, textKeys(more ? utf8("\\|{}_№…«»=") : QString::fromLatin1("@#%&*()-+=")));
        auto third = textKeys(more ? utf8("!?\"':;/,.") : QString::fromLatin1(".,:;!?\"'/"));
        third.prepend({Kind::Shift, more ? QString::fromLatin1("123") : QString::fromLatin1("#+="), 14, QRect()});
        third.append(backspace);
        addRow(2, third);
        last.append({Kind::Layout, russian_ ? utf8("АБВ") : QString::fromLatin1("ABC"), 16, QRect()});
        last.append({Kind::Text, QString::fromLatin1(","), 10, QRect()});
    } else {
        addRow(0, textKeys(russian_ ? utf8("йцукенгшщзх") : QString::fromLatin1("qwertyuiop"), Kind::Letter));
        addRow(1, textKeys(russian_ ? utf8("фывапролджэ") : QString::fromLatin1("asdfghjkl"), Kind::Letter),
               russian_ ? 0 : width() / 22);
        auto third = textKeys(russian_ ? utf8("ячсмитьбю") : QString::fromLatin1("zxcvbnm"), Kind::Letter);
        const int outer = russian_ ? 12 : 15;
        third.prepend({Kind::Shift, QString(), outer, QRect()});
        third.append({Kind::Backspace, QString(), outer, QRect()});
        addRow(2, third);
        last.append({Kind::Layout, QString::fromLatin1("?123"), 16, QRect()});
        last.append({Kind::Text, QString::fromLatin1(","), 10, QRect()});
        if (!russian_)
            last.append({Kind::Text, QString::fromLatin1("'"), 10, QRect()});
    }
    const bool russianLetters = russian_ && symbols_ == 0;
    last.append({Kind::Space, russian_ ? utf8("Русский") : QString::fromLatin1("English"),
                 russianLetters ? 51 : 41, QRect()});
    last.append({Kind::Text, QString(QLatin1Char('.')), 10, QRect()});
    last.append(enter);
    last.append(hide);
    addRow(3, last);
}

int CustomKeyboard::keyAt(const QPoint& point) const {
    for (int i = 0; i < keys_.size(); ++i) {
        if (keys_.at(i).area.contains(point))
            return i;
    }
    return -1;
}

void CustomKeyboard::drawKey(QPainter& painter, int index) const {
    const Key& key = keys_.at(index);
    QRect box = key.area.adjusted(2, 2, -2, -2);
    if (box.isEmpty())
        return;
    const Colors palette = colors(theme_);
    const bool blackTheme = theme_ == QLatin1String("black");
    const bool pressed = index == pressed_ && insidePress_;
    const bool shiftKey = key.kind == Kind::Shift;
    const bool chosen = (key.kind == Kind::Enter && !blackTheme) || (shiftKey && shift_);
    const bool regular = key.kind == Kind::Letter || key.kind == Kind::Text;
    const bool letterFill = regular || (blackTheme && key.kind == Kind::Space);
    const QRgb normalFill = chosen ? palette.accent : (letterFill ? palette.key : palette.function);
    const QRgb ink = chosen ? palette.accentText : (blackTheme && shiftKey ? palette.accent : palette.text);
    int highlight = pressed ? 68 : 0;
    if (index == released_)
        highlight = qMax(0, 50 - int(releaseClock_.elapsed() * 50 / releaseMs));
    if (pressed)
        box.translate(0, 1);
    painter.setPen(QColor::fromRgb(palette.stroke));
    const QRgb pressTint = chosen ? palette.accentText : palette.accent;
    const int tint = chosen ? highlight / 4 : (blackTheme ? highlight / 2 : highlight);
    painter.setBrush(blend(normalFill, pressTint, tint));
    painter.drawRoundedRect(box, 6, 6);
    painter.setPen(QColor::fromRgb(ink));
    const QPointF center = QRectF(box).center();

    if (!key.text.isEmpty()) {
        QFont font = painter.font();
        const int pixels = regular ? qBound(13, box.height() / 2, 24) : qBound(11, box.height() / 3, 17);
        font.setPixelSize(pixels);
        font.setWeight(regular ? QFont::Normal : QFont::DemiBold);
        painter.setFont(font);
        painter.drawText(box, Qt::AlignCenter, keyLabel(key, key.text));
        if (!key.holdText.isEmpty()) {
            font.setPixelSize(qBound(9, box.height() / 5, 12));
            painter.setFont(font);
            painter.drawText(box.adjusted(2, 3, -3, 0), Qt::AlignRight | Qt::AlignTop,
                             keyLabel(key, key.holdText));
        }
        if (key.kind != Kind::Space || box.width() < 83)
            return;
        const QRgb arrowInk = blackTheme ? palette.text : palette.accent;
        painter.setPen(QPen(QColor::fromRgb(arrowInk), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF left[] = {{3, -4}, {0, 0}, {3, 4}};
        const QPointF right[] = {{-3, -4}, {0, 0}, {-3, 4}};
        strokePath(painter, QPointF(box.left() + 9, center.y()), left, 3);
        strokePath(painter, QPointF(box.right() - 9, center.y()), right, 3);
        return;
    }

    painter.setPen(QPen(QColor::fromRgb(ink), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (key.kind == Kind::Shift) {
        const QPointF shape[] = {{-8, 1}, {0, -7}, {8, 1}, {4, 1}, {4, 7}, {-4, 7}, {-4, 1}, {-8, 1}};
        strokePath(painter, center, shape, 8);
    } else if (key.kind == Kind::Backspace) {
        const QPointF shape[] = {{-11, 0}, {-5, -6}, {10, -6}, {10, 6}, {-5, 6}, {-11, 0}};
        strokePath(painter, center, shape, 6);
        painter.drawLine(center + QPointF(-1, -2), center + QPointF(4, 2));
        painter.drawLine(center + QPointF(-1, 2), center + QPointF(4, -2));
    } else if (key.kind == Kind::Enter && multiline_) {
        const QPointF shape[] = {{9, -6}, {9, 3}, {-7, 3}, {-2, -2}};
        strokePath(painter, center, shape, 4);
        painter.drawLine(center + QPointF(-7, 3), center + QPointF(-2, 8));
    } else if (key.kind == Kind::Enter) {
        const QPointF shape[] = {{-7, 0}, {-2, 5}, {8, -5}};
        strokePath(painter, center, shape, 3);
    } else if (key.kind == Kind::Hide) {
        const QPointF shape[] = {{-7, -3}, {0, 3}, {7, -3}};
        strokePath(painter, center, shape, 3);
        painter.drawLine(center + QPointF(-6, 7), center + QPointF(6, 7));
    }
}

void CustomKeyboard::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    const Colors palette = colors(theme_);
    painter.fillRect(event->rect(), QColor::fromRgb(palette.panel));
    painter.setPen(QColor::fromRgb(palette.stroke));
    painter.drawLine(0, 0, width(), 0);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int index = 0; index < keys_.size(); ++index) {
        if (event->rect().intersects(keys_.at(index).area))
            drawKey(painter, index);
    }
}

void CustomKeyboard::resizeEvent(QResizeEvent* event) {
    stopAnimations();
    buildKeys();
    QWidget::resizeEvent(event);
}

void CustomKeyboard::hideEvent(QHideEvent* event) {
    stopAnimations();
    symbols_ = 0;
    shift_ = false;
    buildKeys();
    QWidget::hideEvent(event);
}

QString CustomKeyboard::keyLabel(const Key& key, const QString& text) const {
    return key.kind == Kind::Letter && shift_ ? text.toUpper() : text;
}

void CustomKeyboard::showHoldPreview(const Key& key) {
    QWidget* container = parentWidget() ? parentWidget() : this;
    if (!holdPreview_) {
        // The sibling can extend over the editor when a key is in the first row.
        holdPreview_ = new QLabel(container);
        holdPreview_->setObjectName(QLatin1String("keyboardHoldPreview"));
        holdPreview_->setAttribute(Qt::WA_TransparentForMouseEvents);
        holdPreview_->setFocusPolicy(Qt::NoFocus);
        holdPreview_->setAlignment(Qt::AlignCenter);
    }
    const Colors palette = colors(theme_);
    holdPreview_->setStyleSheet(QString::fromLatin1(
        "QLabel { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; font-size: 24px; }")
        .arg(QColor::fromRgb(palette.key).name(), QColor::fromRgb(palette.text).name(),
             QColor::fromRgb(palette.accent).name()));
    holdPreview_->setText(keyLabel(key, key.holdText));
    const QSize bubble(qMin(container->width(), qMax(38, key.area.width())), 36);
    const QPoint keyOrigin = mapTo(container, key.area.topLeft());
    const int x = qBound(0, keyOrigin.x() + (key.area.width() - bubble.width()) / 2,
                         container->width() - bubble.width());
    const int y = qMax(0, keyOrigin.y() - bubble.height() - 3);
    holdPreview_->setGeometry(QRect(QPoint(x, y), bubble));
    holdPreview_->raise();
    holdPreview_->show();
}

void CustomKeyboard::clearHold() {
    holdTimer_.stop();
    holdReady_ = false;
    if (holdPreview_)
        holdPreview_->hide();
}

void CustomKeyboard::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (released_ >= 0)
        update(keys_.at(released_).area);
    stopAnimations();
    pressed_ = keyAt(event->pos());
    if (pressed_ < 0)
        return;
    event->accept();
    pressPoint_ = event->pos();
    insidePress_ = true;
    spaceSwiped_ = false;
    const Key& key = keys_.at(pressed_);
    update(key.area);
    if (vibrationEnabled_)
        vibration_.pulse(pulseMs);
    if (key.kind == Kind::Backspace) {
        repeatStarted_ = false;
        repeatTimer_.start(firstRepeatMs, this);
        emit keyEntered(Qt::Key_Backspace);
    } else if (!key.holdText.isEmpty()) {
        holdTimer_.start(holdMs, this);
    }
}

void CustomKeyboard::checkSpaceSwipe(const QPoint& point) {
    if (keys_.at(pressed_).kind != Kind::Space)
        return;
    const QPoint distance = point - pressPoint_;
    const int horizontal = qAbs(distance.x());
    if (horizontal >= qMax(22, width() / 11) && horizontal > 2 * qAbs(distance.y()))
        spaceSwiped_ = true;
}

void CustomKeyboard::mouseMoveEvent(QMouseEvent* event) {
    if (pressed_ < 0)
        return;
    checkSpaceSwipe(event->pos());
    const Key& key = keys_.at(pressed_);
    const bool inside = key.area.contains(event->pos());
    if (!inside && !key.holdText.isEmpty()) {
        stopAnimations();
        update(key.area);
        event->accept();
        return;
    }
    if (inside != insidePress_) {
        insidePress_ = inside;
        if (!inside)
            repeatTimer_.stop();
        update(keys_.at(pressed_).area);
    }
    event->accept();
}

void CustomKeyboard::mouseReleaseEvent(QMouseEvent* event) {
    if (pressed_ < 0 || event->button() != Qt::LeftButton)
        return;
    event->accept();
    checkSpaceSwipe(event->pos());
    repeatTimer_.stop();
    const int index = pressed_;
    Key key = keys_.at(index);
    if (holdReady_)
        key.text = key.holdText;
    clearHold();
    pressed_ = -1;
    if (spaceSwiped_) {
        switchLanguage();
        return;
    }
    if (key.area.contains(event->pos())) {
        released_ = index;
        releaseClock_.start();
        releaseTimer_.start(24, this);
        update(key.area);
        activate(key);
    } else {
        update(key.area);
    }
}

void CustomKeyboard::activate(const Key& key) {
    switch (key.kind) {
    case Kind::Letter: {
        const QString text = keyLabel(key, key.text);
        if (shift_) {
            shift_ = false;
            update();
        }
        emit textEntered(text);
        break;
    }
    case Kind::Text:
        emit textEntered(key.text);
        break;
    case Kind::Shift:
        if (symbols_) {
            stopAnimations();
            symbols_ = 3 - symbols_;
            buildKeys();
        } else {
            shift_ = !shift_;
        }
        update();
        break;
    case Kind::Layout:
        stopAnimations();
        symbols_ = symbols_ == 0 ? 1 : 0;
        shift_ = false;
        buildKeys();
        update();
        break;
    case Kind::Space:
        emit textEntered(QString::fromLatin1(" "));
        break;
    case Kind::Backspace:
        break;
    case Kind::Enter:
        emit keyEntered(Qt::Key_Return);
        break;
    case Kind::Hide:
        emit hideRequested();
        break;
    }
}

void CustomKeyboard::switchLanguage() {
    russian_ = !russian_;
    shift_ = false;
    QSettings settings;
    settings.setValue(QLatin1String(languageSetting), QLatin1String(russian_ ? "ru" : "en"));
    settings.sync();
    buildKeys();
    update();
}

void CustomKeyboard::stopAnimations() {
    repeatTimer_.stop();
    releaseTimer_.stop();
    clearHold();
    pressed_ = -1;
    released_ = -1;
}

void CustomKeyboard::timerEvent(QTimerEvent* event) {
    if (event->timerId() == repeatTimer_.timerId()) {
        if (!repeatStarted_) {
            repeatStarted_ = true;
            repeatTimer_.start(repeatMs, this);
        }
        emit keyEntered(Qt::Key_Backspace);
    } else if (event->timerId() == holdTimer_.timerId()) {
        holdTimer_.stop();
        holdReady_ = true;
        showHoldPreview(keys_.at(pressed_));
    } else if (event->timerId() == releaseTimer_.timerId()) {
        const QRect dirty = keys_.at(released_).area;
        if (releaseClock_.elapsed() >= releaseMs) {
            releaseTimer_.stop();
            released_ = -1;
        }
        update(dirty);
    } else {
        QWidget::timerEvent(event);
    }
}

} // namespace NativeKeyboard
