/*
 * 内置 AI 模块
 * 负责人：李东骏
 * 主要内容：分析合法空位、评估攻防棋形、优先取胜和封堵，并输出落子坐标。
 * 相关 Qt 组件：QObject、信号与槽、QTimer（由界面调度 AI 回合）。
 */
#ifndef BUILTINAI_H
#define BUILTINAI_H

#include "aiprovider.h"

class BuiltinAi : public AiProvider
{
    Q_OBJECT

public:
    explicit BuiltinAi(QObject *parent = nullptr);

    QString displayName() const override;
    void requestMove(const GameSnapshot &snapshot) override;
    void cancel() override;

    static BoardPoint chooseMove(const GameSnapshot &snapshot);

private:
    static bool isInside(int row, int col);
    static bool wouldWin(const Board &board, int row, int col, Stone stone);
    static int lineScore(const Board &board, int row, int col, Stone stone,
                         int deltaRow, int deltaCol);
    static int evaluate(const Board &board, int row, int col, Stone aiStone);
    static bool hasNearbyStone(const Board &board, int row, int col);

    quint64 m_requestGeneration = 0;
};

#endif // BUILTINAI_H
