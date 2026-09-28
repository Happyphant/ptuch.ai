// document_state_test.cpp
#include <QtTest>

#include "suggestion/document_state.h"

// Тесты DocumentState: безопасное построение контекста автодополнения.
class DocumentStateTest final : public QObject
{
    Q_OBJECT

private slots:
    void emptyDocument();
    void cursorAtStart();
    void cursorInMiddle();
    void largeTextTruncatesPrefix();
    void largeTextTruncatesSuffix();
    void customLimitNeverExceedsHardCap();
    void selectionTruncated();
    void cursorClampedToBounds();
    void generationIdChangeInvalidatesOldResponses();
    void ghostSuggestionNotIncluded();
};

void DocumentStateTest::emptyDocument()
{
    const auto state = DocumentState::capture(
        QString(), /*cursor*/ 0, QString(),
        /*generationId*/ 1, /*timestamp*/ 1000);

    QCOMPARE(state.text(), QString());
    QCOMPARE(state.cursorPosition(), 0);
    QCOMPARE(state.selectedText(), QString());
    QCOMPARE(state.recentTextBeforeCursor(), QString());
    QCOMPARE(state.lastModifiedMs(), qint64(1000));
    QCOMPARE(state.generationId(), quint64(1));

    const auto ctx = state.buildContext();
    QCOMPARE(ctx.prefix, QString());
    QCOMPARE(ctx.suffix, QString());
    QCOMPARE(ctx.cursorPosition, 0);
    QVERIFY(!ctx.prefixTruncated);
    QVERIFY(!ctx.suffixTruncated);
}

void DocumentStateTest::cursorAtStart()
{
    const QString text = QStringLiteral("hello world");
    const auto state = DocumentState::capture(
        text, /*cursor*/ 0, QString(), 1, 1000);

    QCOMPARE(state.cursorPosition(), 0);
    // Ничего перед курсором — префикс пуст, даже при непустом документе.
    QCOMPARE(state.recentTextBeforeCursor(), QString());

    const auto ctx = state.buildContext();
    QCOMPARE(ctx.prefix, QString());
    // Весь текст уходит в суффикс (здесь он короче лимита).
    QCOMPARE(ctx.suffix, text);
    QVERIFY(!ctx.prefixTruncated);
    QVERIFY(!ctx.suffixTruncated);
}

void DocumentStateTest::cursorInMiddle()
{
    const QString text = QStringLiteral("The quick brown fox");
    const int cursor = 10; // после "The quick "
    const auto state = DocumentState::capture(
        text, cursor, QStringLiteral("quick"), 1, 1000);

    QCOMPARE(state.cursorPosition(), cursor);
    QCOMPARE(state.selectedText(), QStringLiteral("quick"));
    QCOMPARE(state.recentTextBeforeCursor(), text.left(cursor));

    const auto ctx = state.buildContext();
    QCOMPARE(ctx.prefix, QStringLiteral("The quick "));
    QCOMPARE(ctx.suffix, QStringLiteral("brown fox"));
    QCOMPARE(ctx.selection, QStringLiteral("quick"));
    QCOMPARE(ctx.cursorPosition, cursor);
    QVERIFY(!ctx.prefixTruncated);
    QVERIFY(!ctx.suffixTruncated);
}

void DocumentStateTest::largeTextTruncatesPrefix()
{
    // 50k символов, курсор в конце: в контекст должны попасть
    // только последние N символов, а не весь документ.
    const QString text(50000, QLatin1Char('a'));
    const auto state = DocumentState::capture(
        text, text.size(), QString(), 1, 1000);

    const auto ctx = state.buildContext();

    QVERIFY(ctx.prefix.size() <= DocumentState::kDefaultContextLimitChars);
    QCOMPARE(ctx.prefix.size(),
             qint64(DocumentState::kDefaultContextLimitChars));
    QVERIFY(ctx.prefixTruncated);
    // Хвост документа в префиксе именно тот, что прилегает к курсору.
    QVERIFY(ctx.prefix.endsWith(QStringLiteral("aaaa")));
    // Полный текст в контекст не попал.
    QVERIFY(ctx.prefix.size() < state.text().size());
    // При этом сам снимок полный текст хранит (для внутренних нужд).
    QCOMPARE(state.text().size(), qint64(50000));
}

