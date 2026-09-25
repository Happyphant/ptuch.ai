#pragma once

#include <QMainWindow>

class QLineEdit;
class QPlainTextEdit;
class QComboBox;
class QSlider;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    QWidget *createTopPanel();
    QWidget *createSlidersPanel();
    void applyStyle();

    QLineEdit *m_textInput = nullptr;
    QComboBox *m_comboBox = nullptr;
    QPlainTextEdit *m_editor = nullptr;
};
