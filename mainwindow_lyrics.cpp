// Split from mainwindow.cpp (lyrics-sync domain) - implementation-only
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


void MainWindow::toggleLyricsWindow()
{
    if (!lyricsWindow) {
        // Create and show lyrics window
        lyricsWindow = new LyricsWindow(this);

        // Connect close signal
        connect(lyricsWindow, &LyricsWindow::closed, [this]() {
            lyricsWindow = nullptr;
            lyricsButton->setStyleSheet(
                "QPushButton {"
                "    font-size: 16px;"
                "    border: 1px solid #666666;"
                "    border-radius: 3px;"
                "    background-color: #3a3a3a;"
                "    color: white;"
                "    padding: 0px;"
                "}"
                "QPushButton:hover {"
                "    background-color: #4a4a4a;"
                "    border: 2px solid #0078d4;"
                "}"
                "QPushButton:pressed {"
                "    background-color: #2a2a2a;"
                "}"
            );
        });

        // Connect channel changed signal
        connect(lyricsWindow, &LyricsWindow::channelChanged, this, &MainWindow::onLyricChannelChanged);
        connect(lyricsWindow, &LyricsWindow::lyricsEdited, this, &MainWindow::onLyricsEdited);

        // Update button style to show it's active
        lyricsButton->setStyleSheet(
            "QPushButton {"
            "    font-size: 16px;"
            "    border: 2px solid #0078d4;"
            "    border-radius: 3px;"
            "    background-color: #4a4a4a;"
            "    color: white;"
            "    padding: 0px;"
            "}"
            "QPushButton:hover {"
            "    background-color: #5a5a5a;"
            "    border: 2px solid #0078d4;"
            "}"
            "QPushButton:pressed {"
            "    background-color: #2a2a2a;"
            "}"
        );

        // ?꾩옱 ?뚯씪?먯꽌 媛€??濡쒕뱶
        if (!currentFile.isEmpty()) {
            qDebug() << "[MainWindow] Loading lyrics from file:" << currentFile;

            // 플레이리스트에서 현재 곡 이름 가져오기
            QString songTitle = "Lyrics";
            if (plHasCurrent()) {
                songTitle = plCurrentText();
            }
            // Clean up title: remove "\xe2\x99\xab " and "FILENAME - "
            QString cleanTitle = songTitle;
            if (cleanTitle.startsWith("\xe2\x99\xab ")) {
                cleanTitle = cleanTitle.mid(2);
            }
            int dashPos = cleanTitle.indexOf(" - ");
            if (dashPos != -1) {
                cleanTitle = cleanTitle.mid(dashPos + 3);
            }
            lyricsWindow->setTitle(cleanTitle);

            bool isNobFile = currentFile.toLower().endsWith(".nob");
            updateLyricsWindowContent(currentFile, isNobFile, true, "toggleLyrics");

        }

        lyricsWindow->show();
    } else {
        // Close lyrics window
        lyricsWindow->close();
        lyricsWindow = nullptr;

        // Reset button style
        lyricsButton->setStyleSheet(
            "QPushButton {"
            "    font-size: 16px;"
            "    border: 1px solid #666666;"
            "    border-radius: 3px;"
            "    background-color: #3a3a3a;"
            "    color: white;"
            "    padding: 0px;"
            "}"
            "QPushButton:hover {"
            "    background-color: #4a4a4a;"
            "    border: 2px solid #0078d4;"
            "}"
            "QPushButton:pressed {"
            "    background-color: #2a2a2a;"
            "}"
        );
    }
}

