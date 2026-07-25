/*
 * 内置 AI 决策实现
 * 文件职责：遍历合法落点，评估进攻与防守棋形，优先完成五连或封堵对手，
 *          并通过统一 AI 接口返回最终落子坐标。
 * 主要组件：QObject、QTimer、Qt 信号与槽。
 */
#include "builtinai.h"

#include <QTimer>

#include <algorithm>
#include <limits>

BuiltinAi::BuiltinAi(QObject *parent)
    : AiProvider(parent)
{
}

QString BuiltinAi::displayName() const
{
    return QStringLiteral("内置策略 AI");
}

void BuiltinAi::requestMove(const GameSnapshot &snapshot)
{
    const quint64 requestGeneration = ++m_requestGeneration;
    QTimer::singleShot(260, this, [this, snapshot, requestGeneration]() {
        if (requestGeneration != m_requestGeneration) {
            return;
        }
        const BoardPoint move = chooseMove(snapshot);
        if (move.isValid()) {
            emit moveReady(move.row, move.col, QStringLiteral("内置策略 AI"));
        } else {
            emit failed(QStringLiteral("棋盘上没有可用位置"));
        }
    });
}

void BuiltinAi::cancel()
{
    ++m_requestGeneration;
}

BoardPoint BuiltinAi::chooseMove(const GameSnapshot &snapshot)
{
    const Stone aiStone = snapshot.currentPlayer;
    const Stone opponent = oppositeStone(aiStone);

    bool boardEmpty = true;
    for (const auto &row : snapshot.board) {
        for (Stone stone : row) {
            if (stone != Stone::Empty) {
                boardEmpty = false;
                break;
            }
        }
        if (!boardEmpty) {
            break;
        }
    }
    if (boardEmpty) {
        return {kBoardSize / 2, kBoardSize / 2};
    }

    // 第一优先级：直接取胜。
    for (int row = 0; row < kBoardSize; ++row) {
        for (int col = 0; col < kBoardSize; ++col) {
            if (snapshot.board[row][col] == Stone::Empty
                && wouldWin(snapshot.board, row, col, aiStone)) {
                return {row, col};
            }
        }
    }

    // 第二优先级：封堵对手下一步的胜点。
    BoardPoint bestBlock;
    int bestBlockScore = std::numeric_limits<int>::min();
    for (int row = 0; row < kBoardSize; ++row) {
        for (int col = 0; col < kBoardSize; ++col) {
            if (snapshot.board[row][col] == Stone::Empty
                && wouldWin(snapshot.board, row, col, opponent)) {
                const int score = evaluate(snapshot.board, row, col, aiStone);
                if (score > bestBlockScore) {
                    bestBlockScore = score;
                    bestBlock = {row, col};
                }
            }
        }
    }
    if (bestBlock.isValid()) {
        return bestBlock;
    }

    BoardPoint bestMove;
    int bestScore = std::numeric_limits<int>::min();
    for (int row = 0; row < kBoardSize; ++row) {
        for (int col = 0; col < kBoardSize; ++col) {
            if (snapshot.board[row][col] != Stone::Empty
                || !hasNearbyStone(snapshot.board, row, col)) {
                continue;
            }

            const int score = evaluate(snapshot.board, row, col, aiStone);
            if (score > bestScore) {
                bestScore = score;
                bestMove = {row, col};
            }
        }
    }

    if (bestMove.isValid()) {
        return bestMove;
    }

    for (int row = 0; row < kBoardSize; ++row) {
        for (int col = 0; col < kBoardSize; ++col) {
            if (snapshot.board[row][col] == Stone::Empty) {
                return {row, col};
            }
        }
    }
    return {};
}

bool BuiltinAi::isInside(int row, int col)
{
    return row >= 0 && row < kBoardSize && col >= 0 && col < kBoardSize;
}

bool BuiltinAi::wouldWin(const Board &board, int row, int col, Stone stone)
{
    if (!isInside(row, col) || board[row][col] != Stone::Empty) {
        return false;
    }

    constexpr int directions[4][2] = {
        {0, 1}, {1, 0}, {1, 1}, {1, -1}
    };
    for (const auto &direction : directions) {
        int count = 1;
        for (int sign : {-1, 1}) {
            int nextRow = row + sign * direction[0];
            int nextCol = col + sign * direction[1];
            while (isInside(nextRow, nextCol) && board[nextRow][nextCol] == stone) {
                ++count;
                nextRow += sign * direction[0];
                nextCol += sign * direction[1];
            }
        }
        if (count >= kWinLength) {
            return true;
        }
    }
    return false;
}

int BuiltinAi::lineScore(const Board &board, int row, int col, Stone stone,
                         int deltaRow, int deltaCol)
{
    int count = 1;
    int openEnds = 0;

    for (int sign : {-1, 1}) {
        int nextRow = row + sign * deltaRow;
        int nextCol = col + sign * deltaCol;
        while (isInside(nextRow, nextCol) && board[nextRow][nextCol] == stone) {
            ++count;
            nextRow += sign * deltaRow;
            nextCol += sign * deltaCol;
        }
        if (isInside(nextRow, nextCol) && board[nextRow][nextCol] == Stone::Empty) {
            ++openEnds;
        }
    }

    if (count >= 5) {
        return 10'000'000;
    }
    if (count == 4) {
        return openEnds == 2 ? 800'000 : (openEnds == 1 ? 120'000 : 0);
    }
    if (count == 3) {
        return openEnds == 2 ? 45'000 : (openEnds == 1 ? 5'000 : 0);
    }
    if (count == 2) {
        return openEnds == 2 ? 2'000 : (openEnds == 1 ? 260 : 0);
    }
    return openEnds == 2 ? 70 : 15;
}

int BuiltinAi::evaluate(const Board &board, int row, int col, Stone aiStone)
{
    constexpr int directions[4][2] = {
        {0, 1}, {1, 0}, {1, 1}, {1, -1}
    };

    int attack = 0;
    int defense = 0;
    for (const auto &direction : directions) {
        attack += lineScore(board, row, col, aiStone, direction[0], direction[1]);
        defense += lineScore(board, row, col, oppositeStone(aiStone),
                             direction[0], direction[1]);
    }

    const int center = kBoardSize / 2;
    const int centerBias = 20 - (std::abs(row - center) + std::abs(col - center));
    return attack + defense * 9 / 10 + centerBias;
}

bool BuiltinAi::hasNearbyStone(const Board &board, int row, int col)
{
    for (int deltaRow = -2; deltaRow <= 2; ++deltaRow) {
        for (int deltaCol = -2; deltaCol <= 2; ++deltaCol) {
            const int nearRow = row + deltaRow;
            const int nearCol = col + deltaCol;
            if (isInside(nearRow, nearCol)
                && board[nearRow][nearCol] != Stone::Empty) {
                return true;
            }
        }
    }
    return false;
}
