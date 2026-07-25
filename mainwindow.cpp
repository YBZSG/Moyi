/*
 * 主界面设计与交互实现
 * 文件职责：创建主窗口布局，处理模式切换、按钮操作、AI 参数配置、
 *          状态提示、双机对战调度以及游戏结束弹窗。
 * 主要组件：QMainWindow、QPushButton、QComboBox、QLineEdit、QLabel、
 *          QGroupBox、QDialog、QTimer 和各类布局组件。
 */
#include "mainwindow.h"

#include "aiprovider.h"
#include "boardwidget.h"
#include "builtinai.h"
#include "httpaiprovider.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QFormLayout>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

class DarkComboBox final : public QComboBox
{
public:
    explicit DarkComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
        setMaxVisibleItems(20);
    }

protected:
    void showPopup() override
    {
        QComboBox::showPopup();
        QTimer::singleShot(0, this, [this]() {
            QWidget *popup = view() ? view()->window() : nullptr;
            if (!popup) {
                return;
            }

            popup->setAttribute(Qt::WA_StyledBackground, true);
            popup->setContentsMargins(0, 0, 0, 0);
            popup->setStyleSheet(QStringLiteral(
                "QWidget { background:#222831; color:#e7e9ed; border:none; }"
                "QAbstractItemView { background:#222831; color:#e7e9ed;"
                " border:1px solid #3a424f; outline:0; margin:0; padding:0; }"
                "QAbstractItemView::item:selected {"
                " background:#a47730; color:#ffffff; }"));

            QPalette palette = popup->palette();
            palette.setColor(QPalette::Window, QColor(QStringLiteral("#222831")));
            palette.setColor(QPalette::Base, QColor(QStringLiteral("#222831")));
            popup->setPalette(palette);

            if (popup->layout()) {
                popup->layout()->setContentsMargins(0, 0, 0, 0);
                popup->layout()->setSpacing(0);
            }

            for (QWidget *child : popup->findChildren<QWidget *>()) {
                const QString className =
                    QString::fromLatin1(child->metaObject()->className());
                if (className.contains(QStringLiteral("Scroller"),
                                       Qt::CaseInsensitive)) {
                    child->hide();
                    child->setFixedHeight(0);
                }
            }

            if (popup->layout()) {
                popup->layout()->invalidate();
                popup->layout()->activate();
            }
        });
    }
};

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_builtinAi(new BuiltinAi(this))
    , m_httpAi(new HttpAiProvider(this))
{
    buildUi();
    applyStyle();

    connect(m_boardWidget, &BoardWidget::intersectionClicked,
            this, &MainWindow::handleBoardClick);
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startNewGame);
    connect(m_undoButton, &QPushButton::clicked, this, &MainWindow::undoMove);
    connect(m_pauseButton, &QPushButton::clicked,
            this, &MainWindow::toggleAiMatchPaused);
    connect(m_aiMatchConfigButton, &QPushButton::clicked,
            this, &MainWindow::configureAiMatch);
    connect(m_modeCombo, &QComboBox::currentIndexChanged,
            this, &MainWindow::handleModeChanged);
    connect(m_aiCombo, &QComboBox::currentIndexChanged, this, [this]() {
        const bool externalAi = m_aiCombo->currentIndex() == 1
                                && selectedMode() == GameMode::HumanVsAi;
        m_apiGroup->setVisible(externalAi);
        m_aiHintLabel->setVisible(!externalAi
                                  && selectedMode() == GameMode::HumanVsAi);
        updateUi();
    });

    connect(m_builtinAi, &AiProvider::moveReady, this, &MainWindow::handleAiMove);
    connect(m_builtinAi, &AiProvider::failed, this, &MainWindow::handleAiFailure);
    connect(m_httpAi, &AiProvider::moveReady, this, &MainWindow::handleAiMove);
    connect(m_httpAi, &AiProvider::failed, this, &MainWindow::handleAiFailure);

    auto *restartShortcut = new QShortcut(QKeySequence(Qt::Key_R), this);
    auto *undoShortcut = new QShortcut(QKeySequence(Qt::Key_Z), this);
    connect(restartShortcut, &QShortcut::activated, this, &MainWindow::startNewGame);
    connect(undoShortcut, &QShortcut::activated, this, &MainWindow::undoMove);

    m_boardWidget->setModel(&m_game);
    updateAiMatchSummary();
    handleModeChanged();
    updateUi();
}

void MainWindow::startNewGame()
{
    m_builtinAi->cancel();
    m_httpAi->cancel();
    m_game.reset();
    m_gameActive = true;
    m_aiThinking = false;
    m_aiMatchPaused = false;
    m_aiRequestedStone = Stone::Empty;
    m_pendingAiWarning.clear();
    m_boardWidget->setGameActive(true);
    if (selectedMode() == GameMode::HumanVsAi) {
        setMessage(QStringLiteral("你执黑棋，请在棋盘上落子"));
    } else if (selectedMode() == GameMode::AiVsAi) {
        setMessage(QStringLiteral("双机对战已开始，黑方 AI 先行"));
    } else {
        setMessage(QStringLiteral("黑棋先行，请在棋盘上落子"));
    }
    updateUi();

    if (selectedMode() == GameMode::AiVsAi) {
        QTimer::singleShot(400, this, &MainWindow::requestAiMove);
    }
}