void MainWindow::onLyricsEdited(const QStringList& newLyrics)
{
    currentLyrics = newLyrics;

    if (!currentNobFilePath.isEmpty()) {
        if (!currentMarkerEvents.isEmpty()) {
            QList<MidiPlayer::MarkerEvent> adjustedMarkers = adjustMarkersForLyrics(currentMarkerEvents, currentLyrics);
            currentLyricMarkerTicks.clear();
            resetLyricSyncState();
            for (const auto& marker : adjustedMarkers) {
                currentLyricMarkerTicks.append(marker.tick);
            }
            qDebug() << "[MainWindow] Lyrics edited: remapped" << currentLyricMarkerTicks.size() << "markers";
        } else {
            currentLyricMarkerTicks.clear();
            resetLyricSyncState();
            qDebug() << "[MainWindow] Lyrics edited: no marker events available for remap";
        }
        updateWindowTitle();
    } else {
        // Standard MIDI playback: lyrics progress is percentage-based
        currentLyricMarkerTicks.clear();
        resetLyricSyncState();
    }
}

QStringList MainWindow::loadLyricsForNob(const QString& filePath, bool *usedExternal) const
{
    bool externalFound = false;
    QStringList lyrics = NobFileHandler::loadExternalLyrics(filePath, &externalFound);
    if (usedExternal) {
        *usedExternal = externalFound;
    }

    if (externalFound) {
        return lyrics;
    }

    return NobFileHandler::extractLyrics(filePath);
}

