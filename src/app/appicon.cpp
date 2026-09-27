#include "app/appicon.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace {

QPixmap renderIcon(int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 128.0, size / 128.0);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x1b, 0x26, 0x33));
    p.drawRoundedRect(QRectF(8, 8, 112, 112), 26, 26);

    p.setPen(QPen(QColor(0x34, 0x46, 0x5a), 2.5));
    for (int y : {40, 64, 88})
        p.drawLine(QPointF(22, y), QPointF(106, y));

    QPainterPath trace;
    trace.moveTo(22, 76);
    trace.lineTo(42, 76);
    trace.lineTo(52, 50);
    trace.lineTo(62, 96);
    trace.lineTo(73, 32);
    trace.lineTo(83, 76);
    trace.lineTo(106, 76);
    QPen pen(QColor(0x5f, 0xe3, 0xbd), 8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(trace);
    return pm;
}

} // namespace

QIcon pulseScopeIcon()
{
    QIcon icon;
    for (int size : {16, 22, 24, 32, 48, 64, 128, 256})
        icon.addPixmap(renderIcon(size));
    return icon;
}
