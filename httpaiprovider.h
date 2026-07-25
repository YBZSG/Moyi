/*
 * 外部 AI 网络接口模块
 * 负责人：赖泽豪
 * 主要内容：发送棋盘数据、接收外部 AI 落点，并处理超时、错误及临时回退。
 * 相关 Qt 组件：QNetworkAccessManager、QNetworkRequest、QNetworkReply、QUrl、QTimer。
 */
#ifndef HTTPAIPROVIDER_H
#define HTTPAIPROVIDER_H

#include "aiprovider.h"

#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>

class QNetworkReply;

class HttpAiProvider : public AiProvider
{
    Q_OBJECT

public:
    explicit HttpAiProvider(QObject *parent = nullptr);

    QString displayName() const override;
    void requestMove(const GameSnapshot &snapshot) override;
    void cancel() override;

    void setEndpoint(const QUrl &endpoint);
    void setBearerToken(const QString &token);
    void setTimeoutMs(int timeoutMs);

private:
    QByteArray buildPayload(const GameSnapshot &snapshot) const;
    void handleFinished(QNetworkReply *reply);

    QNetworkAccessManager m_manager;
    QPointer<QNetworkReply> m_reply;
    QUrl m_endpoint;
    QString m_bearerToken;
    int m_timeoutMs = 60000;
};

#endif // HTTPAIPROVIDER_H