bool MainWindow::updateLyricsWindowContent(const QString& filePath, bool isNobFile, bool updateMarkers, const char* contextTag)
{
    if (!lyricsWindow) {
        if (isNobFile && !isOkaFile(filePath) && !isGybFile(filePath)) {
            currentNobFilePath = filePath;
        } else {
            currentNobFilePath.clear();
        }
        return false;
    }

    const QString context = contextTag ? QString::fromLatin1(contextTag) : QStringLiteral("update");

    // Title must reflect the file whose lyrics are being shown (filePath), NOT the
    // playlist *selection*. During continuous playback the selected row does not
    // follow the playing track, so using plCurrentText() left the lyrics window
    // showing the PREVIOUS song's title. Derive it straight from the file
    // (player-state independent, same extractors updateTrackInfo uses).
    QString cleanTitle;
    if (!filePath.isEmpty()) {
        if (isGybFile(filePath))                        cleanTitle = GybFileHandler::extractTitle(filePath);
        else if (filePath.toLower().endsWith(".nob"))   cleanTitle = NobFileHandler::extractTitle(filePath);
        else if (OkaFileHandler::isOkaFile(filePath))   cleanTitle = OkaFileHandler::extractTitle(filePath);
        else if (isOplFile(filePath))                   cleanTitle = ImsPlayer::extractTitleQuick(filePath);
        if (cleanTitle.trimmed().isEmpty())
            cleanTitle = QFileInfo(filePath).completeBaseName();
    }
    if (cleanTitle.trimmed().isEmpty()) {
        // Last resort: the playlist selection text, cleaned of the 🎵 prefix and
        // any "FILENAME - " lead-in.
        QString songTitle = plHasCurrent() ? plCurrentText() : QStringLiteral("Lyrics");
        cleanTitle = songTitle;
        if (cleanTitle.startsWith("\xe2\x99\xab ")) cleanTitle = cleanTitle.mid(2);
        int dashPos = cleanTitle.indexOf(" - ");
        if (dashPos != -1) cleanTitle = cleanTitle.mid(dashPos + 3);
    }
    lyricsWindow->setTitle(cleanTitle);

    QStringList lyrics;
    QStringList displayedLyrics;

    bool actualIsNob = isNobFile && !isOkaFile(filePath) && !isGybFile(filePath);
    if (actualIsNob) {
        currentNobFilePath = filePath;
        lyricsWindow->setNobFile(true);
        lyricsWindow->setChannelWidgetVisible(true); // NOB는 채널 선택 노출
        lyricsWindow->setCurrentFilePath(filePath);

        bool usedExternal = false;
        lyrics = loadLyricsForNob(filePath, &usedExternal);
        if (usedExternal) {
            qDebug() << "[MainWindow]" << context << ": Using external lyrics override for" << filePath;
        }

        displayedLyrics = expandLyricsForRepeat(lyrics, true);

        if (updateMarkers) {
            int markerChannel = NobFileHandler::detectMarkerChannel(filePath);
            QList<MidiPlayer::MarkerEvent> allMarkers;
            if (markerChannel > 0) {
                qDebug() << "[MainWindow]" << context << ": Auto-detected marker channel:" << markerChannel;
                lyricsWindow->setCurrentChannel(markerChannel);
                allMarkers = midiPlayer->extractMarkerTimings(markerChannel);
            } else {
                qDebug() << "[MainWindow]" << context << ": Failed to detect marker channel";
            }

            currentMarkerEvents = allMarkers;
            currentLyricMarkerTicks.clear();
            resetLyricSyncState();

            if (!allMarkers.isEmpty()) {
                QList<MidiPlayer::MarkerEvent> adjustedMarkers = adjustMarkersForLyrics(allMarkers, displayedLyrics);
                for (const auto& marker : adjustedMarkers) {
                    currentLyricMarkerTicks.append(marker.tick);
                }
                qDebug() << "[MainWindow]" << context << ": Applied" << currentLyricMarkerTicks.size()
                         << "markers from channel" << (markerChannel > 0 ? markerChannel : -1);
            }
        } else {
            currentMarkerEvents.clear();
            currentLyricMarkerTicks.clear();
            resetLyricSyncState();
        }

        currentLyrics = displayedLyrics;
        qDebug() << "[MainWindow]" << context << ": Displaying" << displayedLyrics.size() << "lyric lines from NOB";
    } else if (isGybFile(filePath)) {
        currentNobFilePath.clear();
        currentMarkerEvents.clear();
        currentLyricMarkerTicks.clear();
        resetLyricSyncState();

        lyricsWindow->setNobFile(true); // Treat like NOB for static display and hyphen formatting
        lyricsWindow->setChannelWidgetVisible(false); // GYB는 채널 선택 숨김
        lyricsWindow->setCurrentFilePath(filePath);

        // The GYB stream itself has no accurate lyric timing (only a coarse
        // linear byte-scroll in the DOS player). But a matching OKA file (same
        // folder + basename) carries the song's true per-syllable MIDI sync
        // ticks. When present, drive the GYB lyrics from that OKA, scaling its
        // ticks into the GYB stream-tick space:
        //   gyb_tick = oka_sync_tick * 10 * tbDiv / oka_ppqn
        // (oka_sync_tick*10 = OKA MIDI tick; *tbDiv/ppqn converts the MIDI tick
        //  rate to the GYB stream rate, since both play at the same BPM).
        bool drivenByOka = false;
        QString okaTwin;
        {
            QFileInfo gi(filePath);
            QString base = gi.completeBaseName();
            const QStringList exts = {"OKA", "oka", "OKW", "okw"};
            // 1. Same folder as the GYB (the gyb_oka pairs live together).
            for (const QString& e : exts) {
                QString c = gi.absolutePath() + "/" + base + "." + e;
                if (QFile::exists(c)) { okaTwin = c; break; }
            }
            // 2. A user-configured OKA library folder (the place where the
            //    nore45-converted OKA files are kept), searched recursively by
            //    basename. Lets standalone GYB files reuse their converted OKA.
            if (okaTwin.isEmpty()) {
                QStringList libDirs;
                QString cfg = SettingsManager::instance().value("Lyrics/OkaLibraryDir", "").toString();
                if (!cfg.isEmpty()) libDirs << cfg;
                // Sensible defaults relative to the GYB and the app.
                libDirs << gi.absolutePath() + "/../gyb_oka"
                        << QApplication::applicationDirPath() + "/gyb_oka"
                        << "D:/py/midi-k-c260415/gyb_oka";
                QStringList nameFilters;
                for (const QString& e : exts) nameFilters << base + "." + e;
                for (const QString& dir : libDirs) {
                    if (dir.isEmpty() || !QDir(dir).exists()) continue;
                    // Fast exact-name probe at the top level first.
                    bool found = false;
                    for (const QString& e : exts) {
                        QString c = QDir(dir).absoluteFilePath(base + "." + e);
                        if (QFile::exists(c)) { okaTwin = c; found = true; break; }
                    }
                    if (found) break;
                    QDirIterator it(dir, nameFilters, QDir::Files, QDirIterator::Subdirectories);
                    if (it.hasNext()) { okaTwin = it.next(); break; }
                }
            }
        }
        if (!okaTwin.isEmpty()) {
            QStringList okaLyrics = OkaFileHandler::extractLyrics(okaTwin);
            QList<unsigned long> okaTicks = OkaFileHandler::extractLyricMarkerTicks(okaTwin);
            int ppqn = OkaFileHandler::getMidiTicksPerQuarter(okaTwin);
            int tbDiv = 4;
            {
                QFile gf(filePath);
                if (gf.open(QIODevice::ReadOnly)) {
                    QByteArray h = gf.read(0x29); gf.close();
                    if (h.size() > 0x28) tbDiv = (unsigned char)h[0x28];
                    if (tbDiv < 1 || tbDiv > 64) tbDiv = 4;
                }
            }
            if (!okaLyrics.isEmpty() && !okaTicks.isEmpty() && ppqn > 0) {
                displayedLyrics = expandLyricsForRepeat(okaLyrics, true);
                currentLyrics = displayedLyrics;
                if (updateMarkers) {
                    double scale = 10.0 * (double)tbDiv / (double)ppqn;
                    currentLyricMarkerTicks.clear();
                    for (unsigned long t : okaTicks)
                        currentLyricMarkerTicks.append((unsigned long)((double)t * scale + 0.5));
                }
                drivenByOka = true;
                qDebug() << "[MainWindow]" << context << ": GYB lyrics synced from OKA twin"
                         << QFileInfo(okaTwin).fileName() << "ppqn=" << ppqn << "tbDiv=" << tbDiv
                         << "scale=" << (10.0 * tbDiv / ppqn) << "syllables=" << okaTicks.size();
            }
        }

        if (!drivenByOka) {
            lyrics = GybFileHandler::extractLyrics(filePath);
            displayedLyrics = expandLyricsForRepeat(lyrics, true);
            currentLyrics = displayedLyrics;
            if (updateMarkers) {
                // Use syllable-ticks (1:1 byte scroll) directly for both Korean and English.
                // This guarantees exact 1:1 synchronization between melody ticks and lyric stream bytes.
                currentLyricMarkerTicks = GybFileHandler::extractLyricSyllableTicks(filePath);

                if (currentLyricMarkerTicks.isEmpty())
                    currentLyricMarkerTicks = GybFileHandler::extractLyricLineTicks(filePath, displayedLyrics);
            }
        }
        qDebug() << "[MainWindow]" << context << ": Displaying" << displayedLyrics.size()
                 << "lyric lines (GYB," << (drivenByOka ? "OKA-synced" : "byte-scroll") << ")";

        if (updateMarkers) {
            int melodyChannel = GybFileHandler::detectMelodyChannel(filePath);
            lyricsWindow->setCurrentChannel(melodyChannel);
        }
    } else if (isOkaFile(filePath)) {
        currentNobFilePath.clear();
        currentMarkerEvents.clear();
        currentLyricMarkerTicks.clear();
        resetLyricSyncState();

        lyricsWindow->setNobFile(true); // Treat like NOB for formatting
        lyricsWindow->setCurrentFilePath(filePath);

        lyrics = OkaFileHandler::extractLyrics(filePath);
        displayedLyrics = expandLyricsForRepeat(lyrics, true);
        currentLyrics = displayedLyrics;
        qDebug() << "[MainWindow]" << context << ": Displaying" << displayedLyrics.size() << "lyric lines from OKA";

        if (updateMarkers) {
            if (isOkaOplFile(filePath)) {
                // .oka → OPL OkaPlayer.
                // 1. Calculate lyric syllable unit count
                int totalUnits = 0;
                for (const QString& line : displayedLyrics) {
                    for (const QChar& ch : line) {
                        if (ch != ' ' && ch != '-' && ch != '@') {
                            totalUnits++;
                        }
                    }
                }

                // 2. Try using the native Lyric Sync Block embedded in the OKA file (10x scaling)
                QList<unsigned long> rawTicks = OkaFileHandler::extractLyricMarkerTicks(filePath);
                if (!rawTicks.isEmpty() && rawTicks.size() >= totalUnits * 0.8) {
                    currentLyricMarkerTicks.clear();
                    for (unsigned long tk : rawTicks) {
                        currentLyricMarkerTicks.append(tk * 10); // scale up 1/10 tick to standard midi tick
                    }
                    qDebug() << "[MainWindow]" << context << ": OKA (OPL) loaded native lyric sync block with" 
                             << currentLyricMarkerTicks.size() << "markers (syllables=" << totalUnits << ")";
                } else {
                    // Fallback to OPL OkaPlayer note-on markers if sync block is corrupted/empty
                    currentLyricMarkerTicks.clear();
                    const QList<unsigned long>& markers = okaPlayer->getLyricMarkerTicks();
                    for (unsigned long tk : markers) {
                        currentLyricMarkerTicks.append(tk);
                    }
                    qDebug() << "[MainWindow]" << context << ": OKA (OPL) fallback to player Note-On markers:" 
                             << currentLyricMarkerTicks.size() << "markers";
                }
                
                // Hide channel selection UI for Oksori files
                lyricsWindow->setChannelWidgetVisible(false);
            } else {
                // .okm → decoded SMF played by midiPlayer.
                // 1. Calculate lyric syllable unit count
                int totalUnits = 0;
                for (const QString& line : displayedLyrics) {
                    for (const QChar& ch : line) {
                        if (ch != ' ' && ch != '-' && ch != '@') {
                            totalUnits++;
                        }
                    }
                }

                // Refined heuristic to auto-detect melody/marker channel (for fallback use)
                int cnt[17] = {0};
                for (int ch = 1; ch <= 16; ++ch) {
                    cnt[ch] = midiPlayer->extractMarkerTimings(ch).size();
                }

                int markerChannel = 2;
                if (totalUnits > 0) {
                    int minCover = (int)(totalUnits * 0.8);
                    int bestCh = -1, bestDiff = 1 << 30;
                    for (int ch : {1, 2, 11}) {
                        if (cnt[ch] < minCover || cnt[ch] == 0) continue;
                        int diff = qAbs(cnt[ch] - totalUnits);
                        if (diff < bestDiff) { bestDiff = diff; bestCh = ch; }
                    }
                    if (bestCh < 0) {
                        double bestScore = -1.0; bestCh = 2;
                        for (int ch = 1; ch <= 16; ++ch) {
                            if (cnt[ch] == 0) continue;
                            double ratio = (double)cnt[ch] / (double)totalUnits;
                            double score = 1.0 - qAbs(ratio - 1.0);
                            if (score > bestScore) { bestScore = score; bestCh = ch; }
                        }
                    }
                    markerChannel = bestCh;
                }
                lyricsWindow->setCurrentChannel(markerChannel);

                // Prepare note-on fallback events in case sync block is missing/corrupted
                QList<MidiPlayer::MarkerEvent> allMarkers = midiPlayer->extractMarkerTimings(markerChannel);
                QList<MidiPlayer::MarkerEvent> adjusted = adjustMarkersForLyrics(allMarkers, displayedLyrics);
                currentMarkerEvents = adjusted;

                // 2. Unify all OKM files to use the native Lyric Sync Block embedded in the file
                QList<unsigned long> rawTicks = OkaFileHandler::extractLyricMarkerTicks(filePath);
                if (!rawTicks.isEmpty() && rawTicks.size() >= totalUnits * 0.8) {
                    currentLyricMarkerTicks.clear();
                    for (unsigned long tk : rawTicks) {
                        currentLyricMarkerTicks.append(tk * 10); // scale up 1/10 tick to standard midi tick
                    }
                    qDebug() << "[MainWindow]" << context << ": OKM loaded native lyric sync block with" 
                             << currentLyricMarkerTicks.size() << "markers (syllables=" << totalUnits << ")";
                } else {
                    // Fallback to Note-On markers if sync block is corrupted/empty
                    currentLyricMarkerTicks.clear();
                    for (const auto& m : adjusted) {
                        currentLyricMarkerTicks.append(m.tick);
                    }
                    qDebug() << "[MainWindow]" << context << ": OKM fallback to Note-On marker channel:" << markerChannel
                             << "→" << currentLyricMarkerTicks.size() << "markers";
                }

                // Hide channel selection UI for Oksori files
                lyricsWindow->setChannelWidgetVisible(false);
            }
        }
    } else if (isOplFile(filePath)) {
        currentNobFilePath.clear();
        currentMarkerEvents.clear();
        currentLyricMarkerTicks.clear();
        currentLyricMarkerMs.clear();
        resetLyricSyncState();

        lyricsWindow->setNobFile(true); // Sync like NOB
        lyricsWindow->setChannelWidgetVisible(false); // IMS는 채널 선택 숨김
        lyricsWindow->setCurrentFilePath(filePath);

        QString issPath = filePath;
        bool isZipInner = false;
        QString zipFile;
        QString innerIssPath;

        if (currentRawPath.contains("::")) {
            QStringList parts = currentRawPath.split("::");
            if (parts.size() >= 2) {
                zipFile = parts[0];
                QString innerOplPath = parts[1];
                if (isOplFile(innerOplPath)) {
                    innerIssPath = innerOplPath;
                    innerIssPath.replace(innerIssPath.length() - 4, 4, ".iss");
                    isZipInner = true;
                }
            }
        }

        if (isZipInner) {
            QZipReader zip(zipFile);
            if (zip.status() == QZipReader::NoError) {
                QByteArray data = zip.fileData(innerIssPath);
                if (data.isEmpty()) {
                    QString upperIss = innerIssPath;
                    upperIss.replace(upperIss.length() - 4, 4, ".ISS");
                    data = zip.fileData(upperIss);
                }

                if (!data.isEmpty()) {
                    if (m_tempIssFile) {
                        delete m_tempIssFile;
                        m_tempIssFile = nullptr;
                    }
                    QString tempDir = QCoreApplication::applicationDirPath() + "/temp";
                    QDir().mkpath(tempDir);
                    m_tempIssFile = new QTemporaryFile(tempDir + "/jJomeZipIss_XXXXXX.iss");
                    if (m_tempIssFile->open()) {
                        m_tempIssFile->write(data);
                        m_tempIssFile->close();
                        issPath = m_tempIssFile->fileName();
                    }
                } else {
                    issPath = "";
                }
            }
        } else {
            if (isOplFile(issPath)) {
                issPath.replace(issPath.length() - 4, 4, ".iss");
                if (!QFile::exists(issPath)) {
                    QString temp = filePath;
                    temp.replace(temp.length() - 4, 4, ".ISS");
                    if (QFile::exists(temp)) issPath = temp;
                }
            }
        }

        if (QFile::exists(issPath) && !issPath.isEmpty()) {
            // IMS 헤더값으로 정확한 ms 변환
            int basicTempo = imsPlayer->getBasicTempo();
            int nTickBeat  = imsPlayer->getTickBeat();
            currentIssData = IssFileHandler::loadIssFile(issPath, basicTempo, nTickBeat);
            displayedLyrics = currentIssData.displayLines;
            currentLyrics   = displayedLyrics;
            qDebug() << "[MainWindow]" << context << ": Loaded ISS:"
                     << displayedLyrics.size() << "display lines";
        } else {
            qDebug() << "[MainWindow]" << context << ": No ISS file found for" << filePath;
        }
    } else {
        currentNobFilePath.clear();
        currentMarkerEvents.clear();
        currentLyricMarkerTicks.clear();
        resetLyricSyncState();

        lyricsWindow->setNobFile(false);
        lyricsWindow->setCurrentFilePath(QString());
        lyrics = midiPlayer->extractLyrics();
        displayedLyrics = lyrics;
        currentLyrics = displayedLyrics;
        qDebug() << "[MainWindow]" << context << ": Displaying" << displayedLyrics.size() << "lyric lines from MIDI";
    }

    if (!displayedLyrics.isEmpty()) {
        lyricsWindow->setLyrics(displayedLyrics);
        lyricsWindow->reset();
        return true;
    }

    qDebug() << "[MainWindow]" << context << ": No lyrics found";
    lyricsWindow->setLyrics(QStringList());
    currentLyrics.clear();
    return false;
}

