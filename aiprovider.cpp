/*
 * 【赖泽豪负责】统一 AI 接口实现
 * 文件职责：为内置 AI 和外部 AI 提供统一的请求、取消和结果通知方式，
 *          降低主界面与具体 AI 实现之间的耦合。
 * 主要组件：QObject、QString、Qt 信号与槽。
 */
#include "aiprovider.h"

AiProvider::AiProvider(QObject *parent)
    : QObject(parent)
{
}

void AiProvider::cancel()
{
}
