// Split from mainwindow.cpp (playlist domain) - implementation-only
// file split, identical behavior. Same pattern as lyricswindow_editor.cpp.
#include "mainwindow.h"
#include "uistrings.h"
#include "folderscanner.h"
#include "pianorollwindow.h"
#include "channelmonitor.h"
#include "lyricswindow.h"
#include "constants.h"
#include "settingsmanager.h"
#include "oplstereodialog.h"
#include <QCloseEvent>
#include "nobfilehandler.h"
#include "gybfilehandler.h"
#include "okafilehandler.h"
#include "okaplayer.h"
#include "okabackend.h"
#include "soundfontmanagerdialog.h"
#include "imsplayer.h"
#include "gybplayer.h"
#include <QApplication>
#include <QStatusBar>
#include <QDateTime>
#include <QtCore/private/qzipreader_p.h>
#include <QMessageBox>
#include <QFileDialog>
#include <QTextStream>
#include <QFile>
#include <QStandardPaths>
#include <QDir>
#include <QDirIterator>
#include <QVector>
#include <cmath>
#include <iostream>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSettings>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QProgressDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>
#include <QMenuBar>
#include "issfilehandler.h"
#include <QMenu>
#include <QAction>
#include <QItemSelectionModel>
#include <algorithm>
#include <climits>
#include "playlistmodel.h"
#include <windows.h>
#include <dwmapi.h>


void MainWindow::openFile()
{
    // Set default directory to actual executable location (works with PyInstaller)
    QString defaultDir = SettingsManager::instance().value("General/lastOpenDirectory", getActualExecutablePath()).toString();

    QStringList fileNames = QFileDialog::getOpenFileNames(this,
        "Open MIDI Files", defaultDir, "Music Files (*.mid *.midi *.nob *.ims *.rol *.sop *.gyb *.oka *.okm *.vgm *.vgz *.zip);;All Files (*)");

    if (!fileNames.isEmpty()) {
        for (const QString &fileName : fileNames) {
            addFileToCurrentNode(fileName);
            SettingsManager::instance().setValue("General/lastOpenDirectory", QFileInfo(fileName).absolutePath());
        }
    }
}

void MainWindow::openFolder()
{
    // Set default directory to actual executable location (works with PyInstaller)
    QString defaultDir = SettingsManager::instance().value("General/lastOpenDirectory", getActualExecutablePath()).toString();

    QString folderPath = QFileDialog::getExistingDirectory(this, "Select Folder", defaultDir);

    if (!folderPath.isEmpty()) {
        // Use new tree system - add folder to current node
        addFolderToCurrentNode(folderPath);
        SettingsManager::instance().setValue("General/lastOpenDirectory", folderPath);
    }
}

void MainWindow::removeFile()
{
    QModelIndexList selectedIndexes;
    if (fileList->selectionModel())
        selectedIndexes = fileList->selectionModel()->selectedIndexes();

    if (selectedIndexes.isEmpty()) {
        return;
    }

    // Collect items to remove (exclude Parent Folder)
    QStringList itemsToRemove;
    for (const QModelIndex& idx : selectedIndexes) {
        int itemType = idx.data(PlaylistModel::TypeRole).toInt();
        if (itemType != PARENT_FOLDER) {
            QString itemPath = idx.data(PlaylistModel::PathRole).toString();
            itemsToRemove.append(itemPath);
        }
    }

    if (itemsToRemove.isEmpty()) {
        return;
    }

    // Confirm deletion
    QString confirmMessage;
    if (itemsToRemove.size() == 1) {
        confirmMessage = QString("Remove selected item from playlist?");
    } else {
        confirmMessage = QString("Remove %1 selected items from playlist?").arg(itemsToRemove.size());
    }

    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "Confirm Remove",
        confirmMessage,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (reply != QMessageBox::Yes) {
        return;
    }

    // Remove items from the tree structure
    for (const QString &itemPath : itemsToRemove) {
        removeItemFromCurrentNode(itemPath);
    }

    // Update UI to reflect tree changes
    updateUIFromCurrentNode();

    // Save changes
    triggerSavePlaylistTree();
}

void MainWindow::sortFiles()
{
    if (!playlistModel) return;

    // Toggle sort order
    sortAscending = !sortAscending;

    // Partition the model rows into parent / folders / files
    QVector<PlaylistRow> parentRows;
    QVector<PlaylistRow> folders;
    QVector<PlaylistRow> files;

    for (const PlaylistRow& r : playlistModel->rows()) {
        if (r.type == PARENT_FOLDER) parentRows.append(r);
        else if (r.type == FOLDER)   folders.append(r);
        else                         files.append(r);
    }

    // Sort folders and files separately
    auto cmp = [this](const PlaylistRow& a, const PlaylistRow& b) {
        return sortAscending ? a.name < b.name : a.name > b.name;
    };
    std::sort(folders.begin(), folders.end(), cmp);
    std::sort(files.begin(), files.end(), cmp);

    // Rebuild in order: parent folder, folders, files
    QVector<PlaylistRow> rows;
    rows.reserve(parentRows.size() + folders.size() + files.size());
    rows += parentRows;
    rows += folders;
    rows += files;
    playlistModel->setRows(std::move(rows));
}

void MainWindow::savePlaylist()
{
    QString filePath = QFileDialog::getSaveFileName(this,
        "Save Playlist", SettingsManager::instance().value("General/lastOpenDirectory", "").toString(), "Playlist Files (*.jjpl);;All Files (*)");

    if (!filePath.isEmpty()) {
        savePlaylistToFile(filePath);
        SettingsManager::instance().setValue("General/lastOpenDirectory", QFileInfo(filePath).absolutePath());
    }
}

void MainWindow::loadPlaylist()
{
    QString filePath = QFileDialog::getOpenFileName(this,
        "Load Playlist", SettingsManager::instance().value("General/lastOpenDirectory", "").toString(), "Playlist Files (*.jjpl);;All Files (*)");

    if (!filePath.isEmpty()) {
        loadPlaylistFromFile(filePath);
        SettingsManager::instance().setValue("General/lastOpenDirectory", QFileInfo(filePath).absolutePath());
    }
}

void MainWindow::showPlaylistMenu()
{
    QMenu menu(this);

    // Save Playlist action
    QAction *saveAction = menu.addAction("Save Playlist...");
    connect(saveAction, &QAction::triggered, this, &MainWindow::savePlaylist);

    // Load Playlist action
    QAction *loadAction = menu.addAction("Load Playlist...");
    connect(loadAction, &QAction::triggered, this, &MainWindow::loadPlaylist);

    // Show menu at button position
    QPoint globalPos = playlistButton->mapToGlobal(QPoint(0, playlistButton->height()));
    menu.exec(globalPos);
}

