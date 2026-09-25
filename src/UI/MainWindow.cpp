#include "MainWindow.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSlider>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Ptuch Editor"));
    resize(1000, 700);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setObjectName("mainSplitter");
    splitter->setHandleWidth(2);

    splitter->addWidget(createTopPanel());

    m_editor = new QPlainTextEdit(splitter);
    m_editor->setObjectName("mainEditor");
    m_editor->setPlaceholderText(QStringLiteral("Начните печатать текст..."));
    splitter->addWidget(m_editor);

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({250, 450});

    setCentralWidget(splitter);

    applyStyle();
}

QWidget *MainWindow::createTopPanel()
{
    auto *topWidget = new QWidget;
    topWidget->setObjectName("topPanel");

    auto *layout = new QHBoxLayout(topWidget);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    m_textInput = new QLineEdit;
    m_textInput->setObjectName("textInput");
    m_textInput->setPlaceholderText(QStringLiteral("Введите текст..."));
    layout->addWidget(m_textInput, /*stretch*/ 1);

    layout->addWidget(createSlidersPanel(), /*stretch*/ 0);

    return topWidget;
}

QWidget *MainWindow::createSlidersPanel()
{
    auto *panel = new QWidget;
    panel->setObjectName("sidePanel");
    panel->setFixedWidth(260);

    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(8);

    m_comboBox = new QComboBox;
    m_comboBox->setObjectName("modeCombo");
    m_comboBox->addItems({QStringLiteral("Режим 1"),
                           QStringLiteral("Режим 2"),
                           QStringLiteral("Режим 3"),
                           QStringLiteral("Режим 4")});
    layout->addWidget(m_comboBox);

    auto *slidersRow = new QHBoxLayout;
    slidersRow->setSpacing(12);

    static const char *sliderNames[] = {"A", "B", "C", "D", "E"};
    for (const char *name : sliderNames) {
        auto *sliderColumn = new QVBoxLayout;
        sliderColumn->setSpacing(4);

        auto *slider = new QSlider(Qt::Vertical);
        slider->setRange(0, 100);
        slider->setValue(50);
        slider->setMinimumHeight(140);

        auto *label = new QLabel(QString::fromLatin1(name));
        label->setAlignment(Qt::AlignCenter);

        sliderColumn->addWidget(slider, 0, Qt::AlignHCenter);
        sliderColumn->addWidget(label, 0, Qt::AlignHCenter);

        slidersRow->addLayout(sliderColumn);
    }

    layout->addLayout(slidersRow);
    layout->addStretch(1);

    return panel;
}

void MainWindow::applyStyle()
{
    setStyleSheet(R"(
        QMainWindow, QWidget#topPanel, QWidget#sidePanel {
            background-color: #3a3a3a;
        }

        QWidget {
            color: #f0f0f0;
            font-size: 13px;
        }

        QSplitter#mainSplitter::handle {
            background-color: #2a2a2a;
        }

        QLineEdit#textInput {
            background-color: #454545;
            border: 1px solid #565656;
            border-radius: 4px;
            padding: 6px 8px;
            color: #ffffff;
            selection-background-color: #6a6a6a;
        }

        QComboBox#modeCombo {
            background-color: #454545;
            border: 1px solid #565656;
            border-radius: 4px;
            padding: 4px 8px;
            color: #ffffff;
        }

        QComboBox#modeCombo::drop-down {
            border: none;
        }

        QComboBox#modeCombo QAbstractItemView {
            background-color: #454545;
            color: #ffffff;
            selection-background-color: #6a6a6a;
        }

        QSlider::groove:vertical {
            background: #2a2a2a;
            width: 6px;
            border-radius: 3px;
        }

        QSlider::handle:vertical {
            background: #d0d0d0;
            border: 1px solid #808080;
            height: 14px;
            margin: 0 -4px;
            border-radius: 7px;
        }

        QSlider::handle:vertical:hover {
            background: #ffffff;
        }

        QSlider::sub-page:vertical {
            background: #6a6a6a;
            border-radius: 3px;
        }

        QLabel {
            color: #dddddd;
        }

        QPlainTextEdit#mainEditor {
            background-color: #333333;
            color: #ffffff;
            border: none;
            padding: 8px;
            selection-background-color: #6a6a6a;
        }
    )");
}
