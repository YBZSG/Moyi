/*
 * 主界面与交互模块
 * 负责人：黎天宇
 * 主要内容：界面布局、模式选择、AI 配置、按钮操作、状态显示和结果弹窗。
 * 相关 Qt 组件：QMainWindow、QPushButton、QComboBox、QLineEdit、QLabel、
 *               QCheckBox、QGroupBox、QStackedWidget、QTimer 及布局组件。
 */
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "gamemodel.h"

#include <QMainWindow>
#include <QString>

class AiProvider;
class BoardWidget;
class BuiltinAi;
class HttpAiProvider;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override = default;

private slots:
    void startNewGame();
    void undoMove();
    void handleBoardClick(int row, int col);
    void handleAiMove(int row, int col, const QString &detail);
    void handleAiFailure(const QString &reason);
    void handleModeChanged();
    void toggleAiMatchPaused();
    void configureAiMatch();

private:
    void buildUi();
    void applyStyle();
    void requestAiMove();
    void finishTurn(const QString &moveDetail = QString());
    void finishGame();
    bool showGameOverDialog(const QString &result);
    void updateUi();
    void updateAiMatchSummary();
    void setMessage(const QString &message, bool warning = false);
    bool isHumanTurn() const;
    bool isAiTurn() const;
    GameMode selectedMode() const;
    AiProvider *selectedAiProvider() const;

    struct AiSideConfig
    {
        bool external = false;
        QString endpoint = QStringLiteral(
            "http://127.0.0.1:8000/v1/move?provider=search&depth=3");
        QString token;
    };

    GameModel m_game;
    bool m_gameActive = false;
    bool m_aiThinking = false;
    bool m_aiMatchPaused = false;
    Stone m_aiRequestedStone = Stone::Empty;
    QString m_pendingAiWarning;

    BoardWidget *m_boardWidget = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QComboBox *m_aiCombo = nullptr;
    QGroupBox *m_aiMatchGroup = nullptr;
    QGroupBox *m_apiGroup = nullptr;
    QLineEdit *m_endpointEdit = nullptr;
    QLineEdit *m_tokenEdit = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_undoButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_aiMatchConfigButton = nullptr;
    QLabel *m_turnStone = nullptr;
    QLabel *m_turnLabel = nullptr;
    QLabel *m_moveCountLabel = nullptr;
    QLabel *m_messageLabel = nullptr;
    QLabel *m_aiHintLabel = nullptr;
    QLabel *m_blackAiSummary = nullptr;
    QLabel *m_whiteAiSummary = nullptr;

    AiSideConfig m_blackAiConfig;
    AiSideConfig m_whiteAiConfig;

    BuiltinAi *m_builtinAi = nullptr;
    HttpAiProvider *m_httpAi = nullptr;
};

#endif // MAINWINDOW_H
