/*
 * 墨弈项目总体设计
 * 负责人：组员一
 * 主要内容：应用程序启动、全局样式、窗口图标及各功能模块的总体组织。
 * 相关 Qt 组件：QApplication、QMainWindow、QIcon、QFont、QStyleFactory。
 */
#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QProcess>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTcpSocket>
#include <QThread>

namespace {

bool adapterIsRunning()
{
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), 8000);
    return socket.waitForConnected(120);
}

// 【组员一负责】随程序自动启动本地外部 AI 适配服务。
// 已有服务时不会重复启动；发布版和开发版均会查找 EXE 旁的脚本。
void ensureAiAdapterRunning()
{
    if (adapterIsRunning()) {
        return;
    }

    const QString applicationDir = QCoreApplication::applicationDirPath();
    const QStringList scriptCandidates = {
        QDir(applicationDir).filePath(QStringLiteral("ai_server.py")),
        QDir(applicationDir).filePath(QStringLiteral("examples/ai_server.py")),
        QDir(applicationDir).filePath(QStringLiteral(
            "../../../examples/ai_server.py"))
    };

    QString scriptPath;
    for (const QString &candidate : scriptCandidates) {
        const QFileInfo script(candidate);
        if (script.exists() && script.isFile()) {
            scriptPath = script.absoluteFilePath();
            break;
        }
    }
    if (scriptPath.isEmpty()) {
        return;
    }

    QString python = QStandardPaths::findExecutable(QStringLiteral("pythonw"));
    if (python.isEmpty()) {
        python = QStandardPaths::findExecutable(QStringLiteral("python"));
    }
    bool usesPythonLauncher = false;
    if (python.isEmpty()) {
        python = QStandardPaths::findExecutable(QStringLiteral("py"));
        usesPythonLauncher = !python.isEmpty();
    }
    if (python.isEmpty()) {
        return;
    }

    QStringList arguments;
    if (usesPythonLauncher) {
        arguments << QStringLiteral("-3");
    }
    arguments << scriptPath;
    if (!QProcess::startDetached(python, arguments, QFileInfo(scriptPath).path())) {
        return;
    }

    // 给 Python 一小段启动时间，避免打开游戏后第一步立即连接失败。
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1500 && !adapterIsRunning()) {
        QThread::msleep(50);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPointSize(10);
    QApplication::setFont(font);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/gomoku-logo.png")));

    ensureAiAdapterRunning();

    MainWindow w;
    w.show();
    return QApplication::exec();
}
