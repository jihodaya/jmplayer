// lyricswindow_editor.cpp
// 가사 편집 기능 (별도 파일로 분리)

#include "lyricswindow.h"
#include "uistrings.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QDir>
#include <QVBoxLayout>
#include <QLabel>
#include <QTextOption>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QRegularExpression>
#include <QStringList>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#endif

void LyricsWindow::onEditLyrics()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Edit Lyrics"));
    dialog.resize(420, 520);

    dialog.setStyleSheet(
        "QDialog {"
        "    background-color: #2b2b2b;"
        "    color: #ffffff;"
        "}"
    );

#ifdef _WIN32
    HWND hwnd = (HWND)dialog.winId();
    BOOL value = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
#endif

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    QLabel *noticeLabel = new QLabel(&dialog);
    QStringList noticeLines;
    noticeLines << LSTR("~ : hold for one more beat (e.g., 가- -> 가~)", "~ : hold for one more beat (e.g., go- -> go~)")
                << tr("# : insert a one-beat rest before the next syllable")
                << tr("@ : repeat from the beginning after this line");
    noticeLabel->setText(noticeLines.join(QStringLiteral("\n")));

    noticeLabel->setStyleSheet(
        "QLabel {"
        "    color: #FFAA00;"
        "    font-size: 11px;"
        "    padding: 5px;"
        "    background-color: #3a3a3a;"
        "    border: 1px solid #555555;"
        "    border-radius: 3px;"
        "}"
    );
    noticeLabel->setWordWrap(true);
    layout->addWidget(noticeLabel);

    QPlainTextEdit *editor = new QPlainTextEdit(&dialog);
    QStringList displayLyrics;
    displayLyrics.reserve(allLyrics.size());
    for (const QString &line : allLyrics) {
        QString normalized = line;
        normalized.replace(QChar(0x00A0), QChar(' '));
        normalized = normalized.trimmed();
        if (!normalized.isEmpty()) {
            normalized.replace(QRegularExpression("\\s{2,}"), " ");
        }
        displayLyrics.append(normalized);
    }
    editor->setPlainText(displayLyrics.join("\n"));
    editor->setStyleSheet(
        "QPlainTextEdit {"
        "    background-color: #2b2b2b;"
        "    color: #ffffff;"
        "    font-size: 14px;"
        "    border: 1px solid #555555;"
        "    padding: 5px;"
        "}"
    );
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor->setWordWrapMode(QTextOption::NoWrap);
    editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    editor->document()->setDocumentMargin(0);

    QTextBlockFormat blockFormat;
    blockFormat.setLeftMargin(0);
    blockFormat.setTextIndent(0);
    QTextCursor cursor = editor->textCursor();
    cursor.select(QTextCursor::Document);
    cursor.setBlockFormat(blockFormat);

    layout->addWidget(editor);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(Qt::Horizontal, &dialog);
    buttonBox->setStyleSheet(
        "QPushButton {"
        "    background-color: #3a3a3a;"
        "    color: #ffffff;"
        "    border: 1px solid #555555;"
        "    padding: 5px 15px;"
        "    min-width: 60px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #4a4a4a;"
        "}"
        "QPushButton:pressed {"
        "    background-color: #5a5a5a;"
        "}"
    );
    layout->addWidget(buttonBox);

    QPushButton *saveButton = buttonBox->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    QPushButton *cancelButton = buttonBox->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    QPushButton *exportButton = buttonBox->addButton(tr("Export"), QDialogButtonBox::ActionRole);

    QObject::connect(exportButton, &QPushButton::clicked, this, [this, editor, &dialog]() {
        QStringList exportLyrics = editor->toPlainText().split('\n', Qt::KeepEmptyParts);
        for (QString &line : exportLyrics) {
            line.remove('\r');
        }
        exportLyricsToFile(exportLyrics, &dialog);
    });

    QObject::connect(saveButton, &QPushButton::clicked, &dialog, [this, &dialog]() {
        auto reply = QMessageBox::question(
            &dialog,
            tr("Save Lyrics"),
            LSTR("가사 변경 사항을 저장하시겠습니까?", "Save the lyric changes?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes);
        if (reply == QMessageBox::Yes) {
            dialog.accept();
        }
    });

    QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        QStringList newLyrics = editor->toPlainText().split('\n', Qt::KeepEmptyParts);
        for (QString &line : newLyrics) {
            line.remove('\r');
        }

        // Always save beside the song as <name>.txt - never into the .NOB. The
        // lyric block's spacing carries the sync, so rewriting it from edited
        // text is a good way to lose the timing; the external file is read in
        // preference to the embedded lyrics anyway (loadLyricsForNob).
        const QString externalPath = externalLyricsFilePath();
        if (!externalPath.isEmpty()) {
            if (!writeExternalLyrics(newLyrics)) {
                QMessageBox::warning(&dialog, tr("Save Lyrics"),
                                     tr("Failed to save lyrics to %1.")
                                         .arg(QDir::toNativeSeparators(externalPath)));
            }
        }

        setLyrics(newLyrics);
        reset();
        emit lyricsEdited(allLyrics);
    }
}