void MainWindow::onCleanupPlaylist()
{
    if (!playlistRoot) {
        return;
    }

    int removedCount = 0;
    int addedFilesCount = 0;
    int addedFoldersCount = 0;

    // ?뚮줈???쒕씪?대툕(A:\, B:\) 寃쎈줈?몄? ?뺤씤?섍퀬, ?쒕씪?대툕媛 以鍮꾨릺吏 ?딆? 寃쎌슦 嫄대꼫?
    // QFileInfo::exists()媛 誘몄궫???쒕씪?대툕?먯꽌 ??珥덇컙 UI瑜?釉붾줉?섎뒗 臾몄젣 諛⑹?
    auto isPathSafeToCheck = [](const QString& path) -> bool {
        if (path.length() >= 3 && path[1] == ':') {
            wchar_t drivePath[4] = { (wchar_t)path[0].unicode(), L':', L'\\', L'\0' };
            UINT driveType = GetDriveTypeW(drivePath);
            if (driveType == DRIVE_REMOVABLE) {
                // ?뚮줈?????대룞???쒕씪?대툕: 誘몃뵒???쎌엯 ?щ? 鍮좊Ⅴ寃??뺤씤
                HANDLE hDrive = CreateFileW(drivePath, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
                if (hDrive == INVALID_HANDLE_VALUE) return false; // 誘몄궫??
                CloseHandle(hDrive);
            }
        }
        return true;
    };

    // Recursively clean up the playlist tree
    std::function<void(PlaylistTreeNode*)> cleanupNode = [&](PlaylistTreeNode* node) {
        if (!node) return;

        QList<PlaylistTreeNode*> nodesToRemove;

        // Check files and folders in this node
        for (PlaylistTreeNode* child : node->children) {
            if (child->isFolder) {
                // For folders, check if folder still exists
                if (!child->fullPath.isEmpty()) {
                    if (!isPathSafeToCheck(child->fullPath) || !QDir(child->fullPath).exists()) {
                        nodesToRemove.append(child);
                        removedCount++;
                        continue;
                    }
                    // Collect existing items in this folder
                    QStringList existingFiles;
                    QStringList existingFolders;
                    for (PlaylistTreeNode* childNode : child->children) {
                        if (childNode->isFolder) {
                            existingFolders.append(childNode->fullPath);
                        } else {
                            existingFiles.append(childNode->fullPath);
                        }
                    }

                    QDir dir(child->fullPath);
                    QStringList filters;
                    filters << "*.mid" << "*.midi" << "*.nob" << "*.ims" << "*.rol" << "*.sop" << "*.gyb"
                            << "*.oka" << "*.okm" << "*.vgm" << "*.vgz";

                    // Check for new files in current directory
                    QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
                    for (const QFileInfo &fileInfo : files) {
                        QString newFile = fileInfo.absoluteFilePath();
                        if (!existingFiles.contains(newFile)) {
                            PlaylistTreeNode* newNode = new PlaylistTreeNode(
                                QFileInfo(newFile).fileName(),
                                newFile,
                                false,
                                false
                            );
                            newNode->parent = child;
                            child->children.append(newNode);
                            addedFilesCount++;
                        }
                    }

                    // Check for new subdirectories with MIDI/NOB files
                    QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const QFileInfo &dirInfo : dirs) {
                        QString subDirPath = dirInfo.absoluteFilePath();
                        if (!existingFolders.contains(subDirPath)) {
                            // Check if this new folder contains any MIDI/NOB files (recursively)
                            QStringList filesInSubDir = findMidiFilesInDirectory(subDirPath);
                            if (!filesInSubDir.isEmpty()) {
                                // Create new folder node with its structure
                                PlaylistTreeNode* newFolderNode = new PlaylistTreeNode(
                                    dirInfo.fileName(),
                                    subDirPath,
                                    true,
                                    false
                                );
                                newFolderNode->parent = child;
                                child->children.append(newFolderNode);

                                // Add folder structure recursively
                                addFolderStructureToTree(newFolderNode, subDirPath);
                                addedFoldersCount++;
                            }
                        }
                    }

                    // Recursively clean up child folders
                    cleanupNode(child);
                }
            } else {
                // For files, check if file still exists
                if (!child->fullPath.isEmpty()) {
                    if (!isPathSafeToCheck(child->fullPath) || !QFileInfo::exists(child->fullPath)) {
                        nodesToRemove.append(child);
                        removedCount++;
                    }
                }
            }
        }

        // Remove dead nodes
        for (PlaylistTreeNode* nodeToRemove : nodesToRemove) {
            node->children.removeOne(nodeToRemove);
            delete nodeToRemove;
        }
    };

    cleanupNode(playlistRoot);

    // Save the updated playlist immediately (user-initiated action)
    savePlaylistTree();

    // Refresh the UI
    updateUIFromCurrentNode();

    // Show summary
    QString message;
    if (removedCount > 0 || addedFilesCount > 0 || addedFoldersCount > 0) {
        QStringList changes;
        if (removedCount > 0) {
            changes << QString("Removed %1 missing item(s)").arg(removedCount);
        }
        if (addedFoldersCount > 0) {
            changes << QString("Added %1 folder(s)").arg(addedFoldersCount);
        }
        if (addedFilesCount > 0) {
            changes << QString("Added %1 file(s)").arg(addedFilesCount);
        }
        message = QString("Refresh complete:\n- %1").arg(changes.join("\n- "));
    } else {
        message = "Playlist is up to date - no changes needed.";
    }

    QMessageBox::information(this, "Playlist Refresh", message);
}

void MainWindow::onSearchTextChanged()
{
    QString searchText = searchBox->text().trimmed();
    if (playlistProxy) {
        playlistProxy->setFilterFixedString(searchText);
        if (!searchText.isEmpty() && playlistProxy->rowCount() > 0) {
            plSetCurrentRow(0);
        }
    }
}

void MainWindow::addMidiFiles(const QStringList &filePaths)
{
    // Show progress dialog for large operations
    QProgressDialog *progressDialog = nullptr;
    if (filePaths.size() > 5) {
        progressDialog = new QProgressDialog("Processing files...", "Cancel", 0, filePaths.size(), this);
        progressDialog->setWindowModality(Qt::WindowModal);
        progressDialog->show();
    }

    int processed = 0;
    bool playlistModified = false;
    for (const QString &path : filePaths) {
        if (progressDialog && progressDialog->wasCanceled()) {
            break;
        }

        if (progressDialog) {
            progressDialog->setValue(processed);
            progressDialog->setLabelText(QString("Processing: %1").arg(QFileInfo(path).fileName()));
            QApplication::processEvents();
        }

        QFileInfo fileInfo(path);

        if (fileInfo.isFile()) {
            // MIDI ?뚯씪 諛?NOB ?뚯씪 紐⑤몢 異붽? 吏€??
            QString suffix = fileInfo.suffix().toLower();
            if (suffix == "mid" || suffix == "midi" || suffix == "nob" || suffix == "ims" || suffix == "rol" || suffix == "zip" || suffix == "sop" || suffix == "gyb" || suffix == "oka" || suffix == "okm") {
                // Add file to current node in tree
                // addFileToCurrentNode() ?대??먯꽌 updateUIFromCurrentNode()?€ savePlaylistTree()瑜??몄텧?섎?濡?蹂꾨룄 UI 泥섎━ 遺덊븘??
                addFileToCurrentNode(fileInfo.absoluteFilePath());
                playlistModified = true;
            }
        } else if (fileInfo.isDir()) {
            // Add folder to current node with complete structure
            addFolderToCurrentNode(fileInfo.absoluteFilePath());
            playlistModified = true;
        }

        processed++;
    }

    if (progressDialog) {
        progressDialog->setValue(filePaths.size());
        progressDialog->close();
        delete progressDialog;
    }

    // Save playlist and update paths if playlist was modified
    if (playlistModified) {
        // Save playlist immediately
        saveSettings();

        // Update allowed paths based on new playlist structure
        updateAllowedPaths();
    }
}

QStringList MainWindow::findMidiFilesInDirectory(const QString &dirPath)
{
    QStringList midiFiles;
    QDir dir(dirPath);
    QStringList filters;
    filters << "*.mid" << "*.midi" << "*.nob" << "*.ims" << "*.rol" << "*.sop" << "*.gyb" << "*.oka" << "*.okm" << "*.vgm" << "*.vgz";

    // Get files in current directory
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
    for (const QFileInfo &fileInfo : files) {
        midiFiles.append(fileInfo.absoluteFilePath());
    }

    // Recursively search subdirectories
    QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &dirInfo : dirs) {
        QStringList subDirFiles = findMidiFilesInDirectory(dirInfo.absoluteFilePath());
        midiFiles.append(subDirFiles);
    }

    return midiFiles;
}