void MainWindow::onLyricChannelChanged(int newChannel)
{
    qDebug() << "[MainWindow] Lyric channel changed to:" << newChannel;

    bool isOkm = OkaFileHandler::isOkaFile(currentFile);
    bool isNob = !currentNobFilePath.isEmpty();

    if (!isOkm && !isNob) {
        qDebug() << "[MainWindow] No suitable file loaded for lyric channel change";
        return;
    }

    QList<MidiPlayer::MarkerEvent> allMarkers = midiPlayer->extractMarkerTimings(newChannel);
    currentMarkerEvents = allMarkers;
    qDebug() << "[MainWindow] Extracted" << allMarkers.size() << "markers for channel" << newChannel;

    if (allMarkers.isEmpty()) {
        qDebug() << "[MainWindow] No markers found on channel" << newChannel;
        return;
    }

    QStringList lyrics;
    if (isOkm) {
        lyrics = OkaFileHandler::extractLyrics(currentFile);
    } else {
        bool usedExternalLyrics = false;
        lyrics = loadLyricsForNob(currentNobFilePath, &usedExternalLyrics);
    }

    QStringList expandedLyrics = expandLyricsForRepeat(lyrics, true);
    currentLyrics = expandedLyrics;

    if (lyrics.isEmpty()) {
        qDebug() << "[MainWindow] Failed to extract lyrics";
        return;
    }

    int totalUnits = 0;
    for (const QString& line : expandedLyrics) {
        for (const QChar& ch : line) {
            if (ch != ' ' && ch != '-' && ch != '@') {
                totalUnits++;
            }
        }
    }

    if (!isOkm && newChannel == 11 && allMarkers.size() > 1) {
        unsigned long gap = allMarkers[1].tick - allMarkers[0].tick;
        if (gap >= 768) {
            allMarkers.removeFirst();
            qDebug() << "[MainWindow] onLyricChannelChanged: Removed intro marker with gap" << gap;
        }
    }

    QList<MidiPlayer::MarkerEvent> adjustedMarkers = adjustMarkersForLyrics(allMarkers, expandedLyrics);
    currentLyricMarkerTicks.clear();
    for (const auto& marker : adjustedMarkers) {
        currentLyricMarkerTicks.append(marker.tick);
    }

    qDebug() << "[MainWindow] Channel" << newChannel << ": Applied" << currentLyricMarkerTicks.size()
             << "markers for" << totalUnits << "lyric units";

    resetLyricSyncState();
    if (lyricsWindow) {
        lyricsWindow->reset();
    }
}

