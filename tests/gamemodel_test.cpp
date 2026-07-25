/*
 * 游戏规则与内置 AI 测试
 * 测试范围：合法落子、胜负判断、悔棋，以及内置 AI 的取胜与封堵行为。
 */
#include "builtinai.h"
#include "gamemodel.h"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void placeSequence(GameModel &game, const std::initializer_list<BoardPoint> &points)
{
    for (const BoardPoint &point : points) {
        expect(game.placeStone(point.row, point.col), "sequence move should be legal");
    }
}

void testInitialStateAndUndo()
{
    GameModel game;
    expect(game.currentPlayer() == Stone::Black, "black should move first");
    expect(game.moveCount() == 0, "new game should be empty");
    expect(game.placeStone(7, 7), "center move should be accepted");
    expect(game.currentPlayer() == Stone::White, "turn should switch after a move");
    expect(!game.placeStone(7, 7), "occupied point must be rejected");
    expect(game.undoOne(), "undo should succeed");
    expect(game.currentPlayer() == Stone::Black, "undo should restore previous player");
    expect(game.at(7, 7) == Stone::Empty, "undo should clear the point");
}

void testHorizontalWin()
{
    GameModel game;
    placeSequence(game, {
        {7, 3}, {0, 0},
        {7, 4}, {0, 2},
        {7, 5}, {0, 4},
        {7, 6}, {0, 6},
        {7, 7}
    });
    expect(game.winner() == Stone::Black, "five horizontal stones should win");
    expect(game.winningLine().size() == 5, "winning line should be recorded");
    expect(!game.placeStone(8, 8), "no moves should be accepted after winning");
}

void testDiagonalWin()
{
    GameModel game;
    placeSequence(game, {
        {2, 2}, {0, 1},
        {3, 3}, {0, 3},
        {4, 4}, {0, 5},
        {5, 5}, {0, 7},
        {6, 6}
    });
    expect(game.winner() == Stone::Black, "five diagonal stones should win");
}

void testAiWinsAndBlocks()
{
    GameSnapshot winning{};
    for (auto &row : winning.board) {
        row.fill(Stone::Empty);
    }
    winning.currentPlayer = Stone::White;
    for (int col = 4; col <= 7; ++col) {
        winning.board[7][col] = Stone::White;
    }
    BoardPoint move = BuiltinAi::chooseMove(winning);
    expect(move.row == 7 && (move.col == 3 || move.col == 8),
           "AI should complete its five-in-a-row");

    GameSnapshot blocking{};
    for (auto &row : blocking.board) {
        row.fill(Stone::Empty);
    }
    blocking.currentPlayer = Stone::White;
    for (int row = 5; row <= 8; ++row) {
        blocking.board[row][6] = Stone::Black;
    }
    move = BuiltinAi::chooseMove(blocking);
    expect((move.row == 4 || move.row == 9) && move.col == 6,
           "AI should block opponent's immediate win");
}

void testAiSelfPlayCompletesLegally()
{
    GameModel game;
    int safetyCounter = 0;
    while (game.winner() == Stone::Empty && !game.isDraw()
           && safetyCounter < kBoardSize * kBoardSize) {
        const BoardPoint move = BuiltinAi::chooseMove(game.snapshot());
        expect(move.isValid(), "self-play AI should return a board point");
        expect(game.canPlace(move.row, move.col),
               "self-play AI should return an empty point");
        expect(game.placeStone(move.row, move.col),
               "self-play AI move should be accepted");
        ++safetyCounter;
    }
    expect(game.winner() != Stone::Empty || game.isDraw(),
           "AI self-play should reach a terminal result");
}

} // namespace

int main()
{
    testInitialStateAndUndo();
    testHorizontalWin();
    testDiagonalWin();
    testAiWinsAndBlocks();
    testAiSelfPlayCompletesLegally();
    std::cout << "All Gomoku tests passed.\n";
    return EXIT_SUCCESS;
}