void MainWindow::addFolderToPlaylist(const QString &folderPath)
{
    QFileInfo folderInfo(folderPath);

    if (!folderInfo.exists() || !folderInfo.isDir()) {
        return;
    }

    if (isInBrowsingMode) {
        // In browsing mode: show folder contents instead of adding folder
        navigateToFolder(folderPath);
        return;
    }

    // Check for duplicates (only in playlist mode)
    if (playlistRoot) {
        bool exists = false;
        if (currentNode) {
            for (PlaylistTreeNode* child : currentNode->children) {
                if (child->fullPath == folderPath) {
                    exists = true;
                    break;
                }
            }
        }
        if (exists) return; // Already exists
    }

    // Add to tree structure and update UI
    addFolderToCurrentNode(folderPath);
}

void MainWindow::navigateToFolder(const QString &folderPath)
{
    if (!QFileInfo(folderPath).exists() || !QFileInfo(folderPath).isDir()) {
        return;
    }

    // Add current path to history if we're navigating away
    if (!currentFolderPath.isEmpty() && currentFolderPath != folderPath) {
        navigationHistory.append(currentFolderPath);
        if (navigationHistory.size() > 10) { // Limit history size
            navigationHistory.removeFirst();
        }
    }

    currentFolderPath = folderPath;
    isInBrowsingMode = true; // Switch to browsing mode
    navigateToFolderWithoutHistory(folderPath);
}

void MainWindow::navigateToFolderWithoutHistory(const QString &folderPath)
{
    if (!QFileInfo(folderPath).exists() || !QFileInfo(folderPath).isDir()) {
        return;
    }

    QVector<PlaylistRow> rows;
    QDir dir(folderPath);

    // Check if current folder is a playlist root folder
    bool isPlaylistFolder = false;
    if (isInBrowsingMode) {
        SettingsManager& settings = SettingsManager::instance();
        int playlistSize = settings.beginReadArray("playlist");
        for (int i = 0; i < playlistSize; ++i) {
            settings.setArrayIndex(i);
            QString playlistPath = settings.value("filePath").toString();
            int itemType = settings.value("itemType", MIDI_FILE).toInt();

            if (itemType == FOLDER && playlistPath == folderPath) {
                isPlaylistFolder = true;
                break;
            }
        }
        settings.endArray();
    }

    if (isPlaylistFolder) {
        rows.append(PlaylistRow("📁 .. (Parent Folder)", PLAYLIST_ROOT, PARENT_FOLDER));
    } else if (dir.cdUp()) {
        QString parentPath = dir.absolutePath();
        if (isPathAllowed(parentPath)) {
            rows.append(PlaylistRow("📁 .. (Parent Folder)", parentPath, PARENT_FOLDER));
        }
    }

    // Add subfolders
    dir.setPath(folderPath);
    QFileInfoList subFolders = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &folderInfo : subFolders) {
        rows.append(PlaylistRow("📁 " + folderInfo.fileName(), folderInfo.absoluteFilePath(), FOLDER));
    }

    // Add MIDI files
    QStringList filters;
    filters << "*.mid" << "*.midi" << "*.nob" << "*.ims" << "*.rol" << "*.sop" << "*.gyb" << "*.oka" << "*.okm" << "*.vgm" << "*.vgz";
    QFileInfoList midiFiles = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const QFileInfo &fileInfo : midiFiles) {
        rows.append(PlaylistRow("🎵" + fileInfo.fileName(), fileInfo.absoluteFilePath(), MIDI_FILE));
    }

    if (playlistModel) {
        playlistModel->setRows(std::move(rows));
    }

    // Update window title to show current path
    updateWindowTitle();

    // Save current state
    saveSettings();
}

void MainWindow::handleFolderDoubleClick(const QString &folderPath)
{
    // Handle Parent Folder navigation
    if (folderPath == "__PARENT__") {
        if (currentNode && currentNode->parent) {
            navigateToNode(currentNode->parent);
        }
        return;
    }

    // Find the node corresponding to this path
    PlaylistTreeNode* targetNode = nullptr;

    if (currentNode) {
        // Search in current node's children
        for (auto* child : currentNode->children) {
            // Match against the same data that was stored in UserRole
            QString storedData = child->fullPath.isEmpty() ? child->name : child->fullPath;

            if (storedData == folderPath) {
                targetNode = child;
                break;
            }
        }
    }

    // Navigate to the found node if it's a folder
    if (targetNode && targetNode->isFolder) {
        navigateToNode(targetNode);
    }
}

QString MainWindow::getCurrentPath() const
{
    return currentFolderPath;
}

void MainWindow::setCurrentPath(const QString &path)
{
    currentFolderPath = path;
}

void MainWindow::updateAllowedPaths()
{
    allowedPaths.clear();

    // Collect all playlist folder paths
    QStringList playlistFolders;
    for (int i = 0; i < plCount(); ++i) {
        int itemType = plRowType(i);

        if (itemType == FOLDER) {
            QString folderPath = plRowPath(i);
            playlistFolders.append(folderPath);
        }
    }

    if (playlistFolders.isEmpty()) {
        return;
    }

    // Build complete playlist tree structure
    QSet<QString> allAllowedPaths;

    // Step 1: Add all playlist folders and their subdirectories
    for (const QString &folderPath : playlistFolders) {
        allAllowedPaths.insert(folderPath);

        // Add all subdirectories
        QDirIterator it(folderPath, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            allAllowedPaths.insert(it.next());
        }
    }

    // Step 2: Find the minimal tree that connects all playlist folders
    // Add parent paths only if they are needed to connect playlist folders
    QSet<QString> necessaryParents;

    for (const QString &folderPath : playlistFolders) {
        QDir dir(folderPath);
        while (dir.cdUp()) {
            QString parentPath = dir.absolutePath();

            // Check how many playlist folders are under this parent
            int foldersUnderParent = 0;
            for (const QString &otherFolder : playlistFolders) {
                if (otherFolder.startsWith(parentPath + QDir::separator()) || otherFolder == parentPath) {
                    foldersUnderParent++;
                }
            }

            // Include this parent if it contains playlist folders
            if (foldersUnderParent > 0) {
                necessaryParents.insert(parentPath);
            }

            // Stop if this parent contains ALL playlist folders (found common root)
            if (foldersUnderParent == playlistFolders.size()) {
                break;
            }
        }
    }

    // Add all necessary parent paths
    for (const QString &parentPath : necessaryParents) {
        allAllowedPaths.insert(parentPath);
    }

    // Convert to QStringList
    allowedPaths.clear();
    for (const QString &path : allAllowedPaths) {
        allowedPaths.append(path);
    }
}

bool MainWindow::isPathAllowed(const QString &path) const
{
    // Always allow playlist root
    if (path == PLAYLIST_ROOT) {
        return true;
    }

    if (allowedPaths.isEmpty()) {
        return true; // If no restrictions, allow all paths
    }

    return allowedPaths.contains(path);
}

void MainWindow::navigateToNode(PlaylistTreeNode* node)
{
    if (!node) return;

    currentNode = node;
    isInBrowsingMode = (node != playlistRoot);

    // Update UI
    updateUIFromCurrentNode();

    // Update window title
    updateWindowTitle();

    // 무겁게 전체 트리를 다시 직렬화해 저장할 필요 없이, 현재 포커스 경로만 경량 업데이트하여 렉을 제거합니다.
    SettingsManager& settings = SettingsManager::instance();
    // Sentinel so "at root" is remembered too (empty used to be ambiguous and
    // fell back to stale paths on restore).
    QString currentPath = PLAYLIST_ROOT;
    if (currentNode && currentNode != playlistRoot) {
        currentPath = currentNode->fullPath;
    }
    settings.setValue("currentNodePath", currentPath);
}

