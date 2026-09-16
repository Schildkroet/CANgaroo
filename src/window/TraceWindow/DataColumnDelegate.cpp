/*
  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "DataColumnDelegate.h"
#include "BaseTraceViewModel.h"
#include "core/ThemeManager.h"

#include <QPainter>
#include <QApplication>

DataColumnDelegate::DataColumnDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void DataColumnDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    // Every row is painted by this path, changed bytes or not. Falling back to
    // QStyledItemDelegate::paint() for unchanged rows placed the text slightly
    // differently, so a row's spacing jumped each time its mask toggled.
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();

    // The text rect depends on opt.text, so take it before clearing the text.
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
    const QString text = opt.text;
    opt.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    if (text.isEmpty()) { return; }

    const bool selected = opt.state & QStyle::State_Selected;
    QColor normalColor = opt.palette.color(
        selected ? QPalette::Active : QPalette::Normal,
        selected ? QPalette::HighlightedText : QPalette::Text);

    // ForegroundRole carries the row color: error red, or the aggregated view's
    // stale-message fade (alpha). Selection keeps the highlighted-text color.
    const QVariant fgVariant = index.data(Qt::ForegroundRole);
    if (!selected && fgVariant.canConvert<QColor>()) {
        normalColor = fgVariant.value<QColor>();
    }

    const bool isDark = ThemeManager::instance().isDarkMode();
    QColor changedColor = isDark ? QColor(210, 120, 0) : QColor(180, 90, 0);
    changedColor.setAlpha(normalColor.alpha());

    // Models return no mask for error frames, whose text is not per-byte.
    const uint64_t changedMask = index.data(BaseTraceViewModel::ChangedBytesRole).value<uint64_t>();

    constexpr int drawFlags = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine;
    const int textMargin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, opt.widget) + 1;
    const QRect area = textRect.adjusted(textMargin, 0, -textMargin, 0);
    const QFontMetrics fm(opt.font);

    painter->save();
    painter->setFont(opt.font);
    painter->setClipRect(textRect);

    if (changedMask == 0) {
        painter->setPen(normalColor);
        painter->drawText(area, drawFlags, text);
        painter->restore();
        return;
    }

    // Text format: "AB CD EF " (hex) or "A B C " (ASCII), one token per byte.
    // Token x positions come from the advance of the whole prefix, so the layout
    // matches the single drawText() above exactly.
    int byteIndex = 0;
    qsizetype pos = 0;
    while (pos < text.size()) {
        qsizetype end = text.indexOf(QLatin1Char(' '), pos);
        if (end < 0) { end = text.size(); }

        if (end > pos) {
            const bool changed = byteIndex < 64 && ((changedMask >> byteIndex) & 1ULL);
            painter->setPen(changed ? changedColor : normalColor);
            const int x = area.left() + fm.horizontalAdvance(text.left(pos));
            painter->drawText(QRect(x, area.top(), area.right() - x + 1, area.height()),
                              drawFlags, text.mid(pos, end - pos));
            ++byteIndex;
        }
        pos = end + 1;
    }

    painter->restore();
}