void MainWindow::undoMove()
{
    if (m_game.moveCount() == 0) {
        setMessage(QStringLiteral("当前没有可以撤销的落子"), true);
        return;
    }

    m_builtinAi->cancel();
    m_httpAi->cancel();

    int undoCount = 1;
    if (selectedMode() == GameMode::HumanVsAi && !m_aiThinking
        && m_game.moveCount() >= 2
        && m_game.history().back().stone == Stone::White) {
        undoCount = 2;
    }

    const int actual = m_game.undo(undoCount);
    m_aiThinking = false;
    m_aiRequestedStone = Stone::Empty;
    m_pendingAiWarning.clear();
    m_gameActive = true;
    m_boardWidget->setGameActive(true);
    if (selectedMode() == GameMode::AiVsAi) {
        m_aiMatchPaused = true;
        setMessage(QStringLiteral("已暂停双机对战并撤销上一步"));
    } else {
        setMessage(actual == 2 ? QStringLiteral("已撤销上一回合")
                               : QStringLiteral("已撤销上一步"));
    }
    updateUi();
}

void MainWindow::handleBoardClick(int row, int col)
{
    if (!m_gameActive || m_aiThinking || !isHumanTurn()) {
        return;
    }
    if (!m_game.placeStone(row, col)) {
        setMessage(QStringLiteral("这里已经有棋子了"), true);
        return;
    }
    finishTurn();
}

void MainWindow::handleAiMove(int row, int col, const QString &detail)
{
    if (!m_gameActive || !m_aiThinking || !isAiTurn()
        || m_game.currentPlayer() != m_aiRequestedStone) {
        return;
    }

    if (!m_game.canPlace(row, col)) {
        const QString reason =
            QStringLiteral("AI 返回了无效落点 (%1, %2)").arg(row).arg(col);
        if (sender() == m_httpAi) {
            m_pendingAiWarning = reason;
            setMessage(QStringLiteral("%1，本手改用内置 AI").arg(reason), true);
            m_builtinAi->requestMove(m_game.snapshot());
        } else {
            handleAiFailure(reason);
        }
        return;
    }

    QString completedDetail = detail;
    if (!m_pendingAiWarning.isEmpty()) {
        completedDetail = QStringLiteral("外部 AI 失败（%1），本手由内置 AI 代下")
                              .arg(m_pendingAiWarning);
        m_pendingAiWarning.clear();
    }
    m_aiThinking = false;
    m_aiRequestedStone = Stone::Empty;
    m_game.placeStone(row, col);
    finishTurn(completedDetail);
}

void MainWindow::handleAiFailure(const QString &reason)
{
    if (!m_gameActive || !m_aiThinking) {
        return;
    }

    if (sender() == m_httpAi) {
        m_pendingAiWarning = reason;
        setMessage(QStringLiteral("%1，本手改用内置 AI").arg(reason), true);
        m_builtinAi->requestMove(m_game.snapshot());
        return;
    }

    m_aiThinking = false;
    m_aiRequestedStone = Stone::Empty;
    m_pendingAiWarning.clear();
    setMessage(QStringLiteral("AI 无法落子：%1").arg(reason), true);
    updateUi();
}

void MainWindow::handleModeChanged()
{
    const bool humanAiMode = selectedMode() == GameMode::HumanVsAi;
    const bool selfPlayMode = selectedMode() == GameMode::AiVsAi;
    m_aiCombo->setVisible(humanAiMode);
    m_aiCombo->setEnabled(humanAiMode);
    const bool externalAi = humanAiMode && m_aiCombo->currentIndex() == 1;
    m_aiHintLabel->setVisible(humanAiMode && !externalAi);
    m_apiGroup->setVisible(externalAi);
    m_aiMatchGroup->setVisible(selfPlayMode);
    m_pauseButton->setVisible(selfPlayMode);

    if (m_gameActive || m_game.moveCount() > 0) {
        startNewGame();
    } else {
        updateUi();
    }
}

void MainWindow::toggleAiMatchPaused()
{
    if (selectedMode() != GameMode::AiVsAi || !m_gameActive) {
        return;
    }

    if (m_aiMatchPaused) {
        m_aiMatchPaused = false;
        setMessage(QStringLiteral("继续双机对战"));
        updateUi();
        QTimer::singleShot(250, this, &MainWindow::requestAiMove);
        return;
    }

    m_aiMatchPaused = true;
    m_builtinAi->cancel();
    m_httpAi->cancel();
    m_aiThinking = false;
    m_aiRequestedStone = Stone::Empty;
    m_pendingAiWarning.clear();
    setMessage(QStringLiteral("双机对战已暂停"));
    updateUi();
}