void MainWindow::updateUIFromCurrentNode()
{
    try {
        // Ensure we have valid objects
        if (!fileList || !playlistModel || !currentNode) {
            return;
        }

        // First, validate and clean the current node to remove invalid entries
        validateAndCleanPlaylistTree(currentNode);

        QVector<PlaylistRow> rows;

        // Add Parent Folder if not at root
        if (currentNode != playlistRoot && currentNode->parent) {
            rows.append(PlaylistRow("📁 .. (Parent Folder)", "__PARENT__", PARENT_FOLDER));
        }

        // Separate folders and files for sorted display
        QList<PlaylistTreeNode*> folders;
        QList<PlaylistTreeNode*> files;

        for (auto* child : currentNode->children) {
            if (!child) continue;
            if (child->isFolder) {
                folders.append(child);
            } else {
                files.append(child);
            }
        }

        // Sort folders and files alphabetically
        std::sort(folders.begin(), folders.end(), [](PlaylistTreeNode* a, PlaylistTreeNode* b) {
            return a->name < b->name;
        });
        std::sort(files.begin(), files.end(), [](PlaylistTreeNode* a, PlaylistTreeNode* b) {
            return a->name < b->name;
        });

        rows.reserve(rows.size() + folders.size() + files.size());

        // Folders first, then files. No item cap: the model/view renders only
        // visible delegates, so even 100k+ rows stay responsive.
        for (auto* child : folders) {
            QString displayName = child->name;
            if (!displayName.startsWith("📁 ") && !displayName.startsWith("📦 ")) {
                displayName = "📁 " + displayName;
            }
            rows.append(PlaylistRow(displayName,
                                    child->fullPath.isEmpty() ? child->name : child->fullPath,
                                    FOLDER));
        }
        for (auto* child : files) {
            rows.append(PlaylistRow(child->name,
                                    child->fullPath.isEmpty() ? child->name : child->fullPath,
                                    MIDI_FILE));
        }

        playlistModel->setRows(std::move(rows));
    } catch (...) {
        qWarning() << "[Playlist] updateUI failed — falling back to minimal state";
        // If the entire function fails, ensure we have a minimal working state
        if (playlistModel) {
            try { playlistModel->clearRows(); } catch (...) {}
        }
    }
}

void MainWindow::initializePlaylistTree()
{
    // Delete existing tree if any
    if (playlistRoot) {
        delete playlistRoot;
        playlistRoot = nullptr;
        currentNode = nullptr;
    }

    // Create virtual root node
    playlistRoot = new PlaylistTreeNode("Playlist Root", "", true, true);
    currentNode = playlistRoot;

    // Reset browsing mode
    isInBrowsingMode = false;

    // Update UI to show empty playlist
    updateUIFromCurrentNode();
}

void MainWindow::addFileToCurrentNode(const QString &filePath)
{
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) return;

    addFileToCurrentNodeWithoutSave(filePath);

    // Update UI and save
    updateUIFromCurrentNode();
    triggerSavePlaylistTree();
}

void MainWindow::addFolderToCurrentNode(const QString &folderPath)
{
    QFileInfo folderInfo(folderPath);
    if (!folderInfo.exists() || !folderInfo.isDir()) return;

    // Check for duplicates in current node
    for (auto* child : currentNode->children) {
        if (child->isFolder && child->fullPath == folderPath) {
            return; // Already exists
        }
    }

    // Show a temporary loading node or status
    PlaylistTreeNode* loadingNode = new PlaylistTreeNode(
        "⏳  " + folderInfo.fileName(), folderPath, true, false);
    loadingNode->parent = currentNode;
    currentNode->children.append(loadingNode);
    updateUIFromCurrentNode();

    // Start background scanner
    FolderScanner* scanner = new FolderScanner(folderPath, this);
    connect(scanner, &FolderScanner::scanFinished, this, [this, loadingNode](FolderScanner* s) {
        PlaylistTreeNode* resultNode = s->getResultNode();
        
        // Remove loading node
        for (int i = 0; i < currentNode->children.size(); ++i) {
            if (currentNode->children[i] == loadingNode) {
                currentNode->children.removeAt(i);
                delete loadingNode;
                break;
            }
        }
        
        if (resultNode) {
            resultNode->parent = currentNode;
            currentNode->children.append(resultNode);
        }
        
        updateUIFromCurrentNode();
        triggerSavePlaylistTree();
        
        s->deleteLater();
    });
    
    scanner->start();
}

void MainWindow::addFileToCurrentNodeWithoutSave(const QString &filePath)
{
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) return;

    if (fileInfo.fileName().toLower().endsWith(".zip")) {
        for (auto* child : currentNode->children) {
            if (child->isFolder && child->fullPath == filePath) {
                return;
            }
        }

        PlaylistTreeNode* zipNode = new PlaylistTreeNode(
            "📦 " + fileInfo.fileName(), filePath, true, false);
        zipNode->parent = currentNode;

        QZipReader zip(filePath);
        if (zip.status() == QZipReader::NoError) {
            QList<QZipReader::FileInfo> entries = zip.fileInfoList();
            for (const auto& entry : entries) {
                if (!entry.isDir) {
                    QString innerPath = entry.filePath;
                    QFileInfo innerInfo(innerPath);
                    QString suffix = innerInfo.suffix().toLower();
                    if (suffix == "mid" || suffix == "midi" || suffix == "nob" || suffix == "ims" || suffix == "rol" || suffix == "sop" || suffix == "gyb" || suffix == "oka" || suffix == "okm") {
                        QString title;
                        QByteArray innerData = zip.fileData(innerPath);
                        if (suffix == "nob") {
                            title = NobFileHandler::extractTitleFromHeader(innerData);
                        } else if (suffix == "gyb") {
                            title = GybFileHandler::extractTitle(innerData);
                        } else if (suffix == "ims") {
                            title = ImsPlayer::extractTitleQuick(innerData, ".ims");
                        } else if (suffix == "oka" || suffix == "okm") {
                            title = OkaFileHandler::extractTitle(innerData);
                        }

                        QString displayName = "🎵 " + innerInfo.fileName();
                        if (!title.isEmpty()) {
                            displayName += " - " + title;
                        }
                        QString virtualPath = QString("%1::%2").arg(filePath, innerPath);

                        PlaylistTreeNode* childNode = new PlaylistTreeNode(
                            displayName, virtualPath, false, false);
                        childNode->parent = zipNode;
                        zipNode->children.append(childNode);
                    }
                }
            }
        }

        if (!zipNode->children.isEmpty()) {
            currentNode->children.append(zipNode);
        } else {
            delete zipNode;
        }
        return;
    }

    // Check for duplicates in current node
    for (auto* child : currentNode->children) {
        if (!child->isFolder && child->fullPath == filePath) {
            return; // Already exists
        }
    }

    QString displayName = "♫ " + fileInfo.fileName();
    if (fileInfo.fileName().toLower().endsWith(".nob")) {
        QString lstTitle = NobFileHandler::extractTitleFromLst(filePath);
        if (!lstTitle.isEmpty()) {
            displayName += " - " + lstTitle;
        } else {
            QString nobTitle = NobFileHandler::extractTitle(filePath);
            if (!nobTitle.isEmpty()) {
                displayName += " - " + nobTitle;
            }
        }
    } else if (fileInfo.fileName().toLower().endsWith(".gyb")) {
        QString gybTitle = GybFileHandler::extractTitle(filePath);
        if (!gybTitle.isEmpty()) {
            displayName += " - " + gybTitle;
        }
    } else if (isOkaFile(filePath)) {
        QString okaTitle = OkaFileHandler::extractTitle(filePath);
        if (!okaTitle.isEmpty()) {
            displayName += " - " + okaTitle;
        }
    } else if (isOplFile(filePath)) {
        QString imsTitle = ImsPlayer::extractTitleQuick(filePath);
        if (!imsTitle.isEmpty()) {
            displayName += " - " + imsTitle;
        }
    }

    PlaylistTreeNode* fileNode = new PlaylistTreeNode(
        displayName, filePath, false, false);
    fileNode->parent = currentNode;
    currentNode->children.append(fileNode);
}

