/*
 * 【组员三负责】棋局公共数据类型
 * 文件职责：定义棋盘尺寸、棋子类型、落子记录、游戏模式及 AI 请求数据，
 *          供棋盘、规则、界面和 AI 模块共同使用。
 */
#ifndef GAMETYPES_H
#define GAMETYPES_H

#include <array>
#include <vector>

constexpr int kBoardSize = 15;
constexpr int kWinLength = 5;

enum class Stone : int
{
    Empty = 0,
    Black = 1,
    White = 2
};

enum class GameMode
{
    LocalTwoPlayer,
    HumanVsAi,
    AiVsAi
};

struct BoardPoint
{
    int row = -1;
    int col = -1;

    bool isValid() const
    {
        return row >= 0 && row < kBoardSize && col >= 0 && col < kBoardSize;
    }
};

struct Move
{
    int row = -1;
    int col = -1;
    Stone stone = Stone::Empty;
};

using Board = std::array<std::array<Stone, kBoardSize>, kBoardSize>;

struct GameSnapshot
{
    Board board{};
    Stone currentPlayer = Stone::White;
    std::vector<Move> history;
};

inline Stone oppositeStone(Stone stone)
{
    return stone == Stone::Black ? Stone::White : Stone::Black;
}

#endif // GAMETYPES_H