void MainWindow::configureAiMatch()
{
    const bool resumeAfterDialog =
        m_gameActive && selectedMode() == GameMode::AiVsAi && !m_aiMatchPaused;
    if (m_gameActive && selectedMode() == GameMode::AiVsAi) {
        m_aiMatchPaused = true;
        m_builtinAi->cancel();
        m_httpAi->cancel();
        m_aiThinking = false;
        m_aiRequestedStone = Stone::Empty;
        m_pendingAiWarning.clear();
        updateUi();
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("配置黑白 AI"));
    dialog.setModal(true);
    dialog.resize(590, 500);
    dialog.setStyleSheet(QStringLiteral(R"(
        QDialog {
            background: #171b22;
            color: #f4f3ef;
            font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
            font-size: 14px;
        }
        QLabel { color: #c9ced6; }
        QLabel#dialogTitle {
            color: #f5d38b;
            font-size: 22px;
            font-weight: 700;
        }
        QLabel#dialogHint { color: #858e9b; font-size: 12px; }
        QGroupBox {
            color: #e4c078;
            background: #20252d;
            border: 1px solid #39414d;
            border-radius: 10px;
            margin-top: 10px;
            font-weight: 600;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 5px;
        }
        QComboBox, QLineEdit {
            color: #e7e9ed;
            background: #151a20;
            border: 1px solid #3d4653;
            border-radius: 7px;
            padding: 8px 10px;
            font-weight: 400;
        }
        QComboBox QAbstractItemView {
            color: #e7e9ed;
            background: #222831;
            border: 1px solid #3a424f;
            outline: 0;
            margin: 0;
            padding: 0;
            selection-background-color: #8d672f;
        }
        QComboBoxPrivateContainer {
            background: #222831;
            border: 1px solid #3a424f;
            margin: 0;
            padding: 0;
        }
        QComboBoxPrivateScroller {
            background: #222831;
            border: none;
            min-height: 0;
            max-height: 0;
        }
        QPushButton {
            min-height: 38px;
            border-radius: 8px;
            padding: 7px 18px;
            font-weight: 600;
        }
        QPushButton#saveButton {
            color: #1c1710;
            background: #e2b65e;
            border: 1px solid #efca7f;
        }
        QPushButton#cancelButton {
            color: #d8dbe0;
            background: #2b323c;
            border: 1px solid #414a57;
        }
    )"));

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(13);
    auto *title = new QLabel(QStringLiteral("黑白双方独立 AI"), &dialog);
    title->setObjectName(QStringLiteral("dialogTitle"));
    auto *hint = new QLabel(
        QStringLiteral("可分别选择内置或外部 AI；两边可使用不同地址、模型和 Token。"),
        &dialog);
    hint->setObjectName(QStringLiteral("dialogHint"));
    layout->addWidget(title);
    layout->addWidget(hint);

    QComboBox *blackEngine = nullptr;
    QLineEdit *blackEndpoint = nullptr;
    QLineEdit *blackToken = nullptr;
    QComboBox *whiteEngine = nullptr;
    QLineEdit *whiteEndpoint = nullptr;
    QLineEdit *whiteToken = nullptr;

    const auto addSideEditor =
        [&](const QString &titleText, const AiSideConfig &config,
            QComboBox *&engine, QLineEdit *&endpoint, QLineEdit *&token) {
            auto *group = new QGroupBox(titleText, &dialog);
            auto *form = new QFormLayout(group);
            form->setContentsMargins(14, 18, 14, 13);
            form->setHorizontalSpacing(12);
            form->setVerticalSpacing(8);
            engine = new DarkComboBox(group);
            engine->addItem(QStringLiteral("内置策略 AI"));
            engine->addItem(QStringLiteral("外部 HTTP AI"));
            engine->setCurrentIndex(config.external ? 1 : 0);
            endpoint = new QLineEdit(config.endpoint, group);
            endpoint->setPlaceholderText(
                QStringLiteral("http://127.0.0.1:8000/v1/move?provider=..."));
            token = new QLineEdit(config.token, group);
            token->setEchoMode(QLineEdit::Password);
            token->setPlaceholderText(QStringLiteral("Bearer Token（可选）"));
            form->addRow(QStringLiteral("引擎"), engine);
            form->addRow(QStringLiteral("接口"), endpoint);
            form->addRow(QStringLiteral("Token"), token);
            const auto updateExternalFields = [engine, endpoint, token]() {
                const bool external = engine->currentIndex() == 1;
                endpoint->setEnabled(external);
                token->setEnabled(external);
            };
            connect(engine, &QComboBox::currentIndexChanged,
                    group, updateExternalFields);
            updateExternalFields();
            layout->addWidget(group);
        };

    addSideEditor(QStringLiteral("黑方 AI"), m_blackAiConfig,
                  blackEngine, blackEndpoint, blackToken);
    addSideEditor(QStringLiteral("白方 AI"), m_whiteAiConfig,
                  whiteEngine, whiteEndpoint, whiteToken);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *cancelButton = new QPushButton(QStringLiteral("取消"), &dialog);
    cancelButton->setObjectName(QStringLiteral("cancelButton"));
    auto *saveButton = new QPushButton(QStringLiteral("保存配置"), &dialog);
    saveButton->setObjectName(QStringLiteral("saveButton"));
    buttons->addWidget(cancelButton);
    buttons->addWidget(saveButton);
    layout->addLayout(buttons);
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    if (dialog.exec() == QDialog::Accepted) {
        m_blackAiConfig.external = blackEngine->currentIndex() == 1;
        m_blackAiConfig.endpoint = blackEndpoint->text().trimmed();
        m_blackAiConfig.token = blackToken->text();
        m_whiteAiConfig.external = whiteEngine->currentIndex() == 1;
        m_whiteAiConfig.endpoint = whiteEndpoint->text().trimmed();
        m_whiteAiConfig.token = whiteToken->text();
        updateAiMatchSummary();
        setMessage(QStringLiteral("黑白 AI 配置已更新"));
    }

    if (resumeAfterDialog && m_gameActive) {
        m_aiMatchPaused = false;
        updateUi();
        QTimer::singleShot(250, this, &MainWindow::requestAiMove);
    }
}