void MainWindow::addFolderToCurrentNodeWithoutSave(const QString &folderPath)
{
    QFileInfo folderInfo(folderPath);
    if (!folderInfo.exists() || !folderInfo.isDir()) return;

    // Check for duplicates in current node
    for (auto* child : currentNode->children) {
        if (child->isFolder && child->fullPath == folderPath) {
            return; // Already exists
        }
    }

    // Show a temporary loading node or status
    PlaylistTreeNode* loadingNode = new PlaylistTreeNode(
        "⏳  " + folderInfo.fileName(), folderPath, true, false);
    loadingNode->parent = currentNode;
    currentNode->children.append(loadingNode);
    updateUIFromCurrentNode();

    // Start background scanner
    FolderScanner* scanner = new FolderScanner(folderPath, this);
    // Fix: Use parentNode captured at start to ensure correct removal even if user navigates away
    PlaylistTreeNode* targetParent = currentNode;
    connect(scanner, &FolderScanner::scanFinished, this, [this, loadingNode, targetParent](FolderScanner* s) {
        PlaylistTreeNode* resultNode = s->getResultNode();
        
        // Remove loading node from the correct parent
        for (int i = 0; i < targetParent->children.size(); ++i) {
            if (targetParent->children[i] == loadingNode) {
                targetParent->children.removeAt(i);
                delete loadingNode;
                break;
            }
        }
        
        if (resultNode) {
            resultNode->parent = targetParent;
            targetParent->children.append(resultNode);
        }
        
        updateUIFromCurrentNode();
        s->deleteLater();
    });
    
    scanner->start();
}

void MainWindow::savePlaylistTree()
{
    // Get JMPLAYER directory path
    QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString jmplayerDir = documentsPath + "/JMPLAYER";
    QString playlistPath = jmplayerDir + "/playlist.json";

    // Create directory if it doesn't exist
    QDir dir;
    if (!dir.exists(jmplayerDir)) {
        dir.mkpath(jmplayerDir);
    }

    // Save playlist to separate JSON file
    if (playlistRoot) {
        QJsonObject rootJson = nodeToJson(playlistRoot);

        // Add current node path to JSON (sentinel = remember root position too)
        QString currentPath = PLAYLIST_ROOT;
        if (currentNode && currentNode != playlistRoot) {
            currentPath = currentNode->fullPath;
        }
        rootJson["currentNodePath"] = currentPath;

        QJsonDocument doc(rootJson);

        QFile file(playlistPath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(doc.toJson(QJsonDocument::Compact));
            file.close();
        }
    }

    // Only save current node path to INI for compatibility
    SettingsManager& settings = SettingsManager::instance();
    // Sentinel so "at root" is remembered too (empty used to be ambiguous and
    // fell back to stale paths on restore).
    QString currentPath = PLAYLIST_ROOT;
    if (currentNode && currentNode != playlistRoot) {
        currentPath = currentNode->fullPath;
    }
    settings.setValue("currentNodePath", currentPath);

    // Clear old formats from INI (for compatibility)
    settings.remove("playlistTree");
    settings.remove("playlistTreeJson");
}

void MainWindow::triggerSavePlaylistTree()
{
    if (playlistSaveTimer) {
        playlistSaveTimer->start(2000); // Debounce save by 2 seconds
    } else {
        savePlaylistTree();
    }
}

