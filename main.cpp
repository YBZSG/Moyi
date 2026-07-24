/*
 * 墨弈项目总体设计
 * 负责人：组员一
 * 主要内容：应用程序启动、全局样式、窗口图标及各功能模块的总体组织。
 * 相关 Qt 组件：QApplication、QMainWindow、QIcon、QFont、QStyleFactory。
 */
#include "mainwindow.h"

#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPointSize(10);
    QApplication::setFont(font);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/gomoku-logo.png")));

    MainWindow w;
    w.show();
    return QApplication::exec();
}
