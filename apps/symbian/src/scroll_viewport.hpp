#pragma once

#include <QtGui/QAbstractScrollArea>
#include <QtGui/QScrollBar>

class ScrollViewportMargins {
public:
    ScrollViewportMargins(int horizontalInset, bool overlayVerticalBar)
        : horizontalInset_(horizontalInset), overlayVerticalBar_(overlayVerticalBar) {}

    int left() const { return horizontalInset_; }
    int right() const { return horizontalInset_ - verticalBarOverlap_; }

    bool update(const QAbstractScrollArea& area) {
        if (!overlayVerticalBar_)
            return false;
        const int targetWidth = area.contentsRect().width() - 2 * horizontalInset_;
        const int actualWidth = area.viewport()->width();
        if (targetWidth < 1 || actualWidth < 1)
            return false;
        // Qt can update the bar range and viewport in separate layout steps.
        // Account for the width reserved in the current geometry.
        const int barWidth = area.verticalScrollBar()->sizeHint().width();
        const int overlap = qBound(0, verticalBarOverlap_ + targetWidth - actualWidth, barWidth);
        if (overlap == verticalBarOverlap_)
            return false;
        verticalBarOverlap_ = overlap;
        return true;
    }

private:
    const int horizontalInset_;
    const bool overlayVerticalBar_;
    int verticalBarOverlap_ = 0;
};