void DocumentStateTest::largeTextTruncatesSuffix()
{
    const QString text(50000, QLatin1Char('b'));
    const auto state = DocumentState::capture(
        text, /*cursor*/ 0, QString(), 1, 1000);

    const auto ctx = state.buildContext();

    QCOMPARE(ctx.prefix, QString());
    QCOMPARE(ctx.suffix.size(),
             qint64(DocumentState::kDefaultContextLimitChars));
    QVERIFY(ctx.suffixTruncated);
    // Начало суффикса — это начало документа (курсор в начале).
    QCOMPARE(ctx.suffix, text.left(DocumentState::kDefaultContextLimitChars));
}

void DocumentStateTest::customLimitNeverExceedsHardCap()
{
    const QString text(20000, QLatin1Char('c'));
    const auto state = DocumentState::capture(
        text, text.size(), QString(), 1, 1000);

    // Явно запросили больше жёсткого потолка — получили потолок.
    const auto ctx = state.buildContext(1'000'000);
    QCOMPARE(ctx.prefix.size(),
             qint64(DocumentState::kHardContextLimitChars));
    QVERIFY(ctx.prefix.size() <= DocumentState::kHardContextLimitChars);

    // Маленький явный лимит применяется.
    const auto small = state.buildContext(16);
    QCOMPARE(small.prefix.size(), qint64(16));

    // Некорректный (<=0) лимит заменяется дефолтным, не нулём.
    const auto fallback = state.buildContext(0);
    QCOMPARE(fallback.prefix.size(),
             qint64(DocumentState::kDefaultContextLimitChars));

    // Ограничение размера действует и на лимит самого снимка.
    const auto greedyState = DocumentState::capture(
        text, text.size(), QString(), 1, 1000, /*contextLimitChars*/ 1'000'000);
    QCOMPARE(greedyState.contextLimitChars(),
             DocumentState::kHardContextLimitChars);
}

void DocumentStateTest::selectionTruncated()
{
    const QString selection(10000, QLatin1Char('s'));
    const auto state = DocumentState::capture(
        QStringLiteral("x"), 1, selection, 1, 1000);

    const auto ctx = state.buildContext();
    QCOMPARE(ctx.selection.size(),
             qint64(DocumentState::kHardContextLimitChars));
}

void DocumentStateTest::cursorClampedToBounds()
{
    const QString text = QStringLiteral("abc");

    const auto beforeStart = DocumentState::capture(
        text, /*cursor*/ -5, QString(), 1, 1000);
    QCOMPARE(beforeStart.cursorPosition(), 0);
    QCOMPARE(beforeStart.buildContext().prefix, QString());

    const auto afterEnd = DocumentState::capture(
        text, /*cursor*/ 999, QString(), 1, 1000);
    QCOMPARE(afterEnd.cursorPosition(), int(text.size()));

    const auto ctx = afterEnd.buildContext();
    // Суффикс не вылезает за конец документа.
    QCOMPARE(ctx.suffix, QString());
    QVERIFY(!ctx.suffixTruncated);
}

void DocumentStateTest::generationIdChangeInvalidatesOldResponses()
{
    const QString text = QStringLiteral("some text");
    auto state = DocumentState::capture(text, text.size(), QString(), 7, 1000);

    QVERIFY(state.isGenerationCurrent(7));
    QVERIFY(!state.isGenerationCurrent(6));
    QCOMPARE(state.buildContext().generationId, quint64(7));

    // Начали новый запрос: id сменился, старый ответ игнорируется.
    state.setGenerationId(8);
    QVERIFY(!state.isGenerationCurrent(7));
    QVERIFY(state.isGenerationCurrent(8));
    QCOMPARE(state.buildContext().generationId, quint64(8));

    // Снимок нового запроса несёт уже новый id.
    const auto next = DocumentState::capture(
        text + QStringLiteral("!"), text.size() + 1, QString(), 8, 2000);
    QVERIFY(next.isGenerationCurrent(8));
    QVERIFY(!next.isGenerationCurrent(7));
    QCOMPARE(next.lastModifiedMs(), qint64(2000));
}

void DocumentStateTest::ghostSuggestionNotIncluded()
{
    // Ghost-подсказка живёт вне документа: снимок строится только из
    // текста документа, в контекст она попасть не может.
    const QString document = QStringLiteral("hello wor");
    const QString ghost = QStringLiteral("hello world");

    const auto state = DocumentState::capture(
        document, document.size(), QString(), 1, 1000);

    const auto ctx = state.buildContext();
    QCOMPARE(ctx.prefix, document);
    QVERIFY(!ctx.prefix.contains(QStringLiteral("ld")));
    QVERIFY(ghost != document);
}

QTEST_APPLESS_MAIN(DocumentStateTest)

#include "document_state_test.moc"
