/*
 * 棋盘显示与鼠标交互模块
 * 主要内容：绘制 15×15 棋盘和棋子、换算鼠标坐标、显示最后落子位置。
 * 相关 Qt 组件：QWidget、QPainter、QMouseEvent、QPaintEvent。
 */
#ifndef BOARDWIDGET_H
#define BOARDWIDGET_H

#include "gamemodel.h"

#include <QWidget>

class BoardWidget : public QWidget
{
    Q_OBJECT

public:
    explicit BoardWidget(QWidget *parent = nullptr);

    void setModel(const GameModel *model);
    void setGameActive(bool active);
    void setInputEnabled(bool enabled);
    QSize sizeHint() const override;
    int heightForWidth(int width) const override;
    bool hasHeightForWidth() const override;

signals:
    void intersectionClicked(int row, int col);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Geometry
    {
        QRectF boardRect;
        qreal spacing = 0.0;
    };

    Geometry boardGeometry() const;
    BoardPoint pointAt(const QPointF &position) const;
    QPointF positionOf(int row, int col, const Geometry &geometry) const;
    void drawBoard(QPainter &painter, const Geometry &geometry);
    void drawStones(QPainter &painter, const Geometry &geometry);
    void drawOverlay(QPainter &painter);

    const GameModel *m_model = nullptr;
    bool m_gameActive = false;
    bool m_inputEnabled = false;
    BoardPoint m_hovered;
};

#endif // BOARDWIDGET_H
