#include "folderscanner.h"
#include "uistrings.h"
#include "nobfilehandler.h"
#include "gybfilehandler.h"
#include "okafilehandler.h"
#include "imsplayer.h"
#include <QFileInfoList>
#include <QDir>

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

void FolderScanner::run()
{
    QFileInfo folderInfo(m_folderPath);
    QString folderName = folderInfo.fileName();
    if (folderName.toUpper() == "BK") folderName = LSTR("병코돌고래", "BK Dolphin");
    m_resultNode = new PlaylistTreeNode(
        "📁 " + folderName, folderInfo.absoluteFilePath(), true, false);

    addFolderStructureToNode(m_resultNode, m_folderPath);

    emit scanFinished(this);
}

void FolderScanner::addFolderStructureToNode(PlaylistTreeNode* parentNode, const QString& folderPath)
{
    if (isInterruptionRequested()) return;

    QDir dir(folderPath);

    // Add MIDI files
    QStringList filters;
    filters << "*.mid" << "*.midi" << "*.nob" << "*.ims" << "*.rol" << "*.sop" << "*.gyb"
            << "*.oka" << "*.okm" << "*.okw" << "*.vgm" << "*.vgz";
    QFileInfoList midiFiles = dir.entryInfoList(filters, QDir::Files);
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
        }

        PlaylistTreeNode* fileNode = new PlaylistTreeNode(
            displayName, fileInfo.absoluteFilePath(), false, false);
        fileNode->parent = parentNode;
        parentNode->children.append(fileNode);
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
