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
        const QScrollBar* bar = area.verticalScrollBar();
        const auto policy = area.verticalScrollBarPolicy();
        const bool reservesSpace = policy == Qt::ScrollBarAlwaysOn ||
            (policy == Qt::ScrollBarAsNeeded && bar->minimum() < bar->maximum());
        const int overlap = reservesSpace ? bar->sizeHint().width() : 0;
        if (overlap == verticalBarOverlap_)
            return false;
        // Qt raises the bar above the viewport. Restore the width it reserved.
        verticalBarOverlap_ = overlap;
        return true;
    }

private:
    const int horizontalInset_;
    const bool overlayVerticalBar_;
    int verticalBarOverlap_ = 0;
};