void MainWindow::loadPlaylistTree()
{
    // Get JMPLAYER directory path
    QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString jmplayerDir = documentsPath + "/JMPLAYER";
    QString playlistPath = jmplayerDir + "/playlist.json";

    // Initialize clean tree first (ensure we always have a valid tree)
    initializePlaylistTree();

    try {
        // Try to load from separate JSON file first
        QFile file(playlistPath);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray jsonData = file.readAll();
            file.close();

            if (!jsonData.isEmpty()) {
                QJsonDocument doc = QJsonDocument::fromJson(jsonData);
                if (!doc.isNull() && doc.isObject()) {
                    QJsonObject rootJson = doc.object();

                    // Create new tree from JSON (keep original as backup)
                    PlaylistTreeNode* tempRoot = nullptr;
                    try {
                        tempRoot = nodeFromJson(rootJson, nullptr);
                        if (tempRoot) {
                            // Only replace if successful
                            delete playlistRoot;
                            playlistRoot = tempRoot;
                            currentNode = playlistRoot;

                            // Validate and clean the loaded tree AFTER setting as current
                            validateAndCleanPlaylistTree(playlistRoot);

                            // Restore current node position. navigateToNode()
                            // lightweight-saves the focused folder path to the
                            // INI on every navigation (the playlist JSON is only
                            // rewritten on tree edits), so the INI is the fresh,
                            // authoritative source — read it first and fall back
                            // to the JSON value for older saves. Without this the
                            // last-browsed folder (and thus the remembered track)
                            // was lost on restart, always reopening at root.
                            SettingsManager& nodeSettings = SettingsManager::instance();
                            QString currentPath = nodeSettings.value("currentNodePath", "").toString();
                            if (currentPath.isEmpty()) {
                                currentPath = rootJson.value("currentNodePath").toString();
                            }
                            if (currentPath == PLAYLIST_ROOT || currentPath.isEmpty()) {
                                // Root was the last position (sentinel), or nothing
                                // saved — start at the playlist root.
                                currentNode = playlistRoot;
                                isInBrowsingMode = false;
                            } else {
                                PlaylistTreeNode* node = findNodeByPath(currentPath);
                                if (node) {
                                    currentNode = node;
                                    isInBrowsingMode = (node != playlistRoot);
                                } else {
                                    // Saved position no longer exists — start at root
                                    currentNode = playlistRoot;
                                    isInBrowsingMode = false;
                                }
                            }

                            updateUIFromCurrentNode();
                            return;
                        }
                    } catch (...) {
                        qWarning() << "[Playlist] JSON tree load failed — using fallback";
                        // If loading failed, clean up temp root and continue with fallback
                        if (tempRoot) {
                            delete tempRoot;
                        }
                    }
                }
            }
        }
    } catch (...) {
        qWarning() << "[Playlist] tree load threw — reinitializing clean tree";
        // If any exception occurs, ensure we have a clean tree
        if (!playlistRoot) {
            initializePlaylistTree();
        }
    }

    // Fallback: Try to load from INI settings (legacy support)
    try {
        SettingsManager& settings = SettingsManager::instance();

        // Try legacy JSON format in INI
        QByteArray jsonData = settings.value("playlistTreeJson").toByteArray();
        if (!jsonData.isEmpty()) {
            QJsonDocument doc = QJsonDocument::fromJson(jsonData);
            if (!doc.isNull() && doc.isObject()) {
                // Load from legacy JSON format in INI
                QJsonObject rootJson = doc.object();

                // Create new tree from JSON (keep original as backup)
                PlaylistTreeNode* tempRoot = nullptr;
                try {
                    tempRoot = nodeFromJson(rootJson, nullptr);
                    if (tempRoot) {
                        // Only replace if successful
                        delete playlistRoot;
                        playlistRoot = tempRoot;
                        currentNode = playlistRoot;

                        // Validate and clean the loaded tree AFTER setting as current
                        validateAndCleanPlaylistTree(playlistRoot);

                        // Restore current node position (after validation)
                        QString currentPath = settings.value("currentNodePath").toString();
                        if (currentPath == PLAYLIST_ROOT || currentPath.isEmpty()) {
                            // Root remembered (sentinel) or nothing saved — start at root
                            currentNode = playlistRoot;
                            isInBrowsingMode = false;
                        } else {
                            PlaylistTreeNode* node = findNodeByPath(currentPath);
                            if (node) {
                                currentNode = node;
                                isInBrowsingMode = (node != playlistRoot);
                            } else {
                                // Saved position no longer exists — start at root
                                currentNode = playlistRoot;
                                isInBrowsingMode = false;
                            }
                        }

                        updateUIFromCurrentNode();

                        // Migrate to new separate JSON file format
                        savePlaylistTree();
                        return;
                    }
                } catch (...) {
                    qWarning() << "[Playlist] legacy tree load failed — using fallback";
                    // If loading failed, clean up temp root and continue with fallback
                    if (tempRoot) {
                        delete tempRoot;
                    }
                }
            }
        }
    } catch (...) {
        qWarning() << "[Playlist] tree load threw — reinitializing clean tree";
        // If any exception occurs, ensure we have a clean tree
        if (!playlistRoot) {
            initializePlaylistTree();
        }
    }

    // Fallback to old array format for compatibility
    try {
        SettingsManager& settings = SettingsManager::instance();
        int size = settings.beginReadArray("playlistTree");
        for (int i = 0; i < size; ++i) {
            try {
                settings.setArrayIndex(i);
                QString name = settings.value("name").toString();
                QString path = settings.value("path").toString();
                bool isFolder = settings.value("isFolder").toBool();

                // Verify path still exists
                if (QFileInfo::exists(path)) {
                    if (isFolder) {
                        // Add folder without calling save (to avoid recursion)
                        addFolderToCurrentNodeWithoutSave(path);
                    } else {
                        // Add file without calling save (to avoid recursion)
                        addFileToCurrentNodeWithoutSave(path);
                    }
                }
            } catch (...) {
                qWarning() << "[Playlist] skipping invalid saved entry";
                // Skip invalid entries and continue
                continue;
            }
        }
        settings.endArray();

        // Restore current node position
        try {
            QString currentPath = settings.value("currentNodePath").toString();
            if (currentPath == PLAYLIST_ROOT || currentPath.isEmpty()) {
                currentNode = playlistRoot;
                isInBrowsingMode = false;
            } else {
                PlaylistTreeNode* node = findNodeByPath(currentPath);
                if (node) {
                    currentNode = node;
                    isInBrowsingMode = (node != playlistRoot);
                } else {
                    // Saved position no longer exists — start at root
                    currentNode = playlistRoot;
                    isInBrowsingMode = false;
                }
            }
        } catch (...) {
            qWarning() << "[Playlist] currentPath restore failed — staying at root";
            // If currentPath restoration fails, stay at root
            currentNode = playlistRoot;
            isInBrowsingMode = false;
        }

        // If we loaded from legacy format, migrate to new format
        if (size > 0) {
            try {
                savePlaylistTree();
            } catch (...) {
                qWarning() << "[Playlist] save during migration failed (ignored)";
                // Ignore save errors during migration
            }
        }
    } catch (...) {
        qWarning() << "[Playlist] settings-array read failed — reinitializing";
        // If array reading fails completely, ensure we have a clean state
        if (!playlistRoot) {
            initializePlaylistTree();
        }
    }

    // Final validation and cleanup for all formats (with safety check)
    try {
        if (playlistRoot) {
            validateAndCleanPlaylistTree(playlistRoot);
        }
    } catch (...) {
        qWarning() << "[Playlist] tree validation failed — reinitializing";
        // If validation fails, reinitialize clean tree
        initializePlaylistTree();
    }

    // Ensure UI update (with safety check)
    try {
        updateUIFromCurrentNode();
    } catch (...) {
        qWarning() << "[Playlist] post-load UI update failed";
        // If UI update fails, ensure we have valid state
        if (!currentNode) {
            currentNode = playlistRoot;
            isInBrowsingMode = false;
        }
    }

    // Check for first run samples
    SettingsManager& settings = SettingsManager::instance();
    bool playlistJsonExists = QFile::exists(playlistPath);

    if (!playlistJsonExists) {
        QString samplePath = QApplication::applicationDirPath() + "/BK";
        if (QFileInfo::exists(samplePath) && QFileInfo(samplePath).isDir()) {
            // Add synchronously to root for first run
            PlaylistTreeNode* bkNode = new PlaylistTreeNode(LSTR("병코돌고래", "BK Dolphin"), samplePath, true, false);
            bkNode->parent = playlistRoot;
            playlistRoot->children.append(bkNode);
            
            // Helper to add files synchronously
            std::function<void(PlaylistTreeNode*, const QString&)> addSync = [&](PlaylistTreeNode* parent, const QString& path) {
                QDir dir(path);
                QStringList filters;
                filters << "*.mid" << "*.midi" << "*.nob" << "*.ims" << "*.rol" << "*.sop" << "*.gyb" << "*.oka" << "*.okm" << "*.vgm" << "*.vgz";
                QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
                for (const QFileInfo &fi : files) {
                    QString disp = "🎵 " + fi.fileName();
                    if (fi.fileName().toLower().endsWith(".nob")) {
                        QString title = NobFileHandler::extractTitleFromLst(fi.absoluteFilePath());
                        if (!title.isEmpty()) {
                            disp += " - " + title;
                        } else {
                            QString nobTitle = NobFileHandler::extractTitle(fi.absoluteFilePath());
                            if (!nobTitle.isEmpty()) {
                                disp += " - " + nobTitle;
                            }
                        }
                    } else if (isOplFile(fi.absoluteFilePath())) {
                        QString title = ImsPlayer::extractTitleQuick(fi.absoluteFilePath());
                        if (!title.isEmpty()) disp += " - " + title;
                    }
                    PlaylistTreeNode* fn = new PlaylistTreeNode(disp, fi.absoluteFilePath(), false, false);
                    fn->parent = parent;
                    parent->children.append(fn);
                }
                QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
                for (const QFileInfo &di : dirs) {
                    PlaylistTreeNode* dn = new PlaylistTreeNode("📁 " + di.fileName(), di.absoluteFilePath(), true, false);
                    dn->parent = parent;
                    parent->children.append(dn);
                    addSync(dn, di.absoluteFilePath());
                }
            };
            
            addSync(bkNode, samplePath);
            settings.setValue("FirstRunSamplesLoaded", true);
            triggerSavePlaylistTree(); 
        }
    }

    // Migration: Rename existing BK nodes to "📁 병코돌고래"
    std::function<void(PlaylistTreeNode*)> migrateBkName = [&](PlaylistTreeNode* node) {
        if (!node) return;
        if (node->isFolder && (node->name.endsWith("BK") || node->name.contains("📁"))) {
            node->name = LSTR("📁 병코돌고래", "📁 BK Dolphin");
        }
        for (auto* child : node->children) {
            migrateBkName(child);
        }
    };
    if (playlistRoot) {
        migrateBkName(playlistRoot);
        updateUIFromCurrentNode();
    }
}

