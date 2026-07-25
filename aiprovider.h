/*
 * AI 统一接口
 * 负责人：赖泽豪
 * 主要内容：统一内置 AI 与外部 AI 的调用方式，向主程序返回落子结果和状态信息。
 * 相关 Qt 组件：QObject、信号与槽、QString。
 */
#ifndef AIPROVIDER_H
#define AIPROVIDER_H

#include "gametypes.h"

#include <QObject>
#include <QString>

class AiProvider : public QObject
{
    Q_OBJECT

public:
    explicit AiProvider(QObject *parent = nullptr);
    ~AiProvider() override = default;

    virtual QString displayName() const = 0;
    virtual void requestMove(const GameSnapshot &snapshot) = 0;
    virtual void cancel();

signals:
    void moveReady(int row, int col, const QString &detail);
    void failed(const QString &reason);
};

#endif // AIPROVIDER_H