void MainWindow::resetLyricSyncState()
{
    lastDisplayedLyricIndex = -1;
    lastIssLineIdx = -1;
}

QStringList MainWindow::expandLyricsForRepeat(const QStringList& originalLyrics, bool isNobFile) const
{
    Q_UNUSED(isNobFile);
    return originalLyrics;
}

QList<MidiPlayer::MarkerEvent> MainWindow::adjustMarkersForLyrics(const QList<MidiPlayer::MarkerEvent>& markers,
                                                                  const QStringList& lyrics) const
{
    if (markers.size() <= 1 || lyrics.isEmpty()) {
        return markers;
    }

    struct LyricUnit {
        bool hasTrailingHyphen = false;
    };

    QVector<LyricUnit> units;
    units.reserve(lyrics.size() * 16);

    for (const QString& line : lyrics) {
        for (int i = 0; i < line.size(); ++i) {
            QChar ch = line[i];
            if (ch == ' ' || ch == '-' || ch == '@') {
                continue;
            }

            LyricUnit unit;
            unit.hasTrailingHyphen = (i + 1 < line.size() && line[i + 1] == '-');
            units.append(unit);
        }
    }

    if (units.isEmpty()) {
        return markers;
    }

    QVector<unsigned long> deltas;
    deltas.reserve(markers.size() - 1);
    for (int i = 1; i < markers.size(); ++i) {
        unsigned long delta = markers[i].tick - markers[i - 1].tick;
        if (delta > 0) {
            deltas.append(delta);
        }
    }

    unsigned long typicalDelta = 0;
    if (!deltas.isEmpty()) {
        std::sort(deltas.begin(), deltas.end());
        typicalDelta = deltas[deltas.size() / 2];
    }

    // [보안] 80ms 미만의 극히 짧은 간격으로 들어오는 연속 Note-On은
    // 멜로디의 꾸밈음, 트릴, 연타 장식음이므로 디바운싱하여 가사 싱크에서 제외합니다.
    QList<MidiPlayer::MarkerEvent> cleanMarkers;
    cleanMarkers.reserve(markers.size());
    if (!markers.isEmpty()) {
        cleanMarkers.append(markers.first());
        for (int i = 1; i < markers.size(); ++i) {
            const auto& prev = cleanMarkers.last();
            const auto& curr = markers[i];
            if (curr.timeMs - prev.timeMs >= 80) {
                cleanMarkers.append(curr);
            } else {
                qDebug() << "[adjustMarkersForLyrics] Debounced close marker at tick" << curr.tick 
                         << "ms" << curr.timeMs << "(diff" << (curr.timeMs - prev.timeMs) << "ms)";
            }
        }
    }

    const unsigned long fallbackThreshold = 96; // half note for TPQN=192
    const unsigned long threshold = typicalDelta > 0 ? qMax<unsigned long>(1, typicalDelta / 2) : fallbackThreshold;

    QList<MidiPlayer::MarkerEvent> adjusted;
    adjusted.reserve(cleanMarkers.size());

    int markerIndex = 0;
    int unitIndex = 0;

    while (markerIndex < cleanMarkers.size() && unitIndex < units.size()) {
        const auto& marker = cleanMarkers[markerIndex];
        adjusted.append(marker);

        bool hasHyphen = units[unitIndex].hasTrailingHyphen;

        markerIndex++;
        unitIndex++;

        if (markerIndex < cleanMarkers.size() && hasHyphen) {
            const auto& candidate = cleanMarkers[markerIndex];
            const auto& reference = adjusted.last();
            unsigned long delta = candidate.tick - reference.tick;
            int noteDiff = std::abs(candidate.noteNumber - reference.noteNumber);
            int velocityDiff = std::abs(candidate.velocity - reference.velocity);

            if (delta <= threshold && noteDiff <= 1 && velocityDiff <= 16) {
                // Skip this candidate marker as it represents a sustained syllable with hyphen
                markerIndex++;
            }
        }
    }

    while (markerIndex < cleanMarkers.size()) {
        adjusted.append(cleanMarkers[markerIndex++]);
    }

    return adjusted;
}
