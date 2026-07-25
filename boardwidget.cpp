/*
 * 【王佳怡负责】棋盘显示与鼠标交互实现
 * 文件职责：绘制 15×15 棋盘、星位、黑白棋子和最后落子标记，
 *          将鼠标像素位置换算为棋盘行列坐标。
 * 主要组件：QWidget、QPainter、QPaintEvent、QMouseEvent。
 */
#include "boardwidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <cmath>

BoardWidget::BoardWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(480, 480);
    setCursor(Qt::PointingHandCursor);
}

void BoardWidget::setModel(const GameModel *model)
{
    m_model = model;
    update();
}

void BoardWidget::setGameActive(bool active)
{
    m_gameActive = active;
    if (!active) {
        m_hovered = {};
    }
    update();
}

void BoardWidget::setInputEnabled(bool enabled)
{
    m_inputEnabled = enabled;
    if (!enabled) {
        m_hovered = {};
    }
    setCursor(enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

QSize BoardWidget::sizeHint() const
{
    return {680, 680};
}

int BoardWidget::heightForWidth(int width) const
{
    return width;
}

bool BoardWidget::hasHeightForWidth() const
{
    return true;
}

void BoardWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const Geometry geometry = boardGeometry();
    drawBoard(painter, geometry);
    drawStones(painter, geometry);
    if (!m_gameActive) {
        drawOverlay(painter);
    }
}

void BoardWidget::mouseMoveEvent(QMouseEvent *event)
{
    const BoardPoint point = m_inputEnabled ? pointAt(event->position()) : BoardPoint{};
    if (point.row != m_hovered.row || point.col != m_hovered.col) {
        m_hovered = point;
        update();
    }
}

void BoardWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_inputEnabled) {
        return;
    }

    const BoardPoint point = pointAt(event->position());
    if (point.isValid() && m_model && m_model->canPlace(point.row, point.col)) {
        emit intersectionClicked(point.row, point.col);
    }
}

void BoardWidget::leaveEvent(QEvent *)
{
    m_hovered = {};
    update();
}

BoardWidget::Geometry BoardWidget::boardGeometry() const
{
    constexpr qreal outerPadding = 28.0;
    const qreal side = std::max<qreal>(1.0, std::min(width(), height()) - outerPadding * 2.0);
    const qreal left = (width() - side) / 2.0;
    const qreal top = (height() - side) / 2.0;
    const qreal gridMargin = side * 0.06;
    const QRectF gridRect(left + gridMargin, top + gridMargin,
                          side - gridMargin * 2.0, side - gridMargin * 2.0);
    return {gridRect, gridRect.width() / (kBoardSize - 1)};
}

BoardPoint BoardWidget::pointAt(const QPointF &position) const
{
    const Geometry geometry = boardGeometry();
    const int col = qRound((position.x() - geometry.boardRect.left()) / geometry.spacing);
    const int row = qRound((position.y() - geometry.boardRect.top()) / geometry.spacing);
    if (row < 0 || row >= kBoardSize || col < 0 || col >= kBoardSize) {
        return {};
    }

    const QPointF target = positionOf(row, col, geometry);
    const qreal hitRadius = geometry.spacing * 0.46;
    if (QLineF(position, target).length() > hitRadius) {
        return {};
    }
    return {row, col};
}

QPointF BoardWidget::positionOf(int row, int col, const Geometry &geometry) const
{
    return {
        geometry.boardRect.left() + col * geometry.spacing,
        geometry.boardRect.top() + row * geometry.spacing
    };
}

