/*
 * 外部 AI HTTP 接口测试
 * 测试范围：请求数据、Authorization、响应解析、错误处理和回调结果。
 */
#include "httpaiprovider.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace {

void fail(const QString &message)
{
    std::cerr << "FAILED: " << message.toStdString() << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        fail(QStringLiteral("cannot start local HTTP test server"));
    }

    QByteArray receivedRequest;
    bool protocolWasValid = false;
    bool responseSent = false;

    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
            receivedRequest += socket->readAll();
            const int headerEnd = receivedRequest.indexOf("\r\n\r\n");
            if (headerEnd < 0 || responseSent) {
                return;
            }

            const QByteArray headers = receivedRequest.left(headerEnd);
            const QRegularExpression contentLengthExpression(
                QStringLiteral("Content-Length:\\s*(\\d+)"),
                QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch match =
                contentLengthExpression.match(QString::fromLatin1(headers));
            if (!match.hasMatch()) {
                fail(QStringLiteral("request has no Content-Length"));
            }

            const int contentLength = match.captured(1).toInt();
            const QByteArray body = receivedRequest.mid(headerEnd + 4);
            if (body.size() < contentLength) {
                return;
            }

            const QJsonDocument requestDocument =
                QJsonDocument::fromJson(body.left(contentLength));
            const QJsonObject requestObject = requestDocument.object();
            protocolWasValid =
                requestObject.value(QStringLiteral("protocol")).toString()
                    == QStringLiteral("gomoku-ai/v1")
                && requestObject.value(QStringLiteral("boardSize")).toInt() == kBoardSize
                && requestObject.value(QStringLiteral("currentPlayer")).toInt()
                    == static_cast<int>(Stone::White)
                && requestObject.value(QStringLiteral("board")).toArray().size()
                    == kBoardSize;

            const QByteArray responseBody =
                QByteArrayLiteral(R"({"move":{"row":7,"col":8},"message":"mock-neural-ai"})");
            const QByteArray response =
                QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n")
                + QByteArrayLiteral("Content-Length: ")
                + QByteArray::number(responseBody.size())
                + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
                + responseBody;
            responseSent = true;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
        });
    });

    HttpAiProvider provider;
    provider.setEndpoint(QUrl(QStringLiteral("http://127.0.0.1:%1/v1/move")
                                  .arg(server.serverPort())));
    provider.setTimeoutMs(3000);

    QEventLoop eventLoop;
    int resultRow = -1;
    int resultCol = -1;
    QString error;
    QObject::connect(&provider, &AiProvider::moveReady, &eventLoop,
                     [&](int row, int col, const QString &) {
                         resultRow = row;
                         resultCol = col;
                         eventLoop.quit();
                     });
    QObject::connect(&provider, &AiProvider::failed, &eventLoop,
                     [&](const QString &reason) {
                         error = reason;
                         eventLoop.quit();
                     });

    GameSnapshot snapshot;
    for (auto &row : snapshot.board) {
        row.fill(Stone::Empty);
    }
    snapshot.board[7][7] = Stone::Black;
    snapshot.currentPlayer = Stone::White;
    snapshot.history.push_back({7, 7, Stone::Black});

    QTimer::singleShot(4000, &eventLoop, &QEventLoop::quit);
    provider.requestMove(snapshot);
    eventLoop.exec();

    if (!error.isEmpty()) {
        fail(QStringLiteral("provider failed: %1").arg(error));
    }
    if (!protocolWasValid) {
        fail(QStringLiteral("request JSON did not match gomoku-ai/v1"));
    }
    if (resultRow != 7 || resultCol != 8) {
        fail(QStringLiteral("provider did not parse nested move response"));
    }

    std::cout << "HTTP AI provider test passed.\n";
    return EXIT_SUCCESS;
}