PlaylistTreeNode* MainWindow::findNodeByPath(const QString &path)
{
    // Recursive search function
    std::function<PlaylistTreeNode*(PlaylistTreeNode*)> search = [&](PlaylistTreeNode* node) -> PlaylistTreeNode* {
        if (node->fullPath == path) return node;
        for (auto* child : node->children) {
            auto* result = search(child);
            if (result) return result;
        }
        return nullptr;
    };

    return search(playlistRoot);
}

void MainWindow::addFolderStructureToTree(PlaylistTreeNode* parentNode, const QString &folderPath)
{
    QDir dir(folderPath);

    // Add MIDI files
    QStringList filters;
    filters << "*.mid" << "*.midi" << "*.nob" << "*.ims" << "*.rol" << "*.sop" << "*.gyb" << "*.oka" << "*.okm" << "*.vgm" << "*.vgz";
    QFileInfoList midiFiles = dir.entryInfoList(filters, QDir::Files);
    for (const QFileInfo &fileInfo : midiFiles) {
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
        QString folderDispName = folderInfo.fileName();
        if (folderDispName.toUpper() == "BK") folderDispName = LSTR("병코돌고래", "BK Dolphin");
        PlaylistTreeNode* folderNode = new PlaylistTreeNode(
            "📁 " + folderDispName, folderInfo.absoluteFilePath(), true, false);
        folderNode->parent = parentNode;
        parentNode->children.append(folderNode);

        // Recursively add structure
        addFolderStructureToTree(folderNode, folderInfo.absoluteFilePath());
    }
}

void MainWindow::removeItemFromCurrentNode(const QString &itemPath)
{
    if (!currentNode) return;

    // Find and remove the item from current node's children
    for (int i = 0; i < currentNode->children.size(); ++i) {
        PlaylistTreeNode* child = currentNode->children[i];

        // Match using the same logic as in updateUIFromCurrentNode
        QString storedData = child->fullPath.isEmpty() ? child->name : child->fullPath;

        if (storedData == itemPath) {
            // Remove from children list
            currentNode->children.removeAt(i);
            // Delete the node and all its children
            delete child;
            break;
        }
    }
}

void MainWindow::savePlaylistToFile(const QString &filePath)
{
    QJsonObject rootJson;
    rootJson["version"] = "1.0";
    rootJson["application"] = "JJoMe MIDI Player";

    // Convert tree to JSON
    QJsonArray childrenArray;
    for (auto* child : playlistRoot->children) {
        childrenArray.append(nodeToJson(child));
    }
    rootJson["playlist"] = childrenArray;

    // Save current node path
    // Sentinel so "at root" is remembered too (empty used to be ambiguous and
    // fell back to stale paths on restore).
    QString currentPath = PLAYLIST_ROOT;
    if (currentNode && currentNode != playlistRoot) {
        currentPath = currentNode->fullPath;
    }
    rootJson["currentPath"] = currentPath;

    // Write to file
    QJsonDocument doc(rootJson);
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson());
        file.close();

        // Update window title to show saved file
        updateWindowTitle();
    } else {
        QMessageBox::warning(this, "Save Error", "Could not save playlist file!");
    }
}

void MainWindow::loadPlaylistFromFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Load Error", "Could not open playlist file!");
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (doc.isNull() || !doc.isObject()) {
        QMessageBox::warning(this, "Load Error", "Invalid playlist file format!");
        return;
    }

    QJsonObject rootJson = doc.object();

    // Clear current playlist safely
    if (playlistRoot) {
        delete playlistRoot;
        playlistRoot = nullptr;
        currentNode = nullptr;
    }
    initializePlaylistTree();

    // Load playlist from JSON
    if (rootJson.contains("playlist") && rootJson["playlist"].isArray()) {
        QJsonArray childrenArray = rootJson["playlist"].toArray();
        for (const QJsonValue &value : childrenArray) {
            if (value.isObject()) {
                PlaylistTreeNode* child = nodeFromJson(value.toObject(), playlistRoot);
                if (child) {
                    playlistRoot->children.append(child);
                }
            }
        }
    }

    // Restore current node position
    QString currentPath = rootJson["currentPath"].toString();
    if (!currentPath.isEmpty()) {
        PlaylistTreeNode* node = findNodeByPath(currentPath);
        if (node) {
            currentNode = node;
            isInBrowsingMode = (node != playlistRoot);
        }
    }

    // Update UI
    updateUIFromCurrentNode();

    // Save to settings (replace current settings)
    triggerSavePlaylistTree();

    // Update window title
    updateWindowTitle();
}

QJsonObject MainWindow::nodeToJson(PlaylistTreeNode* node)
{
    QJsonObject nodeJson;
    nodeJson["name"] = node->name;
    nodeJson["fullPath"] = node->fullPath;
    nodeJson["isFolder"] = node->isFolder;
    nodeJson["isVirtual"] = node->isVirtual;

    // Convert children
    QJsonArray childrenArray;
    for (auto* child : node->children) {
        childrenArray.append(nodeToJson(child));
    }
    nodeJson["children"] = childrenArray;

    return nodeJson;
}

PlaylistTreeNode* MainWindow::nodeFromJson(const QJsonObject &json, PlaylistTreeNode* parent)
{
    QString name = json["name"].toString();
    QString fullPath = json["fullPath"].toString();
    bool isFolder = json["isFolder"].toBool();
    bool isVirtual = json["isVirtual"].toBool();

    // If it is a zip file and exists on disk, reconstruct it as a virtual folder structure
    if (fullPath.toLower().endsWith(".zip")) {
        QFileInfo fileInfo(fullPath);
        if (fileInfo.exists()) {
            PlaylistTreeNode* zipNode = new PlaylistTreeNode(
                "📦 " + fileInfo.fileName(), fullPath, true, false);
            zipNode->parent = parent;

            QZipReader zip(fullPath);
            if (zip.status() == QZipReader::NoError) {
                QList<QZipReader::FileInfo> entries = zip.fileInfoList();
                for (const auto& entry : entries) {
                    if (!entry.isDir) {
                        QString innerPath = entry.filePath;
                        QFileInfo innerInfo(innerPath);
                        QString suffix = innerInfo.suffix().toLower();
                        if (suffix == "mid" || suffix == "midi" || suffix == "nob" || suffix == "ims" || suffix == "rol" || suffix == "sop" || suffix == "gyb" || suffix == "oka" || suffix == "okm") {
                            QString title;
                            QByteArray innerData = zip.fileData(innerPath);
                            if (suffix == "nob") {
                                title = NobFileHandler::extractTitleFromHeader(innerData);
                            } else if (suffix == "gyb") {
                                title = GybFileHandler::extractTitle(innerData);
                            } else if (suffix == "ims") {
                                title = ImsPlayer::extractTitleQuick(innerData, ".ims");
                            } else if (suffix == "oka" || suffix == "okm") {
                                title = OkaFileHandler::extractTitle(innerData);
                            }

                            QString displayName = "🎵 " + innerInfo.fileName();
                            if (!title.isEmpty()) {
                                displayName += " - " + title;
                            }
                            QString virtualPath = QString("%1::%2").arg(fullPath, innerPath);

                            PlaylistTreeNode* childNode = new PlaylistTreeNode(
                                displayName, virtualPath, false, false);
                            childNode->parent = zipNode;
                            zipNode->children.append(childNode);
                        }
                    }
                }
            }
            return zipNode;
        }
    }

    PlaylistTreeNode* node = new PlaylistTreeNode(name, fullPath, isFolder, isVirtual);
    node->parent = parent;

    // Load children
    if (json.contains("children") && json["children"].isArray()) {
        QJsonArray childrenArray = json["children"].toArray();
        for (const QJsonValue &value : childrenArray) {
            if (value.isObject()) {
                PlaylistTreeNode* child = nodeFromJson(value.toObject(), node);
                if (child) {
                    node->children.append(child);
                }
            }
        }
    }

    return node;
}

