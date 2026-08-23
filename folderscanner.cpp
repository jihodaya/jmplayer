#include "folderscanner.h"
#include "uistrings.h"
#include "nobfilehandler.h"
#include "gybfilehandler.h"
#include "okafilehandler.h"
#include "imsplayer.h"
#include "midiplayer.h"
#include <QFileInfoList>
#include <QDir>
#include <QDirIterator>

FolderScanner::FolderScanner(const QString& folderPath, QObject* parent)
    : QThread(parent), m_folderPath(folderPath), m_resultNode(nullptr)
{
}

FolderScanner::~FolderScanner()
{
    if (m_resultNode && m_resultNode->parent == nullptr) {
        // If not adopted by main tree, delete it to prevent leak
        delete m_resultNode;
    }
}

const QStringList& FolderScanner::playableFilters()
{
    static const QStringList filters = {
        "*.mid", "*.midi", "*.nob", "*.ims", "*.rol", "*.sop", "*.gyb",
        "*.oka", "*.okm", "*.okw", "*.vgm", "*.vgz"
    };
    return filters;
}

// A first pass that only counts, so the placeholder row can show a denominator.
//
// It is cheap next to the scan it precedes: walking 8,005 folders and naming
// 114,727 files measures about a second, where the scan proper reads every one
// of those files to pull a title out of it and takes minutes. Buying the "of
// how many" for one second of a two-minute wait is worth it - without it the
// row can only count upwards and still says nothing about when it ends.
int FolderScanner::countPlayableFiles(const QString& folderPath) const
{
    int n = 0;
    QDirIterator it(folderPath, playableFilters(), QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (isInterruptionRequested()) break;
        it.next();
        ++n;
    }
    return n;
}

void FolderScanner::reportProgress(bool force)
{
    if (!force && m_sinceLastReport.isValid() && m_sinceLastReport.elapsed() < 100)
        return;
    m_sinceLastReport.restart();
    emit progress(m_done, m_total);
}

void FolderScanner::run()
{
    m_sinceLastReport.start();
    m_total = countPlayableFiles(m_folderPath);
    reportProgress(true);

    QFileInfo folderInfo(m_folderPath);
    QString folderName = folderInfo.fileName();
    if (folderName.toUpper() == "BK") folderName = LSTR("병코돌고래", "BK Dolphin");
    m_resultNode = new PlaylistTreeNode(
        "📁 " + folderName, folderInfo.absoluteFilePath(), true, false);

    addFolderStructureToNode(m_resultNode, m_folderPath);

    reportProgress(true);
    emit scanFinished(this);
}

void FolderScanner::addFolderStructureToNode(PlaylistTreeNode* parentNode, const QString& folderPath)
{
    if (isInterruptionRequested()) return;

    QDir dir(folderPath);

    // Add MIDI files
    QFileInfoList midiFiles = dir.entryInfoList(playableFilters(), QDir::Files);
    for (const QFileInfo &fileInfo : midiFiles) {
        if (isInterruptionRequested()) return;

        QString displayName = "♫ " + fileInfo.fileName();

        // NOB 파일이면 LST에서 제목 추출 시도 (없으면 파일 헤더 Fallback)
        if (fileInfo.fileName().toLower().endsWith(".nob")) {
            QString lstTitle = NobFileHandler::extractTitleFromLst(fileInfo.absoluteFilePath());
            if (!lstTitle.isEmpty()) {
                displayName += " - " + lstTitle;
            } else {
                QString nobTitle = NobFileHandler::extractTitle(fileInfo.absoluteFilePath());
                if (!nobTitle.isEmpty()) {
                    displayName += " - " + nobTitle;
                }
            }
        } else if (fileInfo.fileName().toLower().endsWith(".gyb")) {
            QString gybTitle = GybFileHandler::extractTitle(fileInfo.absoluteFilePath());
            if (!gybTitle.isEmpty()) {
                displayName += " - " + gybTitle;
            }
        } else if (OkaFileHandler::isOkaFile(fileInfo.absoluteFilePath())) {
            QString okaTitle = OkaFileHandler::extractTitle(fileInfo.absoluteFilePath());
            if (!okaTitle.isEmpty()) {
                displayName += " - " + okaTitle;
            }
        } else if (isOplFile(fileInfo.absoluteFilePath())) {
            QString imsTitle = ImsPlayer::extractTitleQuick(fileInfo.absoluteFilePath());
            if (!imsTitle.isEmpty()) {
                displayName += " - " + imsTitle;
            }
        } else if (fileInfo.fileName().endsWith(".mid", Qt::CaseInsensitive) ||
                   fileInfo.fileName().endsWith(".midi", Qt::CaseInsensitive)) {
            // Most .mid files leave the title slot empty and keep the filename;
            // the ones that fill it in have been showing an 8.3 name for no reason.
            QString midTitle = MidiPlayer::extractTitleQuick(fileInfo.absoluteFilePath());
            if (!midTitle.isEmpty()) {
                displayName += " - " + midTitle;
            }
        }

        PlaylistTreeNode* fileNode = new PlaylistTreeNode(
            displayName, fileInfo.absoluteFilePath(), false, false);
        fileNode->parent = parentNode;
        parentNode->children.append(fileNode);

        ++m_done;
        reportProgress(false);
    }

    // Add subfolders recursively
    QFileInfoList subFolders = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &folderInfo : subFolders) {
        if (isInterruptionRequested()) return;

        QString subFolderName = folderInfo.fileName();
        if (subFolderName.toUpper() == "BK") subFolderName = LSTR("병코돌고래", "BK Dolphin");
        PlaylistTreeNode* folderNode = new PlaylistTreeNode(
            "📁 " + subFolderName, folderInfo.absoluteFilePath(), true, false);
        folderNode->parent = parentNode;
        parentNode->children.append(folderNode);

        // Recursively add structure
        addFolderStructureToNode(folderNode, folderInfo.absoluteFilePath());
    }
}

bool FolderScanner::isOplFile(const QString& filePath)
{
    QString lowerPath = filePath.toLower();
    return lowerPath.endsWith(".ims") || lowerPath.endsWith(".rol") || lowerPath.endsWith(".sop") || lowerPath.endsWith(".vgm") || lowerPath.endsWith(".vgz");
}
