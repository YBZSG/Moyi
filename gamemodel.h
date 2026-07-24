/*
 * 游戏规则与数据模型
 * 负责人：组员三
 * 主要内容：保存棋盘状态、合法落子、回合切换、悔棋、胜负及和棋判断。
 * 相关 Qt/C++ 内容：独立数据模型、QVector/标准容器，以及与界面的状态同步。
 */
#ifndef GAMEMODEL_H
#define GAMEMODEL_H

#include "gametypes.h"

class GameModel
{
public:
    GameModel();

    void reset();
    bool placeStone(int row, int col);
    bool undoOne();
    int undo(int count);

    Stone at(int row, int col) const;
    Stone currentPlayer() const;
    Stone winner() const;
    bool isDraw() const;
    bool isBoardFull() const;
    bool canPlace(int row, int col) const;
    int moveCount() const;

    const Board &board() const;
    const std::vector<Move> &history() const;
    const std::vector<BoardPoint> &winningLine() const;
    GameSnapshot snapshot() const;

private:
    bool isInside(int row, int col) const;
    void updateResultFromLastMove();

    Board m_board{};
    Stone m_currentPlayer = Stone::Black;
    Stone m_winner = Stone::Empty;
    bool m_draw = false;
    std::vector<Move> m_history;
    std::vector<BoardPoint> m_winningLine;
};

#endif // GAMEMODEL_H
