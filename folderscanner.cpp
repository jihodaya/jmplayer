#include "folderscanner.h"
#include "uistrings.h"
#include "nobfilehandler.h"
#include "gybfilehandler.h"
#include "okafilehandler.h"
#include "mdxplayer.h"
#include "vgmplayer.h"
#include "imsplayer.h"
#include "midiplayer.h"
#include "rcpfilehandler.h"
#include "sngmidi.h"
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
    // .r36 / .g36 / .g18 (Recomposer's other forms) are deliberately NOT here.
    // rcpfilehandler.cpp has a path for them, but no sample has ever existed in
    // this library - it was never run on a real file, and it detects the form
    // by searching the whole file for "G36"/"G18", which a real RCP3.0 file is
    // not known to contain. Removed 2026-09-28 until samples turn up.
    static const QStringList filters = {
        "*.mid", "*.midi", "*.nob", "*.rcp", "*.sng", "*.mdx", "*.mdz", "*.ims", "*.rol", "*.sop", "*.gyb",
        "*.oka", "*.okm", "*.okw", "*.vgm", "*.vgz"
    };
    return filters;
}

// Every other place that asks "is this a song" goes through these. There were
// nine hand-written copies of the list across the window code and three had
// drifted: the open dialog, adding files, and opening from outside all lacked
// .okw, and adding files lacked .vgm/.vgz too (found 2026-09-28).
bool FolderScanner::isPlayableSuffix(const QString& suffix)
{
    const QString want = QStringLiteral("*.") + suffix.toLower();
    return playableFilters().contains(want);
}

QString FolderScanner::openDialogFilter()
{
    return QStringLiteral("Music Files (") + playableFilters().join(' ')
         + QStringLiteral(" *.zip);;All Files (*)");
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


// The playlist row for one song: its filename, and the title the format
// carries if it has one.
//
// Shared with the playlist Refresh, which used to name a newly discovered
// file with its bare filename and never look again - so a file that appeared
// after the first scan could never get a title, and neither could one whose
// format only became readable in a later build. Keeping this in one place is
// the same reason playableFilters() is shared.
QString FolderScanner::displayNameFor(const QFileInfo& fileInfo)
{
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
    } else if (fileInfo.fileName().endsWith(".rcp", Qt::CaseInsensitive) ||
               fileInfo.fileName().endsWith(".r36", Qt::CaseInsensitive) ||
               fileInfo.fileName().endsWith(".g36", Qt::CaseInsensitive) ||
               fileInfo.fileName().endsWith(".g18", Qt::CaseInsensitive) ||
               fileInfo.fileName().endsWith(".sng", Qt::CaseInsensitive)) {
        // Recomposer needs asking by name. There was no branch for it, so a
        // .rcp fell through to the OPL reader below and the playlist showed
        // "RCM-PC98V2.0(C)COME ON MUSIC" - the file's signature, which is
        // what sits at offset 0. The song name is 64 bytes at 0x20.
        // .sng belongs to two unrelated sequencers; ask the file which.
        const QString path = fileInfo.absoluteFilePath();
        QString rcpTitle = sngmidi::isSngFile(path) ? sngmidi::extractTitle(path)
                                                    : RcpFileHandler::extractTitle(path);
        if (!rcpTitle.isEmpty()) {
            displayName += " - " + rcpTitle;
        }
    } else if (fileInfo.fileName().endsWith(".mdx", Qt::CaseInsensitive) ||
               fileInfo.fileName().endsWith(".mdz", Qt::CaseInsensitive)) {
        // MDX titles are Shift-JIS, and there was no branch for them, so a
        // .mdx fell through to the OPL reader below and its title came back
        // decoded in the system codepage - Korean, on this machine, which
        // is why the playlist read Hangul where the song name should be.
        QString mdxTitle = MdxPlayer::extractTitleQuick(fileInfo.absoluteFilePath());
        if (!mdxTitle.isEmpty() && mdxTitle != fileInfo.completeBaseName()) {
            displayName += " - " + mdxTitle;
        }
    } else if (fileInfo.fileName().endsWith(".vgm", Qt::CaseInsensitive) ||
               fileInfo.fileName().endsWith(".vgz", Qt::CaseInsensitive)) {
        // A VGM's GD3 tag carries the track and the game it came from, and
        // that is a far better playlist row than a filename made by a
        // ripping tool.
        QString vgmTitle = VgmPlayer::extractTitleQuick(fileInfo.absoluteFilePath());
        if (!vgmTitle.isEmpty()) {
            displayName += " - " + vgmTitle;
        }
    } else if (fileInfo.fileName().endsWith(".okw", Qt::CaseInsensitive)) {
        // isOkaFile() answers .oka and .okm only. An .okw is the same
        // Oksori header with the title in the same place, so it reads the
        // same way - it just never had a branch, in either title path.
        QString okwTitle = OkaFileHandler::extractTitle(fileInfo.absoluteFilePath());
        if (!okwTitle.isEmpty()) {
            displayName += " - " + okwTitle;
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
    return displayName;
}

void FolderScanner::addFolderStructureToNode(PlaylistTreeNode* parentNode, const QString& folderPath)
{
    if (isInterruptionRequested()) return;

    QDir dir(folderPath);

    // Add MIDI files
    QFileInfoList midiFiles = dir.entryInfoList(playableFilters(), QDir::Files);
    for (const QFileInfo &fileInfo : midiFiles) {
        if (isInterruptionRequested()) return;

        QString displayName = displayNameFor(fileInfo);


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