void MainWindow::updateAiMatchSummary()
{
    const auto description = [](const QString &color,
                                const AiSideConfig &config) {
        return QStringLiteral("%1：%2")
            .arg(color,
                 config.external ? QStringLiteral("外部 HTTP AI")
                                 : QStringLiteral("内置策略 AI"));
    };
    m_blackAiSummary->setText(
        description(QStringLiteral("黑方"), m_blackAiConfig));
    m_whiteAiSummary->setText(
        description(QStringLiteral("白方"), m_whiteAiConfig));
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("墨弈"));
    resize(1120, 760);
    setMinimumSize(980, 680);

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("central"));
    setCentralWidget(central);

    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(28, 20, 28, 28);
    rootLayout->setSpacing(18);

    auto *header = new QHBoxLayout;
    auto *logoLabel = new QLabel(central);
    logoLabel->setFixedSize(58, 58);
    logoLabel->setPixmap(
        QPixmap(QStringLiteral(":/assets/gomoku-logo.png"))
            .scaled(58, 58, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logoLabel->setAlignment(Qt::AlignCenter);
    header->addWidget(logoLabel);
    auto *brandColumn = new QVBoxLayout;
    brandColumn->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("墨弈"), central);
    title->setObjectName(QStringLiteral("title"));
    auto *subtitle = new QLabel(QStringLiteral("GOMOKU  ·  五子连珠，落子无悔"), central);
    subtitle->setObjectName(QStringLiteral("subtitle"));
    brandColumn->addWidget(title);
    brandColumn->addWidget(subtitle);
    header->addLayout(brandColumn);
    header->addStretch();

    auto *rulePill = new QLabel(QStringLiteral("15 × 15  ·  黑棋先行  ·  五子连珠"), central);
    rulePill->setObjectName(QStringLiteral("rulePill"));
    header->addWidget(rulePill);
    rootLayout->addLayout(header);

    auto *contentLayout = new QHBoxLayout;
    contentLayout->setSpacing(22);

    auto *boardFrame = new QFrame(central);
    boardFrame->setObjectName(QStringLiteral("boardFrame"));
    auto *boardLayout = new QVBoxLayout(boardFrame);
    boardLayout->setContentsMargins(10, 10, 10, 10);
    m_boardWidget = new BoardWidget(boardFrame);
    boardLayout->addWidget(m_boardWidget);
    contentLayout->addWidget(boardFrame, 1);

    auto *sidePanel = new QFrame(central);
    sidePanel->setObjectName(QStringLiteral("sidePanel"));
    sidePanel->setFixedWidth(330);
    auto *sideLayout = new QVBoxLayout(sidePanel);
    sideLayout->setContentsMargins(22, 22, 22, 20);
    sideLayout->setSpacing(13);

    auto *statusCaption = new QLabel(QStringLiteral("对局状态"), sidePanel);
    statusCaption->setObjectName(QStringLiteral("sectionTitle"));
    sideLayout->addWidget(statusCaption);

    auto *turnCard = new QFrame(sidePanel);
    turnCard->setObjectName(QStringLiteral("turnCard"));
    auto *turnLayout = new QHBoxLayout(turnCard);
    turnLayout->setContentsMargins(16, 13, 16, 13);
    m_turnStone = new QLabel(turnCard);
    m_turnStone->setObjectName(QStringLiteral("turnStone"));
    m_turnStone->setFixedSize(38, 38);
    m_turnStone->setAlignment(Qt::AlignCenter);
    turnLayout->addWidget(m_turnStone);
    auto *turnTextLayout = new QVBoxLayout;
    auto *turnCaption = new QLabel(QStringLiteral("当前回合"), turnCard);
    turnCaption->setObjectName(QStringLiteral("mutedText"));
    m_turnLabel = new QLabel(QStringLiteral("等待开始"), turnCard);
    m_turnLabel->setObjectName(QStringLiteral("turnLabel"));
    turnTextLayout->addWidget(turnCaption);
    turnTextLayout->addWidget(m_turnLabel);
    turnLayout->addLayout(turnTextLayout);
    turnLayout->addStretch();
    m_moveCountLabel = new QLabel(QStringLiteral("0 手"), turnCard);
    m_moveCountLabel->setObjectName(QStringLiteral("countLabel"));
    turnLayout->addWidget(m_moveCountLabel);
    sideLayout->addWidget(turnCard);

    auto *modeCaption = new QLabel(QStringLiteral("游戏模式"), sidePanel);
    modeCaption->setObjectName(QStringLiteral("sectionTitle"));
    sideLayout->addWidget(modeCaption);

    m_modeCombo = new DarkComboBox(sidePanel);
    m_modeCombo->addItem(QStringLiteral("双人对战 · 本地轮流落子"));
    m_modeCombo->addItem(QStringLiteral("人机对战 · 你执黑棋"));
    m_modeCombo->addItem(QStringLiteral("双机对战 · AI 自动对弈"));
    m_modeCombo->setCurrentIndex(1);
    sideLayout->addWidget(m_modeCombo);

    m_aiCombo = new DarkComboBox(sidePanel);
    m_aiCombo->addItem(QStringLiteral("内置策略 AI"));
    m_aiCombo->addItem(QStringLiteral("外部 HTTP AI"));
    sideLayout->addWidget(m_aiCombo);

    m_aiMatchGroup = new QGroupBox(QStringLiteral("黑白 AI 配置"), sidePanel);
    auto *aiMatchLayout = new QVBoxLayout(m_aiMatchGroup);
    aiMatchLayout->setContentsMargins(12, 16, 12, 11);
    aiMatchLayout->setSpacing(7);
    m_blackAiSummary = new QLabel(m_aiMatchGroup);
    m_whiteAiSummary = new QLabel(m_aiMatchGroup);
    m_blackAiSummary->setObjectName(QStringLiteral("mutedText"));
    m_whiteAiSummary->setObjectName(QStringLiteral("mutedText"));
    m_aiMatchConfigButton =
        new QPushButton(QStringLiteral("配置黑方 / 白方 AI"), m_aiMatchGroup);
    m_aiMatchConfigButton->setObjectName(QStringLiteral("secondaryButton"));
    m_aiMatchConfigButton->setMinimumHeight(36);
    aiMatchLayout->addWidget(m_blackAiSummary);
    aiMatchLayout->addWidget(m_whiteAiSummary);
    aiMatchLayout->addWidget(m_aiMatchConfigButton);
    m_aiMatchGroup->setVisible(false);
    sideLayout->addWidget(m_aiMatchGroup);

    m_aiHintLabel = new QLabel(
        QStringLiteral("内置 AI 会主动进攻并封堵胜点；双机模式下双方使用当前引擎。"), sidePanel);
    m_aiHintLabel->setObjectName(QStringLiteral("hintText"));
    m_aiHintLabel->setWordWrap(true);
    sideLayout->addWidget(m_aiHintLabel);

    m_apiGroup = new QGroupBox(QStringLiteral("外部 AI 接口"), sidePanel);
    auto *apiLayout = new QVBoxLayout(m_apiGroup);
    apiLayout->setContentsMargins(12, 15, 12, 11);
    apiLayout->setSpacing(7);
    m_endpointEdit = new QLineEdit(
        QStringLiteral(
            "http://127.0.0.1:8000/v1/move?provider=search&depth=3"),
        m_apiGroup);
    m_endpointEdit->setPlaceholderText(QStringLiteral("POST 接口地址"));
    m_endpointEdit->setToolTip(QStringLiteral("gomoku-ai/v1 的 POST 接口地址"));
    m_tokenEdit = new QLineEdit(m_apiGroup);
    m_tokenEdit->setEchoMode(QLineEdit::Password);
    m_tokenEdit->setPlaceholderText(QStringLiteral("Bearer Token（可选）"));
    m_tokenEdit->setToolTip(QStringLiteral("可选；Token 仅保存在本次运行内"));
    apiLayout->addWidget(m_endpointEdit);
    apiLayout->addWidget(m_tokenEdit);
    sideLayout->addWidget(m_apiGroup);

    m_messageLabel = new QLabel(QStringLiteral("选择模式后开始一局"), sidePanel);
    m_messageLabel->setObjectName(QStringLiteral("messageLabel"));
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setMinimumHeight(48);
    sideLayout->addWidget(m_messageLabel);
    sideLayout->addStretch();

    auto *buttonLayout = new QGridLayout;
    buttonLayout->setHorizontalSpacing(10);
    buttonLayout->setVerticalSpacing(10);
    m_startButton = new QPushButton(QStringLiteral("开始新局"), sidePanel);
    m_startButton->setObjectName(QStringLiteral("primaryButton"));
    m_startButton->setMinimumHeight(44);
    m_undoButton = new QPushButton(QStringLiteral("悔棋"), sidePanel);
    m_undoButton->setObjectName(QStringLiteral("secondaryButton"));
    m_undoButton->setMinimumHeight(44);
    m_pauseButton = new QPushButton(QStringLiteral("暂停对弈"), sidePanel);
    m_pauseButton->setObjectName(QStringLiteral("secondaryButton"));
    m_pauseButton->setMinimumHeight(44);
    m_pauseButton->setVisible(false);
    auto *secondaryButtons = new QHBoxLayout;
    secondaryButtons->setSpacing(10);
    secondaryButtons->addWidget(m_pauseButton);
    secondaryButtons->addWidget(m_undoButton);
    buttonLayout->addWidget(m_startButton, 0, 0, 1, 2);
    buttonLayout->addLayout(secondaryButtons, 1, 0, 1, 2);
    sideLayout->addLayout(buttonLayout);

    auto *shortcutLabel = new QLabel(QStringLiteral("快捷键：R 重新开始   ·   Z 悔棋"), sidePanel);
    shortcutLabel->setObjectName(QStringLiteral("shortcutText"));
    shortcutLabel->setAlignment(Qt::AlignCenter);
    sideLayout->addWidget(shortcutLabel);

    contentLayout->addWidget(sidePanel);
    rootLayout->addLayout(contentLayout, 1);
}