void MainWindow::validateAndCleanPlaylistTree(PlaylistTreeNode* node)
{
    if (!node) return;

    try {
        QList<PlaylistTreeNode*> childrenToRemove;
        bool hasChanges = false;

        // Check all children safely for nullptr and recurse
        for (PlaylistTreeNode* child : node->children) {
            if (!child) {
                childrenToRemove.append(child);
                hasChanges = true;
                continue;
            }

            try {
                // Recursively validate child nodes (only memory integrity checks, no disk I/O to avoid freezes)
                validateAndCleanPlaylistTree(child);
            } catch (...) {
                qWarning() << "[Playlist] invalid child during validation — removing";
                childrenToRemove.append(child);
                hasChanges = true;
            }
        }

        // Remove invalid children safely
        for (PlaylistTreeNode* childToRemove : childrenToRemove) {
            try {
                node->children.removeAll(childToRemove);
                if (childToRemove) {
                    delete childToRemove;
                }
            } catch (...) {
                qWarning() << "[Playlist] child deletion failed (continuing)";
                // Continue even if deletion fails
            }
        }

        // Save playlist if changes were made (only at root level to avoid multiple saves)
        if (hasChanges && node == playlistRoot) {
            try {
                triggerSavePlaylistTree();
            } catch (...) {
                qWarning() << "[Playlist] save during validation cleanup failed (ignored)";
                // Ignore save errors during validation cleanup
            }
        }
    } catch (...) {
        qWarning() << "[Playlist] validation process threw — keeping node integrity only";
        // If the entire validation process fails, at least ensure node integrity
        if (node && node->children.isEmpty() && node->isVirtual) {
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Playlist view bridge helpers. They operate on PROXY (visible) row indices,
// which mirror the old QListWidget row semantics so existing call sites stay
// 1:1. When no search filter is active, proxy row == source row.
// ---------------------------------------------------------------------------
int MainWindow::plCount() const
{
    return playlistProxy ? playlistProxy->rowCount() : 0;
}

int MainWindow::plCurrentRow() const
{
    return fileList ? fileList->currentIndex().row() : -1;
}

void MainWindow::plSetCurrentRow(int row)
{
    if (!fileList || !playlistProxy) return;
    if (row < 0 || row >= playlistProxy->rowCount()) {
        fileList->clearSelection();
        fileList->setCurrentIndex(QModelIndex());
        return;
    }
    QModelIndex idx = playlistProxy->index(row, 0);
    if (fileList->selectionModel())
        fileList->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect);
    else
        fileList->setCurrentIndex(idx);
    fileList->scrollTo(idx, QAbstractItemView::EnsureVisible);
}

int MainWindow::plRowType(int row) const
{
    if (!playlistProxy || row < 0 || row >= playlistProxy->rowCount()) return MIDI_FILE;
    QVariant v = playlistProxy->index(row, 0).data(PlaylistModel::TypeRole);
    return v.isValid() ? v.toInt() : MIDI_FILE;
}

QString MainWindow::plRowPath(int row) const
{
    if (!playlistProxy || row < 0 || row >= playlistProxy->rowCount()) return QString();
    return playlistProxy->index(row, 0).data(PlaylistModel::PathRole).toString();
}

QString MainWindow::plRowText(int row) const
{
    if (!playlistProxy || row < 0 || row >= playlistProxy->rowCount()) return QString();
    return playlistProxy->index(row, 0).data(Qt::DisplayRole).toString();
}

int MainWindow::plCurrentType() const
{
    int r = plCurrentRow();
    return r >= 0 ? plRowType(r) : -1;
}

QString MainWindow::plCurrentPath() const
{
    int r = plCurrentRow();
    return r >= 0 ? plRowPath(r) : QString();
}

QString MainWindow::plCurrentText() const
{
    int r = plCurrentRow();
    return r >= 0 ? plRowText(r) : QString();
}

int MainWindow::plFirstSelectedRow() const
{
    if (!fileList || !fileList->selectionModel()) return -1;
    const QModelIndexList sel = fileList->selectionModel()->selectedIndexes();
    int minRow = INT_MAX;
    for (const QModelIndex &idx : sel)
        if (idx.row() < minRow) minRow = idx.row();
    return (minRow == INT_MAX) ? -1 : minRow;
}

// Row of the currently-playing file in the CURRENT view, or -1 if it isn't
// shown (e.g. the user browsed into another folder). Matched by the raw stored
// path, which is exactly what currentRawPath holds.
int MainWindow::plRowOfPlayingFile() const
{
    if (currentRawPath.isEmpty()) return -1;
    const int n = plCount();
    for (int i = 0; i < n; ++i) {
        if (plRowType(i) == MIDI_FILE && plRowPath(i) == currentRawPath)
            return i;
    }
    return -1;
}

// Recursively find the tree node whose direct child is the given file path.
PlaylistTreeNode* MainWindow::findParentNodeOfFile(const QString& filePath) const
{
    if (filePath.isEmpty() || !playlistRoot) return nullptr;
    std::function<PlaylistTreeNode*(PlaylistTreeNode*)> search =
        [&](PlaylistTreeNode* node) -> PlaylistTreeNode* {
            if (!node) return nullptr;
            for (auto* child : node->children) {
                if (!child) continue;
                if (!child->isFolder && child->fullPath == filePath) return node;
                if (child->isFolder) {
                    if (auto* r = search(child)) return r;
                }
            }
            return nullptr;
        };
    return search(playlistRoot);
}

// Ordered playable paths for the PLAYING file's folder, independent of the
// displayed view (so prev/next/repeat/shuffle advance the play queue WITHOUT
// yanking the browser to the playing folder). Falls back to the displayed list
// when the playing file can't be located in the tree. *playingIndex receives the
// index of currentRawPath, or -1.
QStringList MainWindow::playingQueue(int* playingIndex) const
{
    if (playingIndex) *playingIndex = -1;
    QStringList paths;

    PlaylistTreeNode* node = findParentNodeOfFile(currentRawPath);
    if (node) {
        QList<PlaylistTreeNode*> files;
        for (auto* c : node->children)
            if (c && !c->isFolder) files.append(c);
        std::sort(files.begin(), files.end(), [](PlaylistTreeNode* a, PlaylistTreeNode* b) {
            return a->name < b->name;   // same order updateUIFromCurrentNode shows
        });
        paths.reserve(files.size());
        for (auto* c : files) {
            QString p = c->fullPath.isEmpty() ? c->name : c->fullPath;
            if (playingIndex && p == currentRawPath) *playingIndex = paths.size();
            paths.append(p);
        }
        return paths;
    }

    // Fallback: the displayed view.
    const int n = plCount();
    for (int i = 0; i < n; ++i) {
        if (plRowType(i) != MIDI_FILE) continue;
        QString p = plRowPath(i);
        if (playingIndex && p == currentRawPath) *playingIndex = paths.size();
        paths.append(p);
    }
    return paths;
}

// Move the playlist selection by `delta` rows (used for Up/Down browsing while
// stopped, even when the list doesn't hold keyboard focus).
void MainWindow::movePlaylistSelection(int delta)
{
    int n = plCount();
    if (n == 0) return;
    int cur = plCurrentRow();
    int next;
    if (cur < 0) {
        next = (delta > 0) ? 0 : n - 1;
    } else {
        next = cur + delta;
        if (next < 0) next = 0;
        if (next >= n) next = n - 1;
    }
    plSetCurrentRow(next);
}
