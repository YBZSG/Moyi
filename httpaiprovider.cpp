/*
 * 外部 AI 网络通信实现
 * 文件职责：将棋盘状态编码为 JSON，通过 HTTP 调用外部 AI，
 *          解析落点结果，并处理超时、网络错误和非法响应。
 * 主要组件：QNetworkAccessManager、QNetworkRequest、QNetworkReply、
 *          QJsonDocument、QJsonObject、QUrl、QTimer。
 */
#include "httpaiprovider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>

HttpAiProvider::HttpAiProvider(QObject *parent)
    : AiProvider(parent)
    , m_endpoint(QStringLiteral("http://127.0.0.1:8000/v1/move"))
{
}

QString HttpAiProvider::displayName() const
{
    return QStringLiteral("外部 HTTP AI");
}

void HttpAiProvider::requestMove(const GameSnapshot &snapshot)
{
    cancel();

    if (!m_endpoint.isValid()
        || (m_endpoint.scheme() != QStringLiteral("http")
            && m_endpoint.scheme() != QStringLiteral("https"))) {
        emit failed(QStringLiteral("外部 AI 地址无效，请使用 http:// 或 https:// 地址"));
        return;
    }

    QNetworkRequest request(m_endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Gomoku-Protocol", "gomoku-ai/v1");
    if (!m_bearerToken.trimmed().isEmpty()) {
        request.setRawHeader("Authorization",
                             QByteArrayLiteral("Bearer ") + m_bearerToken.trimmed().toUtf8());
    }

    QNetworkReply *reply = m_manager.post(request, buildPayload(snapshot));
    m_reply = reply;

    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setInterval(m_timeoutMs);
    connect(timer, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("gomokuTimedOut", true);
        reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleFinished(reply);
    });
    timer->start();
}

void HttpAiProvider::cancel()
{
    if (m_reply) {
        m_reply->setProperty("gomokuCancelled", true);
        m_reply->abort();
    }
}

void HttpAiProvider::setEndpoint(const QUrl &endpoint)
{
    m_endpoint = endpoint;
}

void HttpAiProvider::setBearerToken(const QString &token)
{
    m_bearerToken = token;
}

void HttpAiProvider::setTimeoutMs(int timeoutMs)
{
    m_timeoutMs = qBound(1000, timeoutMs, 300000);
}

QByteArray HttpAiProvider::buildPayload(const GameSnapshot &snapshot) const
{
    QJsonArray boardArray;
    for (const auto &row : snapshot.board) {
        QJsonArray rowArray;
        for (Stone stone : row) {
            rowArray.append(static_cast<int>(stone));
        }
        boardArray.append(rowArray);
    }

    QJsonArray historyArray;
    for (const Move &move : snapshot.history) {
        historyArray.append(QJsonObject{
            {QStringLiteral("row"), move.row},
            {QStringLiteral("col"), move.col},
            {QStringLiteral("player"), static_cast<int>(move.stone)}
        });
    }

    const QJsonObject root{
        {QStringLiteral("protocol"), QStringLiteral("gomoku-ai/v1")},
        {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("boardSize"), kBoardSize},
        {QStringLiteral("board"), boardArray},
        {QStringLiteral("currentPlayer"), static_cast<int>(snapshot.currentPlayer)},
        {QStringLiteral("currentPlayerName"),
         snapshot.currentPlayer == Stone::Black ? QStringLiteral("black")
                                                : QStringLiteral("white")},
        {QStringLiteral("history"), historyArray},
        {QStringLiteral("rules"), QJsonObject{
             {QStringLiteral("winLength"), kWinLength},
             {QStringLiteral("overlineWins"), true}
         }}
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void HttpAiProvider::handleFinished(QNetworkReply *reply)
{
    if (m_reply == reply) {
        m_reply.clear();
    }

    const bool cancelled = reply->property("gomokuCancelled").toBool();
    const bool timedOut = reply->property("gomokuTimedOut").toBool();
    const QByteArray responseData = reply->readAll();
    const int statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    reply->deleteLater();

    QString responseError;
    const QJsonDocument errorDocument = QJsonDocument::fromJson(responseData);
    if (errorDocument.isObject()) {
        const QJsonValue errorValue =
            errorDocument.object().value(QStringLiteral("error"));
        if (errorValue.isString()) {
            responseError = errorValue.toString();
        } else if (errorValue.isObject()) {
            responseError =
                errorValue.toObject().value(QStringLiteral("message")).toString();
        }
    }
    responseError = responseError.trimmed().left(240);

    if (cancelled) {
        return;
    }
    if (timedOut) {
        emit failed(QStringLiteral("外部 AI 请求超时"));
        return;
    }
    if (statusCode > 0 && (statusCode < 200 || statusCode >= 300)) {
        const QString detail = responseError.isEmpty()
                                   ? QString()
                                   : QStringLiteral("：%1").arg(responseError);
        emit failed(QStringLiteral("外部 AI 返回 HTTP %1%2")
                        .arg(statusCode)
                        .arg(detail));
        return;
    }
    if (networkError != QNetworkReply::NoError) {
        emit failed(QStringLiteral("外部 AI 连接失败：%1").arg(networkErrorText));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(responseData, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit failed(QStringLiteral("外部 AI 返回的不是有效 JSON"));
        return;
    }

    QJsonObject moveObject = document.object();
    if (moveObject.value(QStringLiteral("move")).isObject()) {
        moveObject = moveObject.value(QStringLiteral("move")).toObject();
    }
    if (!moveObject.value(QStringLiteral("row")).isDouble()
        || !moveObject.value(QStringLiteral("col")).isDouble()) {
        emit failed(QStringLiteral("外部 AI 响应缺少整数 row/col"));
        return;
    }

    const int row = moveObject.value(QStringLiteral("row")).toInt(-1);
    const int col = moveObject.value(QStringLiteral("col")).toInt(-1);
    const QString detail =
        document.object().value(QStringLiteral("message")).toString(QStringLiteral("外部 HTTP AI"));
    emit moveReady(row, col, detail);
}