void MainWindow::applyStyle()
{
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget#central {
            background: #171b22;
            color: #f4f3ef;
            font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
            font-size: 14px;
        }
        QLabel#title {
            color: #f5d38b;
            font-size: 31px;
            font-weight: 700;
        }
        QLabel#subtitle {
            color: #747d8b;
            font-size: 11px;
            letter-spacing: 2px;
        }
        QLabel#rulePill {
            color: #c5cad2;
            background: #212730;
            border: 1px solid #303844;
            border-radius: 16px;
            padding: 7px 14px;
        }
        QFrame#boardFrame {
            background: #222831;
            border: 1px solid #323a46;
            border-radius: 18px;
        }
        QFrame#sidePanel {
            background: #20252d;
            border: 1px solid #303743;
            border-radius: 18px;
        }
        QLabel#sectionTitle {
            color: #aab1bc;
            font-size: 12px;
            font-weight: 600;
        }
        QFrame#turnCard {
            background: #292f39;
            border: 1px solid #373f4b;
            border-radius: 12px;
        }
        QLabel#turnStone {
            background: #12151a;
            border: 2px solid #525b68;
            border-radius: 19px;
            color: white;
            font-size: 15px;
            font-weight: 700;
        }
        QLabel#mutedText, QLabel#hintText, QLabel#shortcutText {
            color: #858e9b;
            font-size: 12px;
        }
        QLabel#turnLabel {
            color: #f4f3ef;
            font-size: 16px;
            font-weight: 600;
        }
        QLabel#countLabel {
            color: #f1c979;
            font-size: 13px;
            font-weight: 600;
        }
        QLabel#messageLabel {
            color: #bdc4ce;
            background: #191e25;
            border-left: 3px solid #d6a64d;
            border-radius: 5px;
            padding: 9px 11px;
        }
        QComboBox, QLineEdit {
            color: #e7e9ed;
            background: #171c23;
            border: 1px solid #3a424f;
            border-radius: 8px;
            padding: 9px 11px;
            selection-background-color: #9b7131;
        }
        QComboBox:hover, QLineEdit:focus {
            border-color: #b9863f;
        }
        QComboBox:disabled {
            color: #606875;
            background: #1b2027;
        }
        QComboBox QAbstractItemView {
            color: #e7e9ed;
            background: #222831;
            border: 1px solid #3a424f;
            outline: 0;
            margin: 0;
            padding: 0;
            selection-background-color: #8d672f;
        }
        QComboBoxPrivateContainer {
            background: #222831;
            border: 1px solid #3a424f;
            margin: 0;
            padding: 0;
        }
        QComboBoxPrivateScroller {
            background: #222831;
            border: none;
            min-height: 0;
            max-height: 0;
        }
        QGroupBox {
            color: #aab1bc;
            background: #1b2027;
            border: 1px solid #343c47;
            border-radius: 9px;
            margin-top: 8px;
            font-size: 12px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 11px;
            padding: 0 4px;
        }
        QPushButton {
            border-radius: 9px;
            padding: 9px 14px;
            font-weight: 600;
        }
        QPushButton#primaryButton {
            color: #1c1710;
            background: #e2b65e;
            border: 1px solid #efca7f;
        }
        QPushButton#primaryButton:hover {
            background: #edc572;
        }
        QPushButton#primaryButton:pressed {
            background: #c99a44;
        }
        QPushButton#secondaryButton {
            color: #d8dbe0;
            background: #2b323c;
            border: 1px solid #414a57;
        }
        QPushButton#secondaryButton:hover {
            background: #353d48;
            border-color: #626d7b;
        }
        QPushButton:disabled {
            color: #616976;
            background: #232932;
            border-color: #343b46;
        }
    )"));
}

