/*
 * 【王佳怡负责】游戏规则与棋局数据实现
 * 文件职责：管理棋盘状态、合法落子、回合切换、悔棋、重新开始、
 *          五子连线判断和和棋判断。
 */
#include "gamemodel.h"

#include <algorithm>

GameModel::GameModel()
{
    reset();
}

void GameModel::reset()
{
    for (auto &row : m_board) {
        row.fill(Stone::Empty);
    }
    m_currentPlayer = Stone::Black;
    m_winner = Stone::Empty;
    m_draw = false;
    m_history.clear();
    m_winningLine.clear();
}

bool GameModel::placeStone(int row, int col)
{
    if (!canPlace(row, col) || m_winner != Stone::Empty || m_draw) {
        return false;
    }

    const Stone placedStone = m_currentPlayer;
    m_board[row][col] = placedStone;
    m_history.push_back({row, col, placedStone});
    updateResultFromLastMove();

    if (m_winner == Stone::Empty && !m_draw) {
        m_currentPlayer = oppositeStone(m_currentPlayer);
    }
    return true;
}

bool GameModel::undoOne()
{
    if (m_history.empty()) {
        return false;
    }

    const Move last = m_history.back();
    m_history.pop_back();
    m_board[last.row][last.col] = Stone::Empty;
    m_currentPlayer = last.stone;
    m_winner = Stone::Empty;
    m_draw = false;
    m_winningLine.clear();
    return true;
}

int GameModel::undo(int count)
{
    int undone = 0;
    while (undone < count && undoOne()) {
        ++undone;
    }
    return undone;
}

Stone GameModel::at(int row, int col) const
{
    return isInside(row, col) ? m_board[row][col] : Stone::Empty;
}

Stone GameModel::currentPlayer() const
{
    return m_currentPlayer;
}

Stone GameModel::winner() const
{
    return m_winner;
}

bool GameModel::isDraw() const
{
    return m_draw;
}

bool GameModel::isBoardFull() const
{
    return static_cast<int>(m_history.size()) == kBoardSize * kBoardSize;
}

bool GameModel::canPlace(int row, int col) const
{
    return isInside(row, col) && m_board[row][col] == Stone::Empty;
}

int GameModel::moveCount() const
{
    return static_cast<int>(m_history.size());
}

const Board &GameModel::board() const
{
    return m_board;
}

const std::vector<Move> &GameModel::history() const
{
    return m_history;
}

const std::vector<BoardPoint> &GameModel::winningLine() const
{
    return m_winningLine;
}

GameSnapshot GameModel::snapshot() const
{
    return {m_board, m_currentPlayer, m_history};
}

bool GameModel::isInside(int row, int col) const
{
    return row >= 0 && row < kBoardSize && col >= 0 && col < kBoardSize;
}

void GameModel::updateResultFromLastMove()
{
    m_winner = Stone::Empty;
    m_draw = false;
    m_winningLine.clear();

    if (m_history.empty()) {
        return;
    }

    const Move &last = m_history.back();
    constexpr int directions[4][2] = {
        {0, 1},
        {1, 0},
        {1, 1},
        {1, -1}
    };

    for (const auto &direction : directions) {
        std::vector<BoardPoint> line;

        int row = last.row;
        int col = last.col;
        while (isInside(row - direction[0], col - direction[1])
               && m_board[row - direction[0]][col - direction[1]] == last.stone) {
            row -= direction[0];
            col -= direction[1];
        }

        while (isInside(row, col) && m_board[row][col] == last.stone) {
            line.push_back({row, col});
            row += direction[0];
            col += direction[1];
        }

        if (static_cast<int>(line.size()) >= kWinLength) {
            m_winner = last.stone;
            m_winningLine = std::move(line);
            return;
        }
    }

    m_draw = isBoardFull();
}