void BoardWidget::drawBoard(QPainter &painter, const Geometry &geometry)
{
    const QRectF woodRect = geometry.boardRect.adjusted(-geometry.spacing * 0.85,
                                                        -geometry.spacing * 0.85,
                                                        geometry.spacing * 0.85,
                                                        geometry.spacing * 0.85);
    QLinearGradient wood(woodRect.topLeft(), woodRect.bottomRight());
    wood.setColorAt(0.0, QColor(240, 199, 116));
    wood.setColorAt(0.48, QColor(218, 164, 77));
    wood.setColorAt(1.0, QColor(198, 139, 60));

    painter.setPen(QPen(QColor(118, 77, 31, 90), 1.0));
    painter.setBrush(wood);
    painter.drawRoundedRect(woodRect, 16, 16);

    painter.save();
    painter.setClipPath([&woodRect]() {
        QPainterPath path;
        path.addRoundedRect(woodRect, 16, 16);
        return path;
    }());
    painter.setPen(QPen(QColor(121, 77, 28, 22), 1));
    for (int y = static_cast<int>(woodRect.top()); y < woodRect.bottom(); y += 9) {
        painter.drawLine(QPointF(woodRect.left(), y),
                         QPointF(woodRect.right(), y + std::sin(y * 0.08) * 5.0));
    }
    painter.restore();

    painter.setPen(QPen(QColor(74, 48, 21, 205), 1.25));
    for (int index = 0; index < kBoardSize; ++index) {
        const qreal offset = index * geometry.spacing;
        painter.drawLine(QPointF(geometry.boardRect.left() + offset, geometry.boardRect.top()),
                         QPointF(geometry.boardRect.left() + offset, geometry.boardRect.bottom()));
        painter.drawLine(QPointF(geometry.boardRect.left(), geometry.boardRect.top() + offset),
                         QPointF(geometry.boardRect.right(), geometry.boardRect.top() + offset));
    }

    constexpr int starPoints[5][2] = {
        {3, 3}, {3, 11}, {7, 7}, {11, 3}, {11, 11}
    };
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(64, 40, 18));
    for (const auto &star : starPoints) {
        painter.drawEllipse(positionOf(star[0], star[1], geometry),
                            geometry.spacing * 0.10, geometry.spacing * 0.10);
    }

    if (m_hovered.isValid() && m_model
        && m_model->canPlace(m_hovered.row, m_hovered.col)) {
        const QPointF center = positionOf(m_hovered.row, m_hovered.col, geometry);
        painter.setBrush(QColor(255, 255, 255, 35));
        painter.setPen(QPen(QColor(255, 255, 255, 145), 1.5));
        painter.drawEllipse(center, geometry.spacing * 0.39, geometry.spacing * 0.39);
    }
}

void BoardWidget::drawStones(QPainter &painter, const Geometry &geometry)
{
    if (!m_model) {
        return;
    }

    const qreal radius = geometry.spacing * 0.42;
    for (int row = 0; row < kBoardSize; ++row) {
        for (int col = 0; col < kBoardSize; ++col) {
            const Stone stone = m_model->at(row, col);
            if (stone == Stone::Empty) {
                continue;
            }

            const QPointF center = positionOf(row, col, geometry);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(43, 31, 18, 70));
            painter.drawEllipse(center + QPointF(radius * 0.10, radius * 0.14), radius, radius);

            QRadialGradient gradient(center - QPointF(radius * 0.30, radius * 0.34),
                                     radius * 1.45);
            if (stone == Stone::Black) {
                gradient.setColorAt(0.0, QColor(92, 96, 103));
                gradient.setColorAt(0.40, QColor(37, 39, 43));
                gradient.setColorAt(1.0, QColor(7, 8, 10));
                painter.setPen(QPen(QColor(0, 0, 0, 120), 0.8));
            } else {
                gradient.setColorAt(0.0, QColor(255, 255, 255));
                gradient.setColorAt(0.62, QColor(237, 236, 230));
                gradient.setColorAt(1.0, QColor(177, 174, 165));
                painter.setPen(QPen(QColor(110, 104, 94, 150), 0.8));
            }
            painter.setBrush(gradient);
            painter.drawEllipse(center, radius, radius);
        }
    }

    if (!m_model->history().empty()) {
        const Move &last = m_model->history().back();
        const QPointF center = positionOf(last.row, last.col, geometry);
        painter.setPen(QPen(last.stone == Stone::Black ? QColor(255, 207, 79)
                                                        : QColor(202, 61, 45),
                            2.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, radius * 0.27, radius * 0.27);
    }

    if (!m_model->winningLine().empty()) {
        const BoardPoint &first = m_model->winningLine().front();
        const BoardPoint &last = m_model->winningLine().back();
        painter.setPen(QPen(QColor(226, 59, 45, 220),
                            std::max<qreal>(3.0, geometry.spacing * 0.09),
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(positionOf(first.row, first.col, geometry),
                         positionOf(last.row, last.col, geometry));
    }
}

void BoardWidget::drawOverlay(QPainter &painter)
{
    painter.fillRect(rect(), QColor(24, 29, 36, 82));

    const QRectF card(width() / 2.0 - 145, height() / 2.0 - 58, 290, 116);
    painter.setPen(QPen(QColor(255, 255, 255, 65), 1));
    painter.setBrush(QColor(28, 32, 39, 225));
    painter.drawRoundedRect(card, 18, 18);

    QFont titleFont = painter.font();
    titleFont.setFamilies({QStringLiteral("Microsoft YaHei UI"), QStringLiteral("sans-serif")});
    titleFont.setPixelSize(22);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(248, 247, 243));
    painter.drawText(card.adjusted(0, 18, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                     QStringLiteral("准备好了吗？"));

    QFont detailFont = titleFont;
    detailFont.setPixelSize(14);
    detailFont.setBold(false);
    painter.setFont(detailFont);
    painter.setPen(QColor(196, 201, 209));
    painter.drawText(card.adjusted(0, 59, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                     QStringLiteral("点击右侧「开始新局」落下第一子"));
}