void MainWindow::requestAiMove()
{
    if (!m_gameActive || !isAiTurn() || m_aiThinking
        || (selectedMode() == GameMode::AiVsAi && m_aiMatchPaused)) {
        return;
    }

    m_aiThinking = true;
    m_aiRequestedStone = m_game.currentPlayer();
    const QString color = m_aiRequestedStone == Stone::Black
                              ? QStringLiteral("黑方")
                              : QStringLiteral("白方");
    setMessage(QStringLiteral("%1 AI 正在思考…").arg(color));
    updateUi();

    AiProvider *provider = selectedAiProvider();
    if (provider == m_httpAi) {
        if (selectedMode() == GameMode::AiVsAi) {
            const AiSideConfig &config =
                m_aiRequestedStone == Stone::Black ? m_blackAiConfig
                                                   : m_whiteAiConfig;
            m_httpAi->setEndpoint(QUrl::fromUserInput(config.endpoint));
            m_httpAi->setBearerToken(config.token);
        } else {
            m_httpAi->setEndpoint(
                QUrl::fromUserInput(m_endpointEdit->text().trimmed()));
            m_httpAi->setBearerToken(m_tokenEdit->text());
        }
    }
    provider->requestMove(m_game.snapshot());
}

void MainWindow::finishTurn(const QString &moveDetail)
{
    m_boardWidget->update();
    if (m_game.winner() != Stone::Empty || m_game.isDraw()) {
        finishGame();
        return;
    }

    if (!moveDetail.isEmpty()) {
        if (selectedMode() == GameMode::AiVsAi) {
            setMessage(QStringLiteral("%1 已落子，准备下一回合").arg(moveDetail));
        } else {
            setMessage(QStringLiteral("%1 已落子，现在轮到你").arg(moveDetail));
        }
    }
    updateUi();

    if (isAiTurn()
        && !(selectedMode() == GameMode::AiVsAi && m_aiMatchPaused)) {
        const int delay = selectedMode() == GameMode::AiVsAi ? 520 : 0;
        QTimer::singleShot(delay, this, &MainWindow::requestAiMove);
    }
}

