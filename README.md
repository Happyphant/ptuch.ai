# Ptuch Editor

Простой текстовый редактор на Qt 6 Widgets.

## Разметка окна

- Окно разделено вертикально (сверху/снизу) через `QSplitter`.
- **Верхняя часть**: слева — однострочное текстовое поле (`QLineEdit`),
  справа — панель шириной 260px с `QComboBox` и рядом из пяти вертикальных
  `QSlider`.
- **Нижняя часть**: многострочный текстовый редактор (`QPlainTextEdit`),
  занимающий всё доступное пространство.
- Цветовая гамма: тёмно-серый фон (`#333`-`#454545`) и белый/светло-серый
  текст, задаётся через `setStyleSheet` в `MainWindow::applyStyle()`.

## Структура

```
CMakeLists.txt
src/
  main.cpp
  MainWindow.h
  MainWindow.cpp
```

## Сборка

Используется Qt 6.11.2 (arm64), собранный в
`/Users/alexanderrabinov/Workspace/Qt/6.11.2-arm64`.

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/Users/alexanderrabinov/Workspace/Qt/6.11.2-arm64 \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Запуск

```bash
./build/PtuchEditor.app/Contents/MacOS/PtuchEditor
```