void MainWindow::finishGame()
{
    m_aiThinking = false;
    m_aiMatchPaused = true;
    m_aiRequestedStone = Stone::Empty;
    m_gameActive = false;
    m_boardWidget->setGameActive(true);
    updateUi();

    QString result;
    if (m_game.isDraw()) {
        result = QStringLiteral("棋盘已满，本局和棋！");
    } else if (m_game.winner() == Stone::Black) {
        if (selectedMode() == GameMode::HumanVsAi) {
            result = QStringLiteral("黑棋胜！恭喜你赢得本局。");
        } else if (selectedMode() == GameMode::AiVsAi) {
            result = QStringLiteral("黑方 AI 获胜！");
        } else {
            result = QStringLiteral("黑棋胜！");
        }
    } else {
        if (selectedMode() == GameMode::HumanVsAi) {
            result = QStringLiteral("白棋胜！AI 赢得本局。");
        } else if (selectedMode() == GameMode::AiVsAi) {
            result = QStringLiteral("白方 AI 获胜！");
        } else {
            result = QStringLiteral("白棋胜！");
        }
    }
    setMessage(result);

    if (showGameOverDialog(result)) {
        startNewGame();
    }
}

bool MainWindow::showGameOverDialog(const QString &result)
{
    QDialog dialog(this);
    dialog.setModal(true);
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setAttribute(Qt::WA_TranslucentBackground);
    dialog.setStyleSheet(QStringLiteral(R"(
        QFrame#resultCard {
            background: #20252d;
            border: 1px solid #48515f;
            border-radius: 18px;
        }
        QLabel#resultCaption {
            color: #8e97a4;
            font-family: "Microsoft YaHei UI";
            font-size: 12px;
            font-weight: 600;
        }
        QLabel#resultTitle {
            color: #f5d38b;
            font-family: "Microsoft YaHei UI";
            font-size: 25px;
            font-weight: 700;
        }
        QLabel#resultDetail {
            color: #b8bfc9;
            font-family: "Microsoft YaHei UI";
            font-size: 13px;
        }
        QPushButton {
            min-height: 42px;
            border-radius: 9px;
            padding: 8px 18px;
            font-family: "Microsoft YaHei UI";
            font-size: 14px;
            font-weight: 600;
        }
        QPushButton#resultPrimary {
            color: #1b1710;
            background: #e2b65e;
            border: 1px solid #f0cc83;
        }
        QPushButton#resultPrimary:hover { background: #edc572; }
        QPushButton#resultSecondary {
            color: #dce0e6;
            background: #2c333d;
            border: 1px solid #46505e;
        }
        QPushButton#resultSecondary:hover { background: #363e49; }
    )"));

    auto *outerLayout = new QVBoxLayout(&dialog);
    outerLayout->setContentsMargins(24, 24, 24, 24);
    auto *card = new QFrame(&dialog);
    card->setObjectName(QStringLiteral("resultCard"));
    card->setFixedWidth(450);
    auto *shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(42);
    shadow->setOffset(0, 12);
    shadow->setColor(QColor(0, 0, 0, 155));
    card->setGraphicsEffect(shadow);
    outerLayout->addWidget(card);

    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(28, 25, 28, 24);
    cardLayout->setSpacing(15);
    auto *heading = new QHBoxLayout;
    heading->setSpacing(17);
    auto *logo = new QLabel(card);
    logo->setFixedSize(72, 72);
    logo->setPixmap(
        QPixmap(QStringLiteral(":/assets/gomoku-logo.png"))
            .scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    auto *headingText = new QVBoxLayout;
    headingText->setSpacing(5);
    auto *caption = new QLabel(QStringLiteral("本局已结束"), card);
    caption->setObjectName(QStringLiteral("resultCaption"));
    auto *title = new QLabel(result, card);
    title->setObjectName(QStringLiteral("resultTitle"));
    title->setWordWrap(true);
    headingText->addWidget(caption);
    headingText->addWidget(title);
    heading->addWidget(logo);
    heading->addLayout(headingText, 1);
    cardLayout->addLayout(heading);

    auto *divider = new QFrame(card);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color:#39414d;"));
    cardLayout->addWidget(divider);
    auto *detail = new QLabel(
        QStringLiteral("可以查看最终棋局、悔棋复盘，或者立即开始新的一局。"),
        card);
    detail->setObjectName(QStringLiteral("resultDetail"));
    detail->setWordWrap(true);
    cardLayout->addWidget(detail);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(11);
    auto *reviewButton = new QPushButton(QStringLiteral("查看棋局"), card);
    reviewButton->setObjectName(QStringLiteral("resultSecondary"));
    auto *restartButton = new QPushButton(QStringLiteral("再来一局"), card);
    restartButton->setObjectName(QStringLiteral("resultPrimary"));
    buttons->addWidget(reviewButton);
    buttons->addWidget(restartButton);
    cardLayout->addLayout(buttons);

    bool restart = false;
    connect(reviewButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(restartButton, &QPushButton::clicked, &dialog, [&]() {
        restart = true;
        dialog.accept();
    });
    dialog.exec();
    return restart;
}

void MainWindow::updateUi()
{
    const bool hasMoves = m_game.moveCount() > 0;
    const bool humanInput = m_gameActive && !m_aiThinking && isHumanTurn()
                            && m_game.winner() == Stone::Empty && !m_game.isDraw();
    m_boardWidget->setInputEnabled(humanInput);
    m_boardWidget->update();
    m_undoButton->setEnabled(hasMoves);
    m_moveCountLabel->setText(QStringLiteral("%1 手").arg(m_game.moveCount()));

    if (!m_gameActive && !hasMoves) {
        m_turnStone->setText(QStringLiteral("—"));
        m_turnLabel->setText(QStringLiteral("等待开始"));
    } else if (m_game.winner() != Stone::Empty || m_game.isDraw()) {
        m_turnStone->setText(QStringLiteral("✓"));
        m_turnLabel->setText(QStringLiteral("对局结束"));
    } else {
        const bool black = m_game.currentPlayer() == Stone::Black;
        m_turnStone->setText(black ? QStringLiteral("黑") : QStringLiteral("白"));
        if (m_aiThinking) {
            m_turnLabel->setText(QStringLiteral("AI 思考中"));
        } else if (selectedMode() == GameMode::HumanVsAi) {
            m_turnLabel->setText(black ? QStringLiteral("你的回合")
                                      : QStringLiteral("AI 回合"));
        } else if (selectedMode() == GameMode::AiVsAi) {
            m_turnLabel->setText(m_aiMatchPaused
                                     ? QStringLiteral("对弈已暂停")
                                     : (black ? QStringLiteral("黑方 AI")
                                              : QStringLiteral("白方 AI")));
        } else {
            m_turnLabel->setText(black ? QStringLiteral("黑棋回合")
                                      : QStringLiteral("白棋回合"));
        }
        m_turnStone->setStyleSheet(black
            ? QStringLiteral("background:#111318;color:#f1f2f4;border:2px solid #525b68;border-radius:19px;")
            : QStringLiteral("background:#f1f0eb;color:#34373d;border:2px solid #aaa79f;border-radius:19px;"));
    }

    m_modeCombo->setEnabled(!m_aiThinking);
    m_aiCombo->setEnabled(selectedMode() != GameMode::LocalTwoPlayer
                          && !m_aiThinking);
    m_endpointEdit->setEnabled(!m_aiThinking);
    m_tokenEdit->setEnabled(!m_aiThinking);
    m_pauseButton->setVisible(selectedMode() == GameMode::AiVsAi);
    m_pauseButton->setEnabled(m_gameActive);
    m_pauseButton->setText(m_aiMatchPaused ? QStringLiteral("继续对弈")
                                           : QStringLiteral("暂停对弈"));
    m_aiMatchConfigButton->setEnabled(!m_aiThinking);
}

void MainWindow::setMessage(const QString &message, bool warning)
{
    m_messageLabel->setText(message);
    m_messageLabel->setStyleSheet(
        warning ? QStringLiteral("color:#efc3a9;background:#2a2020;border-left:3px solid #d87545;"
                                 "border-radius:5px;padding:9px 11px;")
                : QString());
}

bool MainWindow::isHumanTurn() const
{
    return selectedMode() == GameMode::LocalTwoPlayer
           || (selectedMode() == GameMode::HumanVsAi
               && m_game.currentPlayer() == Stone::Black);
}

bool MainWindow::isAiTurn() const
{
    return selectedMode() == GameMode::AiVsAi
           || (selectedMode() == GameMode::HumanVsAi
               && m_game.currentPlayer() == Stone::White);
}

GameMode MainWindow::selectedMode() const
{
    if (!m_modeCombo || m_modeCombo->currentIndex() == 0) {
        return GameMode::LocalTwoPlayer;
    }
    return m_modeCombo->currentIndex() == 1
               ? GameMode::HumanVsAi
               : GameMode::AiVsAi;
}

AiProvider *MainWindow::selectedAiProvider() const
{
    if (selectedMode() == GameMode::AiVsAi) {
        const AiSideConfig &config =
            m_game.currentPlayer() == Stone::Black ? m_blackAiConfig
                                                   : m_whiteAiConfig;
        return config.external ? static_cast<AiProvider *>(m_httpAi)
                               : static_cast<AiProvider *>(m_builtinAi);
    }
    return m_aiCombo->currentIndex() == 1
               ? static_cast<AiProvider *>(m_httpAi)
               : static_cast<AiProvider *>(m_builtinAi);
}
