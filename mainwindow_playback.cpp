// Split from mainwindow.cpp (playback/transport domain) - implementation-only
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


// JJoMeSynth (the miniaudio host that drives Ims/Gyb/OkaPlayer's renderAudio
// callbacks) used to be lazily initialized ONLY in the IMS branch of
// playPause() - so on a fresh launch, playing a GYB/OKA FIRST produced no
// audio callback at all: no sequencer ticks, no local sound, and nothing on
// the OPL tunnel ("mt32-pi 모드에서 GYB가 IMS를 한 번 재생한 후부터만 연주됨",
// 2026-07-16). Every OPL-engine branch now calls this before play().
void MainWindow::ensureJJoMeSynthReady()
{
    if (JJoMeSynth::instance().isInitialized())
        return;

    SettingsManager& settings = SettingsManager::instance();
    QString sfPath = settings.value("Synth/SoundFontPath", "").toString();
    if (sfPath.isEmpty() || !QFileInfo::exists(sfPath)) {
        QDir sfDir(QApplication::applicationDirPath() + "/SoundFonts");
        if (sfDir.exists()) {
            QStringList filters; filters << "*.sf2" << "*.sf3";
            QFileInfoList fileList = sfDir.entryInfoList(filters, QDir::Files);
            if (!fileList.isEmpty()) sfPath = fileList.first().absoluteFilePath();
        }
    }
    if (!sfPath.isEmpty()) JJoMeSynth::instance().initialize(sfPath);
}

void MainWindow::playPause()
{
    // Skip MIDI device check - allow playback without device

    if (plCount() == 0) {
        QMessageBox::information(this, "MIDI Player", "No files to play!");
        return;
    }

    if (isPlaying) {
        midiPlayer->pause();
        imsPlayer->pause();
        gybPlayer->pause();
        okaPlayer->pause();
        setPlaying(false);
        m_pausedByUser = true;   // Play-button pause is resumable by Space too
        positionTimer->stop();
    } else {
        if (plCurrentRow() < 0) {
            // Find first playable file
            for (int i = 0; i < plCount(); ++i) {
                if (plRowType(i) == MIDI_FILE) {
                    plSetCurrentRow(i);
                    break;
                }
            }
        }

        if (plHasCurrent() && plCurrentType() == MIDI_FILE) {
            QString rawPath = plCurrentPath();
            QString filePath = resolvePlayablePath(rawPath);
            bool isImsFile = isOplFile(filePath);
            bool isGybFile = filePath.toLower().endsWith(".gyb");
            bool isNobFile = filePath.toLower().endsWith(".nob");
            bool isOka = isOkaFile(filePath);
            bool playOkaViaOpl = isOkaOplFile(filePath);

            qDebug() << "[MainWindow] Attempting to play:" << filePath << "isGybFile:" << isGybFile << "isImsFile:" << isImsFile << "isNobFile:" << isNobFile << "isOka:" << isOka << "playOkaViaOpl:" << playOkaViaOpl;

            if (currentRawPath != rawPath) {
                bool loaded = false;
                if (isGybFile) {
                    qDebug() << "[MainWindow] Loading GYB file:" << filePath;
                    SettingsManager& settings = SettingsManager::instance();
                    QString gybExt = settings.value("Synth/ExternalGybBank", "").toString();
                    if (!gybExt.isEmpty() && QFileInfo::exists(gybExt)) {
                        gybPlayer->setExternalBankPath(gybExt);
                    } else {
                        gybPlayer->setExternalBankPath("");
                    }
                    loaded = gybPlayer->loadFile(filePath);
                    if (!loaded) qWarning() << "[MainWindow] Failed to load GYB file:" << filePath;
                } else if (isImsFile) {
                    qDebug() << "[MainWindow] Loading IMS/OPL file:" << filePath;
                    loaded = imsPlayer->loadFile(filePath);
                } else if (playOkaViaOpl) {
                    qDebug() << "[MainWindow] Loading OKA file via OPL:" << filePath;
                    SettingsManager& settings = SettingsManager::instance();
                    QString okaExt = settings.value("Synth/ExternalOkaBank", "").toString();
                    if (!okaExt.isEmpty() && QFileInfo::exists(okaExt)) {
                        okaPlayer->setExternalBankPath(okaExt);
                    } else {
                        okaPlayer->setExternalBankPath("");
                    }
                    loaded = okaPlayer->loadFile(filePath);
                    if (!loaded) qWarning() << "[MainWindow] Failed to load OKA file via OPL:" << filePath;
                } else {
                    qDebug() << "[MainWindow] Loading MIDI file:" << filePath;
                    loaded = midiPlayer->loadMidiFile(filePath);
                    midiPlayer->setIsNobFile(isNobFile || isOka);
                }

                if (loaded) {
                    currentFile = filePath;
                    currentRawPath = rawPath;
                    qDebug() << "[MainWindow] File loaded successfully";

                    updateLyricsWindowContent(filePath, isNobFile || isOka, true, "playPause");

                    updateTrackInfo();
                    progressSlider->setValue(0);
                    positionLabel->setText("0%");
                } else {
                    QMessageBox::warning(this, "MIDI Player", "Failed to load file!");
                    qWarning() << "[MainWindow] File loading failed";
                    return;
                }
            } else {
                // Same file: decide between resume-from-pause vs restart-from-end.
                // - Paused mid-song (0 < pos < dur): just play() to resume.
                // - End-of-song or fresh: stop() to rewind cleanly, then play().
                // Without the position check, every pause→play would restart
                // from the beginning because stop() resets m_position to 0.
                unsigned long pos = 0, dur = 0;
                if (isGybFile)          { pos = gybPlayer->getPosition(); dur = gybPlayer->getDuration(); }
                else if (isImsFile)     { pos = imsPlayer->getPosition(); dur = imsPlayer->getDuration(); }
                else if (playOkaViaOpl) { pos = okaPlayer->getPosition(); dur = okaPlayer->getDuration(); }
                else                    { pos = midiPlayer->getCurrentPosition(); dur = midiPlayer->getTotalDuration(); }

                bool atEnd = (dur > 0 && pos + 100 >= dur);  // within 100ms of end
                bool fresh = (pos == 0);
                bool needRestart = atEnd || fresh;

                if (needRestart) {
                    if (isGybFile) {
                        gybPlayer->stop(); if(pianoRollWindow) pianoRollWindow->clearNotes();
                    } else if (isImsFile) {
                        imsPlayer->stop(); if(pianoRollWindow) pianoRollWindow->clearNotes();
                    } else if (playOkaViaOpl) {
                        okaPlayer->stop(); if(pianoRollWindow) pianoRollWindow->clearNotes();
                    } else {
                        midiPlayer->stop(); if(pianoRollWindow) pianoRollWindow->clearNotes();
                    }
                    progressSlider->setValue(0);
                    positionLabel->setText("0%");
                }
                // else: paused mid-song — fall through to play() to resume
            }

            if (isGybFile) {
                // GYB uses OPL just like IMS. External BNK support is enabled.
                qDebug() << "[MainWindow] Starting GYB playback";
                ensureJJoMeSynthReady();
                JJoMeSynth::instance().setGybPlayer(gybPlayer);
                JJoMeSynth::instance().setImsPlayer(nullptr);
                JJoMeSynth::instance().setOkaPlayer(nullptr);
                gybPlayer->play();
                qDebug() << "[MainWindow] GYB play() called";
                dspButton->show(); bankButton->show(); oplTunnelButton->show();
                updateDspButtonStyle();
                if (channelMonitor) {
                    channelMonitor->setImsMode(true, gybPlayer->getBankName(),
                                               gybPlayer->getInstruments(), "GYB");
                    channelMonitor->updateVoiceInstrumentNames(gybPlayer->getVoiceInstrumentNames());
                }
            } else if (isImsFile) {
                if (!JJoMeSynth::instance().isInitialized()) {
                    SettingsManager& settings = SettingsManager::instance();
                    QString extBank = settings.value("Synth/ExternalImsBank", "").toString();
    if (!extBank.isEmpty() && QFileInfo::exists(extBank)) imsPlayer->setExternalBankPath(extBank);
    QString sfPath = settings.value("Synth/SoundFontPath", "").toString();
                    if (sfPath.isEmpty() || !QFileInfo::exists(sfPath)) {
                        QDir sfDir(QApplication::applicationDirPath() + "/SoundFonts");
                        if (sfDir.exists()) {
                            QStringList filters; filters << "*.sf2" << "*.sf3";
                            QFileInfoList fileList = sfDir.entryInfoList(filters, QDir::Files);
                            if (!fileList.isEmpty()) sfPath = fileList.first().absoluteFilePath();
                        }
                    }
                    if (!sfPath.isEmpty()) JJoMeSynth::instance().initialize(sfPath);
                }
                JJoMeSynth::instance().setGybPlayer(nullptr);
                JJoMeSynth::instance().setImsPlayer(imsPlayer);
                JJoMeSynth::instance().setOkaPlayer(nullptr);
                imsPlayer->play();
                dspButton->show(); bankButton->show(); oplTunnelButton->show();
                updateDspButtonStyle(); // Update style
                if (channelMonitor) {
                    channelMonitor->setImsMode(true, imsPlayer->getBankName(), imsPlayer->getInstruments(), QFileInfo(currentFile).suffix().toUpper());
                    channelMonitor->updateVoiceInstrumentNames(imsPlayer->getVoiceInstrumentNames());
                }
            } else if (playOkaViaOpl) {
                qDebug() << "[MainWindow] Starting OKA OPL playback";
                ensureJJoMeSynthReady();
                JJoMeSynth::instance().setGybPlayer(nullptr);
                JJoMeSynth::instance().setImsPlayer(nullptr);
                JJoMeSynth::instance().setOkaPlayer(okaPlayer);
                okaPlayer->play();
                dspButton->show(); bankButton->show(); oplTunnelButton->show();
                updateDspButtonStyle();
                if (channelMonitor) {
                    channelMonitor->setImsMode(true, okaPlayer->getBankName(),
                                               okaPlayer->getInstruments(), "OKA");
                    channelMonitor->updateVoiceInstrumentNames(okaPlayer->getVoiceInstrumentNames());
                }
            } else {
                JJoMeSynth::instance().setGybPlayer(nullptr);
                JJoMeSynth::instance().setImsPlayer(nullptr);
                JJoMeSynth::instance().setOkaPlayer(nullptr);
                midiPlayer->play();
                dspButton->hide(); bankButton->hide(); oplTunnelButton->hide();
                if (channelMonitor) channelMonitor->setImsMode(false);
            }
            setPlaying(true);
            positionTimer->start(100);

            channelUpdateTimer->setSingleShot(true);
            channelUpdateTimer->start(200);
        } else if (plHasCurrent() && plCurrentType() != MIDI_FILE) {
            QMessageBox::information(this, "MIDI Player", "Please select a playable file!");
            return;
        }
    }

    updatePlayButton();
}

void MainWindow::stop()
{
    if (JJoMeSynth::instance().isRecording()) {
        toggleRecording();
    }

    midiPlayer->stop(); if(pianoRollWindow) pianoRollWindow->clearNotes();
    imsPlayer->stop(); if(pianoRollWindow) pianoRollWindow->clearNotes();
    gybPlayer->stop(); if(pianoRollWindow) pianoRollWindow->clearNotes();
    okaPlayer->stop(); if(pianoRollWindow) pianoRollWindow->clearNotes();
    JJoMeSynth::instance().setImsPlayer(nullptr);
    JJoMeSynth::instance().setGybPlayer(nullptr);
    JJoMeSynth::instance().setOkaPlayer(nullptr);
    setPlaying(false);
    m_pausedByUser = false; // a stop is not a pause — Space won't "resume" after Stop
    positionTimer->stop();
    progressSlider->setValue(0);
    positionLabel->setText("0%");
    if (channelMonitor) channelMonitor->setImsMode(false);
    updatePlayButton();
}

void MainWindow::previousTrack()
{
    // While PLAYING: walk the PLAYING folder's queue by path, WITHOUT moving the
    // browser view (like repeat-one). While stopped: just move the selection in
    // the displayed list (browsing).
    if (isPlaying && !currentRawPath.isEmpty()) {
        if (repeatMode == 3) {
            // 셔플: 재생 히스토리를 거슬러 올라감
            if (!shuffleHistory.isEmpty() && shuffleHistory.last() == currentRawPath)
                shuffleHistory.removeLast();
            if (!shuffleHistory.isEmpty()) {
                QString target = shuffleHistory.takeLast(); // pop
                stop();
                if (loadAndPlayByRawPath(target)) return;
                // 실패 시 순차 이전 곡으로 폴백
            }
        }
        int idx;
        QStringList q = playingQueue(&idx);
        if (idx > 0) {
            stop();
            loadAndPlayByRawPath(q[idx - 1]);
        }
        return;
    }

    // Stopped: browse the current view's selection only.
    if (plCount() == 0) return;
    int currentRow = plCurrentRow();
    for (int i = currentRow - 1; i >= 0; --i) {
        if (plRowType(i) == MIDI_FILE) { plSetCurrentRow(i); return; }
    }
}

void MainWindow::nextTrack()
{
    // While PLAYING: advance the PLAYING folder's queue by path, WITHOUT moving
    // the browser view. While stopped: just move the selection (browsing).
    if (isPlaying && !currentRawPath.isEmpty()) {
        if (repeatMode == 3) {
            // 셔플: 현재 곡을 히스토리에 백업 후 다른 무작위 곡 재생
            if (shuffleHistory.isEmpty() || shuffleHistory.last() != currentRawPath) {
                shuffleHistory.append(currentRawPath);
                if (shuffleHistory.size() > 100) shuffleHistory.removeFirst();
            }
            int idx;
            QStringList q = playingQueue(&idx);
            if (!q.isEmpty()) {
                int r = 0;
                if (q.size() > 1) {
                    do { r = QRandomGenerator::global()->bounded(q.size()); } while (r == idx);
                }
                stop();
                loadAndPlayByRawPath(q[r]);
            }
            return;
        }
        int idx;
        QStringList q = playingQueue(&idx);
        stop();
        if (idx >= 0 && idx + 1 < q.size())
            loadAndPlayByRawPath(q[idx + 1]);
        // else: at end → already stopped (next button does not wrap)
        return;
    }

    // Stopped: browse the current view's selection only.
    if (plCount() == 0) return;
    int currentRow = plCurrentRow();
    for (int i = currentRow + 1; i < plCount(); ++i) {
        if (plRowType(i) == MIDI_FILE) { plSetCurrentRow(i); return; }
    }
}

void MainWindow::rewind()
{
    bool isGyb = isGybFile(currentFile);
    if (isGyb && gybPlayer) {
        unsigned long current = gybPlayer->getPosition();
        unsigned long newPos = (current > 5000) ? current - 5000 : 0;
        gybPlayer->setPosition(newPos);
        lastDisplayedLyricIndex = -1;
        return;
    }
    bool isIms = isOplFile(currentFile);
    if (isIms && imsPlayer) {
        unsigned long current = imsPlayer->getPosition();
        unsigned long newPos = (current > 5000) ? current - 5000 : 0;
        imsPlayer->setPosition(newPos);
        lastDisplayedLyricIndex = -1; // 가사 싱크 리셋
        return;
    }
    // Only .oka uses OkaPlayer; .okm plays via midiPlayer (handled below).
    bool isOka = isOkaOplFile(currentFile);
    if (isOka && okaPlayer) {
        unsigned long current = okaPlayer->getPosition();
        unsigned long newPos = (current > 5000) ? current - 5000 : 0;
        okaPlayer->setPosition(newPos);
        lastDisplayedLyricIndex = -1;
        return;
    }
    if (midiPlayer->getTotalDuration() > 0) {
        unsigned long current = midiPlayer->getCurrentPosition();
        unsigned long newPosition = (current > 5000) ? current - 5000 : 0;
        midiPlayer->setPosition(newPosition);
    }
}

void MainWindow::fastForward()
{
    bool isGyb = isGybFile(currentFile);
    if (isGyb && gybPlayer) {
        unsigned long current = gybPlayer->getPosition();
        unsigned long total = gybPlayer->getDuration();
        unsigned long newPos = (current + 5000 < total) ? current + 5000 : total;
        gybPlayer->setPosition(newPos);
        lastDisplayedLyricIndex = -1;
        return;
    }
    bool isIms = isOplFile(currentFile);
    if (isIms && imsPlayer) {
        unsigned long current = imsPlayer->getPosition();
        unsigned long total = imsPlayer->getDuration();
        unsigned long newPos = (current + 5000 < total) ? current + 5000 : total;
        imsPlayer->setPosition(newPos);
        lastDisplayedLyricIndex = -1; // 가사 싱크 리셋
        return;
    }
    // Only .oka uses OkaPlayer; .okm plays via midiPlayer (handled below).
    bool isOka = isOkaOplFile(currentFile);
    if (isOka && okaPlayer) {
        unsigned long current = okaPlayer->getPosition();
        unsigned long total = okaPlayer->getDuration();
        unsigned long newPos = (current + 5000 < total) ? current + 5000 : total;
        okaPlayer->setPosition(newPos);
        lastDisplayedLyricIndex = -1;
        return;
    }
    if (midiPlayer->getTotalDuration() > 0) {
        unsigned long current = midiPlayer->getCurrentPosition();
        unsigned long total = midiPlayer->getTotalDuration();
        unsigned long newPosition = (current + 5000 < total) ? current + 5000 : total;
        midiPlayer->setPosition(newPosition);
    }
}

void MainWindow::onVolumeChanged(int value)
{
    midiPlayer->setVolume(value);
    // AdPlug doesn't have a direct master volume in all players, 
    // but JJoMeSynth::setVolume affects both SoundFont and IMS
    JJoMeSynth::instance().setVolume(value / 127.0f);
    volumeValue->setText(QString::number(value));

    // Persist once the slider settles, not on every step. A drag or a spin of
    // the mouse wheel emits valueChanged dozens of times per second, and each
    // one used to dirty QSettings, whose event-loop sync rewrites the whole INI
    // file on the GUI thread. Volume is written again by saveSettings() on
    // exit, so all this timer risks is losing the last change to a hard kill -
    // hence a short delay rather than dropping the write entirely.
    if (!volumeSaveTimer) {
        volumeSaveTimer = new QTimer(this);
        volumeSaveTimer->setSingleShot(true);
        volumeSaveTimer->setInterval(400);
        connect(volumeSaveTimer, &QTimer::timeout, this, [this]() {
            SettingsManager::instance().setValue("General/volume", volumeSlider->value());
        });
    }
    volumeSaveTimer->start();
}

void MainWindow::onPositionChanged(int value)
{
    bool isGyb = isGybFile(currentFile);
    bool isIms = isOplFile(currentFile);
    // Only .oka uses the OPL OkaPlayer; .okm/.okw play via midiPlayer, so route
    // seeks there too (otherwise the seek hits an idle okaPlayer and does nothing).
    bool isOka = isOkaOplFile(currentFile);
    unsigned long duration = isGyb ? gybPlayer->getDuration()
                                   : (isIms ? imsPlayer->getDuration() 
                                            : (isOka ? okaPlayer->getDuration() : midiPlayer->getTotalDuration()));
    if (duration > 0) {
        // Update position label immediately for visual feedback
        positionLabel->setText(QString::number(value) + "%");

        // Only seek if user is NOT dragging (clicked to specific position)
        if (!isUserDragging) {
            unsigned long newPosition = (value * duration) / 100;
            if (isGyb) {
                gybPlayer->setPosition(newPosition);
            } else if (isIms) {
                imsPlayer->setPosition(newPosition);
            } else if (isOka) {
                okaPlayer->setPosition(newPosition);
            } else {
                midiPlayer->setPosition(newPosition);
            }
        }
    }
}

void MainWindow::updatePosition()
{
    bool isIms = isOplFile(currentFile);
    bool isGyb = isGybFile(currentFile);
    bool isOka = isOkaFile(currentFile);
    bool playOkaViaOpl = isOkaOplFile(currentFile);

    unsigned long current = isGyb ? gybPlayer->getPosition()
                                  : (isIms ? imsPlayer->getPosition()
                                           : (playOkaViaOpl ? okaPlayer->getPosition()
                                                            : midiPlayer->getCurrentPosition()));
    unsigned long total   = isGyb ? gybPlayer->getDuration()
                                  : (isIms ? imsPlayer->getDuration()
                                           : (playOkaViaOpl ? okaPlayer->getDuration()
                                                            : midiPlayer->getTotalDuration()));

    if (isPlaying && total > 0) {

        // Let MidiPlayer handle its own playback end detection

        int percentage = static_cast<int>((current * 100) / total);

        // Clamp percentage to 0-100 range
        if (percentage > 100) percentage = 100;
        if (percentage < 0) percentage = 0;

        // Update progress slider without triggering signal
        progressSlider->blockSignals(true);
        progressSlider->setValue(percentage);
        progressSlider->blockSignals(false);

        positionLabel->setText(QString::number(percentage) + "%");

        // Update time display
        updateTimeDisplay();
        
        // Update OPL channel-monitor visualizer (IMS and GYB share the layout).
        if (isIms && channelMonitor && channelMonitor->isVisible()) {
            channelMonitor->updateImsVolumes(imsPlayer->getVoiceVolumes(), imsPlayer->getInstrumentVolumes());
            channelMonitor->updateVoiceInstrumentNames(imsPlayer->getVoiceInstrumentNames());
        } else if (isGyb && channelMonitor && channelMonitor->isVisible()) {
            channelMonitor->updateImsVolumes(gybPlayer->getVoiceVolumes(), gybPlayer->getInstrumentVolumes());
            // GYB voices dynamically change programs; refresh the per-voice
            // instrument names so the monitor shows the actual current patch.
            channelMonitor->updateVoiceInstrumentNames(gybPlayer->getVoiceInstrumentNames());
        } else if (playOkaViaOpl && channelMonitor && channelMonitor->isVisible()) {
            channelMonitor->updateImsVolumes(okaPlayer->getVoiceVolumes(), okaPlayer->getInstrumentVolumes());
            channelMonitor->updateVoiceInstrumentNames(okaPlayer->getVoiceInstrumentNames());
        }

        // Update lyrics window based on tick position (for NOB / GYB files) or percentage (for standard MIDI)
        if (lyricsWindow && lyricsWindow->isVisible()) {
            if ((!currentNobFilePath.isEmpty() || isOka || isGybFile(currentFile)) && !isIms) {
                // Determine the active playback tick
                bool isGyb = isGybFile(currentFile);
                unsigned long currentTick = isGyb ? gybPlayer->getCurrentTick()
                                                  : (playOkaViaOpl ? okaPlayer->getCurrentTick() 
                                                                   : midiPlayer->getCurrentTick());

                // Use markers if available
                // If markers are not available, only show static lyrics without auto-progression
                if (!currentLyricMarkerTicks.isEmpty()) {
                    int currentMarker = -1;
                    const int markerCount = currentLyricMarkerTicks.size();

                    // Find the latest marker reached by the current playback tick.
                    for (int i = markerCount - 1; i >= 0; --i) {
                        if (currentTick >= currentLyricMarkerTicks[i]) {
                            currentMarker = i;
                            break;
                        }
                    }

                    const bool perSyllable = (markerCount > currentLyrics.size() * 3 / 2);

                    if (perSyllable && currentMarker >= 0) {
                        // GYB/OKA/OKM/NOB: smooth (continuous) highlight. Recompute every
                        // tick so the next line's first syllable fades in across the
                        // inter-line gap instead of pausing then snapping. frac = progress
                        // from the current syllable toward the next one (0..1). This is the
                        // visual fade only — the GYB byte-scroll lead/speed corrections live
                        // in GybFileHandler and never touch the accurate OKA/OKM/NOB markers.
                        double frac = 0.0;
                        if (currentMarker + 1 < markerCount) {
                            unsigned long a = currentLyricMarkerTicks[currentMarker];
                            unsigned long b = currentLyricMarkerTicks[currentMarker + 1];
                            if (b > a)
                                frac = qBound(0.0, (double)((long long)currentTick - (long long)a)
                                                   / (double)(b - a), 1.0);
                        }
                        lyricsWindow->setSyllableProgressF((double)currentMarker + frac);
                        lastDisplayedLyricIndex = currentMarker;
                    } else if (currentMarker != lastDisplayedLyricIndex) {
                        lastDisplayedLyricIndex = currentMarker;
                        if (currentMarker < 0) {
                            lyricsWindow->reset();
                        } else if (perSyllable) {
                            // More markers than lines → per-SYLLABLE timing
                            // (OKA/OKM/NOB, and GYB driven by a matching OKA).
                            lyricsWindow->setSyllableProgress(currentMarker);
                        } else {
                            // One marker per line (GYB byte-scroll fallback).
                            lyricsWindow->setCurrentLine(currentMarker);
                        }
                    }
                } // else: No markers available - lyrics are displayed statically without auto-progression
              } else if (isIms) {

                // ISS 틱크: 재생 틱에 따른 과거 이벤트를 누적하여 음절 단위로 칠함
                if (!currentIssData.records.isEmpty()) {
                    uint64_t imsTick = imsPlayer->getCurrentTick();

                    int activeDisplayLineIdx = -1;
                    int lastLine = -1;
                    int maxCharIdx = 0;
                    // For smooth line entry: track the last sung record tick and the
                    // first upcoming record (the next display line) so we can fade it in.
                    uint64_t lastRecTick = 0;
                    uint64_t nextRecTick = 0;
                    int nextDisplayLineIdx = -1;

                    // 각 표시 줄에 해당하는 하이라이트 상태 마스크 리스트 준비
                    int displayCount = currentIssData.displayLines.size();
                    QVector<QVector<bool>> lineHighlights(displayCount);
                    for (int i = 0; i < displayCount; ++i) {
                        lineHighlights[i].resize(currentIssData.displayLines[i].size(), false);
                    }

                    // 과거 시점부터 현재 틱 시점까지의 레코드를 순회하며 칠하기 상태를 누적
                    for (const auto& rec : currentIssData.records) {
                        uint64_t recTick = (uint64_t)rec.kasa_tick * currentIssData.tickMultiplier;
                        if (recTick > imsTick) {
                            // First upcoming record: remember it (and its display line)
                            // so the next line can fade in across the gap.
                            nextRecTick = recTick;
                            for (int i = 0; i < currentIssData.displayLineSource.size(); ++i) {
                                if (currentIssData.displayLineSource[i] == rec.line) {
                                    nextDisplayLineIdx = i;
                                    break;
                                }
                            }
                            break;
                        }

                        int displayLineIdx = -1;
                        for (int i = 0; i < currentIssData.displayLineSource.size(); ++i) {
                            if (currentIssData.displayLineSource[i] == rec.line) {
                                displayLineIdx = i;
                                break;
                            }
                        }
                        if (displayLineIdx < 0) continue;

                        activeDisplayLineIdx = displayLineIdx;

                        // 줄이 변경되거나 칠하는 위치가 이전 최대위치보다 왼쪽으로 가면 리셋
                        if (rec.line != lastLine || rec.char_start < maxCharIdx) {
                            lineHighlights[displayLineIdx].fill(false);
                            lastLine = rec.line;
                        }

                        int endChar = rec.char_start + rec.char_width;
                        for (int c = rec.char_start; c < endChar; ++c) {
                            if (c >= 0 && c < lineHighlights[displayLineIdx].size()) {
                                lineHighlights[displayLineIdx][c] = true;
                            }
                        }
                        maxCharIdx = endChar;
                        lastRecTick = recTick;
                    }

                    // 가사창 갱신
                    if (activeDisplayLineIdx >= 0) {
                        if (activeDisplayLineIdx != lastDisplayedLyricIndex) {
                            lastDisplayedLyricIndex = activeDisplayLineIdx;
                            lyricsWindow->setCurrentLine(activeDisplayLineIdx);
                        }
                        lyricsWindow->setIssHighlight(activeDisplayLineIdx, lineHighlights[activeDisplayLineIdx]);

                        // Smooth line entry: when the next upcoming record belongs to a
                        // different display line, fade that line's first syllable in over
                        // the inter-line gap (matches the GYB/OKA/OKM/NOB behavior).
                        if (nextDisplayLineIdx >= 0 && nextDisplayLineIdx != activeDisplayLineIdx
                            && nextRecTick > lastRecTick) {
                            double frac = (double)(imsTick - lastRecTick)
                                        / (double)(nextRecTick - lastRecTick);
                            lyricsWindow->renderFirstSyllableFade(nextDisplayLineIdx, qBound(0.0, frac, 1.0));
                        }
                    } else {
                        lyricsWindow->reset();
                    }
                }
            } else {
                // Standard MIDI: Use percentage-based timing (no marker channel)
                double progress = static_cast<double>(percentage) / 100.0;
                lyricsWindow->setProgress(progress);
            }
        }
    }
}

void MainWindow::onRepeatModeChanged()
{
    // Cycle through 4 modes: 0->1->2->3->0
    repeatMode = (repeatMode + 1) % 4;

    // Update button text based on mode
    switch (repeatMode) {
        case 0: // Play once
            repeatModeButton->setIcon(QIcon());
            repeatModeButton->setText("▶️");
            break;
        case 1: // Repeat current
            repeatModeButton->setIcon(QIcon(":/1re.png"));
            repeatModeButton->setText("");
            break;
        case 2: // Repeat all
            repeatModeButton->setIcon(QIcon());
            repeatModeButton->setText("🔁");
            break;
        case 3: // Shuffle
            repeatModeButton->setIcon(QIcon());
            repeatModeButton->setText("🔀");
            break;
    }

    // Save repeat mode setting immediately
    SettingsManager& settings = SettingsManager::instance();
    settings.setValue("General/repeatMode", repeatMode);
}

void MainWindow::onPlaybackFinished()
{
    // Always stop the position timer first
    positionTimer->stop();

    // Explicitly stop all players to clean up audio resources and reset states before next track loads
    stop();

    switch (repeatMode) {
        case 0: // Play once - stop playback completely
            setPlaying(false);
            updatePlayButton();
            // Reset progress to 0
            progressSlider->setValue(0);
            positionLabel->setText("0%");
            break;

        case 1: // Repeat current - replay the PLAYING file from the start
            if (!currentRawPath.isEmpty()) {
                loadAndPlayByRawPath(currentRawPath);
            } else {
                setPlaying(false);
                updatePlayButton();
            }
            break;

        case 2: // Repeat all - next track in the PLAYING folder (wrap at end), no view jump
            {
                int idx;
                QStringList q = playingQueue(&idx);
                if (!q.isEmpty()) {
                    int next = (idx >= 0 && idx + 1 < q.size()) ? idx + 1 : 0; // wrap
                    if (!loadAndPlayByRawPath(q[next])) {
                        setPlaying(false);
                        updatePlayButton();
                    }
                } else {
                    setPlaying(false);
                    updatePlayButton();
                }
            }
            break;

        case 3: // Shuffle - random track in the PLAYING folder, no view jump
            {
                // 현재 곡을 히스토리에 백업 (뒤로가기용)
                if (!currentRawPath.isEmpty() &&
                    (shuffleHistory.isEmpty() || shuffleHistory.last() != currentRawPath)) {
                    shuffleHistory.append(currentRawPath);
                    if (shuffleHistory.size() > 100) shuffleHistory.removeFirst();
                }
                int idx;
                QStringList q = playingQueue(&idx);
                if (!q.isEmpty()) {
                    int r = 0;
                    if (q.size() > 1) {
                        do { r = QRandomGenerator::global()->bounded(q.size()); } while (r == idx);
                    }
                    if (!loadAndPlayByRawPath(q[r])) {
                        setPlaying(false);
                        updatePlayButton();
                    }
                } else {
                    setPlaying(false);
                    updatePlayButton();
                }
            }
            break;
    }
}

void MainWindow::setPlaying(bool playing)
{
    isPlaying = playing;
    if (playing) m_pausedByUser = false; // any (re)start clears the pause flag
    JJoMeSynth::instance().setPlaybackActive(playing);

    // 재생 상태 변화에 따른 녹음 버튼 동적 제어
    if (playing) {
        // 재생 시작 시: 녹음 중이 아닌 상태라면 녹음 버튼을 비활성화 (재생 중 녹음 시작 불가)
        if (!JJoMeSynth::instance().isRecording()) {
            recordButton->setEnabled(false);
        }
    } else {
        // 재생 정지 시: 녹음 버튼을 무조건 활성화
        recordButton->setEnabled(true);
    }
}

void MainWindow::updatePlayButton()
{
    if (isPlaying) {
        playButton->setText("PAUSE");
    } else {
        playButton->setText("PLAY");
    }
    updateTrackInfo();
    updateWindowTitle();
}

void MainWindow::updateTrackInfo()
{
    QString filePath;
    QString fileName;
    bool displayingPlayingFile = false;

    if (isPlaying && !currentFile.isEmpty()) {
        filePath = currentFile;
        // Use clean name from imsPlayer or midiPlayer if possible, else fallback
        bool isIms = isOplFile(filePath);
        bool isGyb = isGybFile(filePath);
        if (isGyb) {
            fileName = gybPlayer->getTitle();
        } else if (isIms) {
            fileName = imsPlayer->getTitle();
        } else if (filePath.toLower().endsWith(".nob")) {
            fileName = NobFileHandler::extractTitle(filePath);
        } else if (OkaFileHandler::isOkaFile(filePath)) {
            fileName = OkaFileHandler::extractTitle(filePath);
        }

        if (fileName.isEmpty()) {
            fileName = QFileInfo(filePath).fileName();
        }
        displayingPlayingFile = true;
    } else {
        if (!plHasCurrent()) {
            trackInfoLabel->setText("No file selected");
            return;
        }
        filePath = plCurrentPath();
        fileName = plCurrentText();
    }

    if (!filePath.isEmpty()) {
        // Remove music note symbol and extensions for cleaner display in title
        QString cleanFileName = fileName;
        if (cleanFileName.startsWith("🎵")) {
            cleanFileName = cleanFileName.mid(2); // Remove "🎵" prefix
        } else if (cleanFileName.startsWith("♫ ")) {
            cleanFileName = cleanFileName.mid(3); // Remove "♫ " prefix
        }

        // Check if it's a NOB file and extract title
        if (filePath.endsWith(".nob", Qt::CaseInsensitive)) {
            QString nobTitle = NobFileHandler::extractTitle(filePath);
            if (!nobTitle.isEmpty()) {
                cleanFileName = nobTitle;  // Use NOB title
            } else {
                // Fallback to filename without extension
                cleanFileName = cleanFileName.left(cleanFileName.length() - 4);
            }
        } else if (isGybFile(filePath)) {
            QString gybTitle;
            if (displayingPlayingFile) {
                gybTitle = gybPlayer->getTitle();
            } else {
                gybTitle = GybFileHandler::extractTitle(filePath);
                if (gybTitle.isEmpty()) {
                    gybTitle = GybFileHandler::extractTitleFromLst(filePath);
                }
            }
            if (!gybTitle.isEmpty()) {
                cleanFileName = gybTitle;
            } else if (cleanFileName.endsWith(".gyb", Qt::CaseInsensitive)) {
                cleanFileName = cleanFileName.left(cleanFileName.length() - 4);
            }
        } else if (isOplFile(filePath)) {
            QString imsTitle;
            if (displayingPlayingFile) {
                imsTitle = imsPlayer->getTitle();
            } else {
                imsTitle = ImsPlayer::extractTitleQuick(filePath);
            }
            
            if (!imsTitle.isEmpty()) {
                cleanFileName = imsTitle;
            } else {
                cleanFileName = cleanFileName.left(cleanFileName.length() - 4);
            }
        } else if (OkaFileHandler::isOkaFile(filePath)) {
            QString okaTitle = OkaFileHandler::extractTitle(filePath);
            if (!okaTitle.isEmpty()) {
                cleanFileName = okaTitle;
            } else if (cleanFileName.length() > 4) {
                cleanFileName = cleanFileName.left(cleanFileName.length() - 4); // strip .oka/.okm
            }
        } else if (cleanFileName.endsWith(".mid", Qt::CaseInsensitive)) {
            cleanFileName = cleanFileName.left(cleanFileName.length() - 4);
        } else if (cleanFileName.endsWith(".midi", Qt::CaseInsensitive)) {
            cleanFileName = cleanFileName.left(cleanFileName.length() - 5);
        }

        // Bank name in the title is only kept for SOP (per request: hide the
        // bank suffix in the title for IMS/ROL — it cluttered the title bar).
        if (isOplFile(filePath) && filePath.toLower().endsWith(".sop")) {
            QString bName = imsPlayer->getBankName();
            if (!bName.isEmpty()) {
                cleanFileName += " [" + bName + "]";
            }
        }

        // Get track info
        QString info;
        if (displayingPlayingFile) {
            info = midiPlayer->getTrackInfo();
        } else {
            // For non-playing selection, only show info if it belongs to the loaded file
            if (!filePath.isEmpty() && filePath == midiPlayer->getCurrentFile()) {
                info = midiPlayer->getTrackInfo();
            }
        }

        // Create display text with title and track info using HTML for different font sizes
        QString displayText;
        QString sfInfo = "";
        
        // Only show SF info if playing through the internal SoundFont synth.
        // .okm/.okw play via the synth (only .oka is OPL-routed), so exclude just
        // the OPL formats — the previous isOkaFile() check also hid it for OKM.
        bool isIms = isOplFile(filePath);
        bool isGyb = isGybFile(filePath);
        bool okaViaOpl = isOkaOplFile(filePath);
        if (isPlaying && !isIms && !isGyb && !okaViaOpl && deviceComboBox->currentText() == "[JJoMe Synth (SoundFont)]") {
            QString sfName = JJoMeSynth::instance().getSoundFontName();
            if (!sfName.isEmpty()) {
                sfInfo = QString("<div style='font-size: 11px; color: #FFA500; font-weight: bold;'>[SoundFont: %1]</div>").arg(sfName);
            }
        }

        if (!info.isEmpty()) {
            displayText = QString("<div style='font-size: 18px; font-weight: bold; color: #00FFFF;'>%1%2</div><div style='font-size: 12px;'>%3</div>%4")
                         .arg(QFileInfo(filePath).isDir() ? "" : "🎵")
                         .arg(cleanFileName)
                         .arg(info)
                         .arg(sfInfo);
        } else {
            displayText = QString("<div style='font-size: 18px; font-weight: bold; color: #00FFFF;'>%1%2</div>%3")
                         .arg(QFileInfo(filePath).isDir() ? "" : "🎵")
                         .arg(cleanFileName)
                         .arg(sfInfo);
        }

        trackInfoLabel->setText(displayText);
    } else {
        trackInfoLabel->setText("No file selected");
    }
}

void MainWindow::updateTimeDisplay()
{
    bool isIms = isOplFile(currentFile);
    bool isGyb = isGybFile(currentFile);
    // Only .oka uses OkaPlayer; .okm plays via midiPlayer.
    bool isOka = isOkaOplFile(currentFile);
    unsigned long current = isGyb ? gybPlayer->getPosition()
                                  : (isIms ? imsPlayer->getPosition()
                                           : (isOka ? okaPlayer->getPosition() : midiPlayer->getCurrentPosition()));
    unsigned long total   = isGyb ? gybPlayer->getDuration()
                                  : (isIms ? imsPlayer->getDuration()
                                           : (isOka ? okaPlayer->getDuration() : midiPlayer->getTotalDuration()));

    // Current track number and total tracks
    int currentTrack = plCurrentRow() + 1;
    int totalTracks = plCount();

    // Format time as MM:SS
    QString currentTime = formatTime(current);
    QString totalTime = formatTime(total);

    // Format: XXXX/YYYY MM:SS MM:SS XXX (track/total current total ticks)
    QString timeDisplay = QString("%1/%2 %3 %4 %5")
        .arg(currentTrack, 4, 10, QChar('0'))
        .arg(totalTracks, 4, 10, QChar('0'))
        .arg(currentTime)
        .arg(totalTime)
        .arg(QString::number(current / 10).rightJustified(3, '0')); // Simple tick approximation

    int key = 0;
    int bpm = 120;
    int scale = 100;
    
    if (isGyb && gybPlayer) {
        key = gybPlayer->getUserKeyTranspose();
        bpm = gybPlayer->getCurrentBpm();
        scale = gybPlayer->getUserTempoScale();
    } else if (isOka && okaPlayer) {
        key = okaPlayer->getUserKeyTranspose();
        bpm = okaPlayer->getCurrentBpm();
        scale = okaPlayer->getUserTempoScale();
    } else if (isIms && imsPlayer) {
        key = imsPlayer->getUserKeyTranspose();
        bpm = imsPlayer->getCurrentBpm();
        scale = imsPlayer->getUserTempoScale();
    } else if (!currentFile.isEmpty() && midiPlayer) {
        key = midiPlayer->getUserKeyTranspose();
        bpm = midiPlayer->getCurrentBpm();
        scale = midiPlayer->getUserTempoScale();
    }
    
    if (!currentFile.isEmpty()) {
        timeDisplay += QString(" | Key: %1 | %2 - %3%")
            .arg(key > 0 ? QString("+%1").arg(key) : QString::number(key))
            .arg(bpm)
            .arg(scale);
    }

    timeDisplayLabel->setText(timeDisplay);
}

QString MainWindow::formatTime(unsigned long milliseconds)
{
    unsigned long seconds = milliseconds / 1000;
    unsigned long minutes = seconds / 60;
    seconds = seconds % 60;

    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

// Load + play a file by its raw playlist path WITHOUT moving the displayed view.
// The caller is responsible for stop()-ing the previous track first. Only
// highlights the row if it happens to be visible. Returns success.
bool MainWindow::loadAndPlayByRawPath(const QString& rawPath)
{
    if (rawPath.isEmpty()) return false;

    QString filePath = resolvePlayablePath(rawPath);
    bool isGyb = isGybFile(filePath);
    bool isIms = isOplFile(filePath);
    bool isOka = isOkaFile(filePath);
    bool playOkaViaOpl = isOkaOplFile(filePath);

    if (isGyb) {
        SettingsManager& s = SettingsManager::instance();
        QString gybExt = s.value("Synth/ExternalGybBank", "").toString();
        gybPlayer->setExternalBankPath((!gybExt.isEmpty() && QFileInfo::exists(gybExt)) ? gybExt : QString());
    }
    if (isOka) {
        SettingsManager& sOka = SettingsManager::instance();
        QString okaExt = sOka.value("Synth/ExternalOkaBank", "").toString();
        okaPlayer->setExternalBankPath((!okaExt.isEmpty() && QFileInfo::exists(okaExt)) ? okaExt : QString());
    }

    bool loaded;
    if (isGyb)              loaded = gybPlayer->loadFile(filePath);
    else if (isIms)         loaded = imsPlayer->loadFile(filePath);
    else if (playOkaViaOpl) loaded = okaPlayer->loadFile(filePath);
    else                    loaded = midiPlayer->loadMidiFile(filePath);

    if (!loaded) {
        setPlaying(false);
        updatePlayButton();
        return false;
    }

    currentFile = filePath;
    currentRawPath = rawPath;
    bool isNobFile = filePath.toLower().endsWith(".nob");
    if (!isGyb && !isIms && !playOkaViaOpl)
        midiPlayer->setIsNobFile(isNobFile || isOka);
    updateLyricsWindowContent(filePath, isNobFile || isOka, true, "loadAndPlayByRawPath");
    updateTrackInfo();
    progressSlider->setValue(0);
    positionLabel->setText("0%");

    if (isGyb) {
        JJoMeSynth::instance().setGybPlayer(gybPlayer);
        JJoMeSynth::instance().setImsPlayer(nullptr);
        JJoMeSynth::instance().setOkaPlayer(nullptr);
        gybPlayer->play();
        dspButton->show(); bankButton->show(); oplTunnelButton->show();
        updateDspButtonStyle();
        if (channelMonitor)
            channelMonitor->setImsMode(true, gybPlayer->getBankName(),
                                       gybPlayer->getInstruments(), "GYB");
    } else if (isIms) {
        JJoMeSynth::instance().setGybPlayer(nullptr);
        JJoMeSynth::instance().setImsPlayer(imsPlayer);
        JJoMeSynth::instance().setOkaPlayer(nullptr);
        imsPlayer->play();
        dspButton->show(); bankButton->show(); oplTunnelButton->show();
        updateDspButtonStyle();
        if (channelMonitor)
            channelMonitor->setImsMode(true, imsPlayer->getBankName(),
                                       imsPlayer->getInstruments(),
                                       QFileInfo(filePath).suffix().toUpper());
    } else if (playOkaViaOpl) {
        JJoMeSynth::instance().setGybPlayer(nullptr);
        JJoMeSynth::instance().setImsPlayer(nullptr);
        JJoMeSynth::instance().setOkaPlayer(okaPlayer);
        okaPlayer->play();
        dspButton->show(); bankButton->show(); oplTunnelButton->show();
        updateDspButtonStyle();
        if (channelMonitor)
            channelMonitor->setImsMode(true, okaPlayer->getBankName(),
                                       okaPlayer->getInstruments(), "OKA");
    } else {
        JJoMeSynth::instance().setGybPlayer(nullptr);
        JJoMeSynth::instance().setImsPlayer(nullptr);
        JJoMeSynth::instance().setOkaPlayer(nullptr);
        midiPlayer->play();
        dspButton->hide(); bankButton->hide(); oplTunnelButton->hide();
        if (channelMonitor) channelMonitor->setImsMode(false);
    }

    setPlaying(true);
    updatePlayButton();
    positionTimer->start(100);
    channelUpdateTimer->setSingleShot(true);
    channelUpdateTimer->start(500);

    // Highlight the row only if it is already visible — never move the view.
    int row = plRowOfPlayingFile();
    if (row >= 0) plSetCurrentRow(row);
    return true;
}

// Space = pause/resume of the CURRENT track only. Never starts a newly-selected
// track (that is Enter's job), so browsing the list while paused won't hijack it.
void MainWindow::spacePauseResume()
{
    if (isPlaying) {
        midiPlayer->pause();
        imsPlayer->pause();
        gybPlayer->pause();
        okaPlayer->pause();
        setPlaying(false);
        positionTimer->stop();
        m_pausedByUser = true;   // remember this was a pause (not a stop)
        updatePlayButton();
    } else if (m_pausedByUser && !currentFile.isEmpty()) {
        // Resume exactly what Space paused (monitor/DSP/routing are retained across
        // a pause, so we only need to restart the player from its kept position).
        m_pausedByUser = false;
        bool isGyb = isGybFile(currentFile);
        bool isIms = isOplFile(currentFile);
        bool playOkaViaOpl = isOkaOplFile(currentFile);
        if (isGyb) {
            JJoMeSynth::instance().setGybPlayer(gybPlayer);
            JJoMeSynth::instance().setImsPlayer(nullptr);
            JJoMeSynth::instance().setOkaPlayer(nullptr);
            gybPlayer->play();
        } else if (isIms) {
            JJoMeSynth::instance().setGybPlayer(nullptr);
            JJoMeSynth::instance().setImsPlayer(imsPlayer);
            JJoMeSynth::instance().setOkaPlayer(nullptr);
            imsPlayer->play();
        } else if (playOkaViaOpl) {
            JJoMeSynth::instance().setGybPlayer(nullptr);
            JJoMeSynth::instance().setImsPlayer(nullptr);
            JJoMeSynth::instance().setOkaPlayer(okaPlayer);
            okaPlayer->play();
        } else {
            JJoMeSynth::instance().setGybPlayer(nullptr);
            JJoMeSynth::instance().setImsPlayer(nullptr);
            JJoMeSynth::instance().setOkaPlayer(nullptr);
            midiPlayer->play();
        }
        setPlaying(true);
        positionTimer->start(100);
        updatePlayButton();
    }
    // else: nothing loaded → do nothing
}

// Enter = enter the selected folder, or start playing the selected file.
void MainWindow::activateSelectedPlaylistRow()
{
    int row = plCurrentRow();
    if (row < 0) row = plFirstSelectedRow();
    // No selection (e.g. Enter straight from the search box): take the top row,
    // so search → Enter plays the first match.
    if (row < 0 && plCount() > 0) row = 0;
    if (row < 0) return;
    int itemType = plRowType(row);
    if (itemType == FOLDER || itemType == PARENT_FOLDER) {
        handleFolderDoubleClick(plRowPath(row));
    } else {
        plSetCurrentRow(row);
        onFileDoubleClicked(); // stop (if playing) + play the selected file
    }
}

void MainWindow::onFileSelected()
{
    // FIX: Do not update currentFile, TrackInfo, or ChannelMonitor while playing.
    // This keeps the display locked to the currently playing song even if the user browses the playlist.
    if (isPlaying) return;

    // Remember whether the user is typing in the search box BEFORE the preview
    // work below — lyrics-window updates, monitor mode switches and zip
    // extraction can steal focus mid-way, and we must hand it back afterwards.
    const bool searchTyping = (searchBox && searchBox->hasFocus());

    updateTrackInfo();
    updateTimeDisplay();

    if (plHasCurrent()) {
        QString rawPath = plCurrentPath();
        QString filePath = resolvePlayablePath(rawPath);
        
        bool isGyb = isGybFile(filePath);
        bool isIms = isOplFile(filePath);
        bool isOka = isOkaFile(filePath);

        if (isGyb || isIms || isOka) {
            dspButton->show();
            oplTunnelButton->show();
            bankButton->show();
            updateDspButtonStyle();
        } else {
            dspButton->hide();
            oplTunnelButton->hide();
            bankButton->hide();
            if (channelMonitor) {
                channelMonitor->setImsMode(false);
            }
        }
    }

    if (lyricsWindow && lyricsWindow->isVisible()) {
        if (plHasCurrent()) {
            QString rawPath = plCurrentPath();
            QString filePath = resolvePlayablePath(rawPath);
            bool isNobFile = filePath.toLower().endsWith(".nob");
            if (isNobFile) {
                updateLyricsWindowContent(filePath, isNobFile, false, "onFileSelected");
            }
        }
    }

    // Save current track selection immediately
    SettingsManager& settings = SettingsManager::instance();
    settings.setValue("General/currentTrackIndex", plCurrentRow());
    // Focus policy: if the user was typing in the search box, KEEP/RESTORE the
    // search box focus (preview work above may have stolen it — e.g. lyrics
    // window update when the first match is a NOB). Otherwise the list takes
    // focus as before.
    if (searchTyping) {
        if (searchBox && !searchBox->hasFocus()) {
            searchBox->setFocus();
        }
    } else if (fileList) {
        fileList->setFocus();
    }
}

void MainWindow::onFileDoubleClicked()
{
    if (isPlaying) {
        stop();
    }
    playPause();
}

QString MainWindow::resolvePlayablePath(const QString& path) {
    if (path.contains("::")) {
        if (m_tempZipFile) {
            delete m_tempZipFile;
            m_tempZipFile = nullptr;
        }
        if (m_tempIssFile) {
            delete m_tempIssFile;
            m_tempIssFile = nullptr;
        }
        
        QStringList parts = path.split("::");
        if (parts.size() >= 2) {
            QString zipPath = parts[0];
            QString innerPath = parts[1];
            
            QZipReader zip(zipPath);
            if (zip.status() == QZipReader::NoError) {
                QByteArray data = zip.fileData(innerPath);
                if (!data.isEmpty()) {
                    QFileInfo innerInfo(innerPath);
                    QString tempDir = QCoreApplication::applicationDirPath() + "/temp";
                    QDir().mkpath(tempDir);
                    m_tempZipFile = new QTemporaryFile(tempDir + "/jJomeZip_XXXXXX." + innerInfo.suffix());
                    if (m_tempZipFile->open()) {
                        m_tempZipFile->write(data);
                        m_tempZipFile->close();
                        return m_tempZipFile->fileName();
                    }
                }
            }
        }
    }
    return path;
}

void MainWindow::handleExternalFileLoad(const QString& filePath)
{
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) return;

    // Check if it's a supported format
    QString suffix = fileInfo.suffix().toLower();
    if (suffix != "mid" && suffix != "midi" && suffix != "nob" && suffix != "ims" && suffix != "rol" && suffix != "zip" && suffix != "sop" && suffix != "gyb" && suffix != "oka" && suffix != "okm" && suffix != "vgm" && suffix != "vgz") {
        return;
    }

    // 1. Add file to current node without saving to playlist.json
    addFileToCurrentNodeWithoutSave(filePath);
    
    // 2. Update UI to show the new item
    updateUIFromCurrentNode();
    
    // 3. Find the newly added item in the list widget
    for (int i = 0; i < plCount(); ++i) {
        QString itemPath = plRowPath(i);
        
        if (itemPath == filePath) {
            // Select it and scroll to it
            plSetCurrentRow(i);
            
            // 4. Automatically play it
            onFileDoubleClicked();
            break;
        }
    }
}

void MainWindow::onDeviceChanged(int index)
{
    if (index >= 0 && deviceComboBox->isEnabled()) {
        // If playing, stop completely first to avoid lingering sound
        if (isPlaying) {
            qDebug() << "Switching device during playback - stopping first";
            
            // Stop logic
            midiPlayer->stop(); if(pianoRollWindow) pianoRollWindow->clearNotes();
            imsPlayer->stop(); if(pianoRollWindow) pianoRollWindow->clearNotes();
            okaPlayer->stop(); if(pianoRollWindow) pianoRollWindow->clearNotes();
            JJoMeSynth::instance().setImsPlayer(nullptr);
            JJoMeSynth::instance().setOkaPlayer(nullptr);
            setPlaying(false);
            
            positionTimer->stop();
            updatePlayButton(); // Change button back to PLAY
            
            // Small delay to ensure audio thread and hardware handle the stop
            QThread::msleep(100);
        }

        QString deviceName = deviceComboBox->currentText();
        
        if (deviceName == "[JJoMe Synth (SoundFont)]") {
            SettingsManager& settings = SettingsManager::instance();
            QString extBank = settings.value("Synth/ExternalImsBank", "").toString();
    if (!extBank.isEmpty() && QFileInfo::exists(extBank)) imsPlayer->setExternalBankPath(extBank);
    QString sfPath = settings.value("Synth/SoundFontPath", "").toString();
            
            // Auto-detect a soundfont if none is configured
            if (sfPath.isEmpty() || !QFileInfo::exists(sfPath)) {
                QDir sfDir(QApplication::applicationDirPath() + "/SoundFonts");
                if (sfDir.exists()) {
                    QStringList filters;
                    filters << "*.sf2" << "*.sf3";
                    QFileInfoList fileList = sfDir.entryInfoList(filters, QDir::Files);
                    if (!fileList.isEmpty()) {
                        sfPath = fileList.first().absoluteFilePath();
                        settings.setValue("Synth/SoundFontPath", sfPath);
                        settings.sync();
                        
                        // Show a brief message or just quietly use it
                        qDebug() << "Auto-selected SoundFont:" << sfPath;
                    }
                }
            }
            
            midiPlayer->setUseInternalSynth(true, sfPath);
            midiPlayer->connectToDevice(-1); // Connect to internal synth
        } else if (deviceName == Sc55Bridge::DeviceLabel()) {
            // Never put a modal dialog up while the app is closing, and never
            // let one open on top of another.
            static bool bSc55DialogOpen = false;
            if (m_isShuttingDown || bSc55DialogOpen)
                return;
            struct Guard {
                bool& f;
                explicit Guard(bool& r) : f(r) { f = true; }
                ~Guard() { f = false; }
            } guard(bSc55DialogOpen);

            // Nuked-SC55 over the named pipe. Check first and explain rather
            // than let the launch fail with nothing to go on: the usual causes
            // are "not installed at all" and "mk1 ROMs, which have no serial
            // port to emulate".
            // Fall back to the internal synth when it can't be used. Set the
            // combo directly with signals blocked - calling
            // loadMidiDeviceSettings() here re-entered this slot (it ends by
            // invoking onDeviceChanged itself), and if the *saved* device was
            // also the SC-55 the dialog reappeared forever and the window could
            // not be closed (2026-07-29).
            auto fallBackToInternal = [this]() {
                deviceComboBox->blockSignals(true);
                deviceComboBox->setCurrentIndex(0);   // [JJoMe Synth (SoundFont)]
                deviceComboBox->blockSignals(false);
                SettingsManager::instance().setValue(
                    "General/lastUsedDevice", deviceComboBox->currentText());
                midiPlayer->setUseInternalSynth(
                    true, SettingsManager::instance()
                              .value("Synth/SoundFontPath", "").toString());
                midiPlayer->connectToDevice(-1);
            };

            const QString reason = Sc55Bridge::UnavailableReason();
            if (!reason.isEmpty()) {
                QMessageBox::information(this,
                    LSTR("Nuked SC-55", "Nuked SC-55"), reason);
                fallBackToInternal();
                return;
            }

            midiPlayer->setUseInternalSynth(false);
            if (!midiPlayer->connectToSc55()) {
                QMessageBox::warning(this,
                    LSTR("Nuked SC-55 연결 실패", "Nuked SC-55 Connection Failed"),
                    midiPlayer->sc55Bridge() ? midiPlayer->sc55Bridge()->errorString()
                                             : QString());
                fallBackToInternal();
                return;
            }
        } else {
            midiPlayer->setUseInternalSynth(false);
            // Connect by NAME first: on Win11's new MIDI stack the WinMM device
            // order can differ from the combo snapshot, so index-1 may open the
            // wrong device (user report: picked MT32-PI, heard GS Wavetable).
            bool ok = midiPlayer->connectToDeviceByName(deviceName);
            if (!ok) {
                int actualIndex = index - 1; // legacy fallback (index 0 is virtual synth)
                ok = midiPlayer->connectToDevice(actualIndex);
            }
            if (!ok) {
                QMessageBox::warning(this,
                    LSTR("장치 연결 실패", "Device Connection Failed"),
                    LSTR("MIDI 장치에 연결하지 못했습니다:\n%1\n\n"
                         "장치가 켜져 있는지 확인한 후 R 버튼으로 장치 목록을 "
                         "새로고침하고 다시 선택해 주세요.",
                         "Failed to connect to the MIDI device:\n%1\n\n"
                         "Make sure the device is powered on, then press the R "
                         "button to refresh the device list and select it again.")
                        .arg(deviceName));
            }
        }


        // Save device selection and device name
        SettingsManager& settings = SettingsManager::instance();
        settings.setValue("General/selectedDevice", index);
        settings.setValue("General/lastUsedDevice", deviceName);
        settings.sync();  // Force immediate save
    }
}

void MainWindow::onDeviceRefresh()
{
    // Save current selection
    QString currentDevice = deviceComboBox->currentText();

    // Reload devices
    loadMidiDevices();

    // Try to restore previous selection
    if (!currentDevice.isEmpty() && currentDevice != "No MIDI devices found") {
        int index = deviceComboBox->findText(currentDevice);
        if (index >= 0) {
            deviceComboBox->setCurrentIndex(index);
            onDeviceChanged(index);  // Save the restored selection
        }
    }
    // If previous device not found, fallback to default (index 0)
    else if (deviceComboBox->count() > 0) {
        deviceComboBox->setCurrentIndex(0);
        onDeviceChanged(0);  // Save default selection
    }
}

void MainWindow::loadMidiDevices()
{
    // Block signals to prevent onDeviceChanged from being triggered during device list update
    deviceComboBox->blockSignals(true);

    deviceComboBox->clear();
    QStringList devices = midiPlayer->getAvailableDevices();
    
    // Add internal synth option
    devices.insert(0, "[JJoMe Synth (SoundFont)]");

    // Nuked-SC55, driven directly over a named pipe (sc55/sc55bridge.h) - no
    // virtual MIDI cable needed. Always listed, even when it isn't installed:
    // hiding it entirely would leave no way to discover the feature exists.
    // Picking it while absent explains what to install instead of failing
    // silently (see onDeviceChanged).
    //
    // Make sure the drop folder exists first - it lives under release\, which a
    // rebuild wipes, so there would otherwise be nowhere to put the emulator.
    Sc55Bridge::EnsureInstallDir();
    devices.insert(1, Sc55Bridge::DeviceLabel());

    deviceComboBox->addItems(devices);
    deviceComboBox->setEnabled(true);

    // Re-enable signals
    deviceComboBox->blockSignals(false);

}

void MainWindow::loadMidiDeviceSettings()
{
    SettingsManager& settings = SettingsManager::instance();

    // Load last used MIDI device
    QString lastDeviceName = settings.value("General/lastUsedDevice", "").toString();

    // Block signals to prevent onDeviceChanged from being triggered during loading
    deviceComboBox->blockSignals(true);

    bool deviceFound = false;

    // Only try to restore if we have a saved device name
    if (!lastDeviceName.isEmpty()) {
        // Try to find the exact device by name
        for (int i = 0; i < deviceComboBox->count(); ++i) {
            if (deviceComboBox->itemText(i) == lastDeviceName) {
                deviceComboBox->setCurrentIndex(i);
                deviceFound = true;
                break;
            }
        }
    }

    // If no saved device or saved device not found, select first available device for first run
    if (!deviceFound && deviceComboBox->count() > 0 &&
        deviceComboBox->itemText(0) != "No MIDI devices found") {
        deviceComboBox->setCurrentIndex(0);
        deviceFound = true;
    }

    // Re-enable signals
    deviceComboBox->blockSignals(false);

    // Connect to the selected MIDI device
    if (deviceFound) {
        int currentDeviceIndex = deviceComboBox->currentIndex();
        if (currentDeviceIndex >= 0) {
            // Call onDeviceChanged to properly route JJoMeSynth vs external devices
            onDeviceChanged(currentDeviceIndex);
            
            // Save the automatically selected device for first run
            SettingsManager& settingsSave = SettingsManager::instance();
            settingsSave.setValue("General/selectedDevice", currentDeviceIndex);
            settingsSave.setValue("General/lastUsedDevice", deviceComboBox->currentText());
            settingsSave.sync();
        }
    }

}

bool MainWindow::isOplFile(const QString& filePath) const
{
    QString lower = filePath.toLower();
    return lower.endsWith(".ims") || lower.endsWith(".rol") || lower.endsWith(".sop") || lower.endsWith(".vgm") || lower.endsWith(".vgz");
}

bool MainWindow::isGybFile(const QString& filePath) const
{
    return filePath.toLower().endsWith(".gyb");
}

bool MainWindow::isOkaFile(const QString& filePath) const
{
    return OkaFileHandler::isOkaFile(filePath);
}

bool MainWindow::isOkaOplFile(const QString& filePath) const
{
    return isOkaFile(filePath) && filePath.toLower().endsWith(".oka");
}

void MainWindow::toggleRecording() {
    JJoMeSynth& synth = JJoMeSynth::instance();
    if (synth.isRecording()) {
        synth.stopRecording();
        recordButton->setText("⏺");
        recordButton->setStyleSheet(
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
            "    border: 2px solid #ff4444;"
            "}"
        );
        // 재생 중일 때 녹음 중지를 명시적으로 누른 것이라면, 재생 중엔 다시 녹음을 시작할 수 없으므로 비활성화
        if (isPlaying) {
            recordButton->setEnabled(false);
        }
    } else {
        // 재생 중일 때는 녹음을 시작할 수 없음
        if (isPlaying) {
            return;
        }

        QString appDir = QApplication::applicationDirPath();
        QString recDirPath = QDir(appDir).absoluteFilePath("rec");
        QDir().mkpath(recDirPath);
        
        QString rawFile;
        if (!currentRawPath.isEmpty()) {
            rawFile = currentRawPath;
        } else if (plHasCurrent()) {
            rawFile = plCurrentPath();
        }

        QString baseName = "record";
        if (!rawFile.isEmpty()) {
            if (rawFile.contains("::")) {
                QStringList parts = rawFile.split("::");
                if (parts.size() >= 2) {
                    baseName = QFileInfo(parts[1]).completeBaseName();
                } else {
                    baseName = QFileInfo(rawFile).completeBaseName();
                }
            } else {
                baseName = QFileInfo(rawFile).completeBaseName();
            }
        }
        
        QString timeStr = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
        QString filePath = QDir(recDirPath).absoluteFilePath(QString("%1_%2.wav").arg(baseName, timeStr));
        
        if (synth.startRecording(filePath)) {
            recordButton->setText("🛑");
            recordButton->setStyleSheet(
                "QPushButton {"
                "    font-size: 16px;"
                "    border: 1px solid #ff4444;"
                "    border-radius: 3px;"
                "    background-color: #5a2a2a;"
                "    color: #ff4444;"
                "    padding: 0px;"
                "}"
            );
        } else {
            QMessageBox::warning(this, "Error", "Failed to start recording!");
        }
    }
}

void MainWindow::forceChannelUpdate()
{
    // Stop the single-shot timer
    channelUpdateTimer->stop();

    // Force update all channel programs by triggering a refresh
    if (channelMonitor && channelMonitor->isVisible()) {
        channelMonitor->refreshActiveChannels();

        // Schedule another update after a bit more time for late-arriving program changes
        QTimer::singleShot(300, [this]() {
            if (channelMonitor && channelMonitor->isVisible()) {
                channelMonitor->refreshActiveChannels();
            }
        });
    }
}

// Channel state tracking implementations
void MainWindow::onNoteOn(int channel, int note, int velocity)
{
    if (channel < 0 || channel >= 16) return;

    channelIsActive[channel] = true;

    // Forward to channel monitor if it exists
    if (channelMonitor) {
        channelMonitor->onNoteOn(channel, note, velocity);
    }
}

void MainWindow::onNoteOff(int channel, int note)
{
    if (channel < 0 || channel >= 16) return;

    // Forward to channel monitor if it exists
    if (channelMonitor) {
        channelMonitor->onNoteOff(channel, note);
    }
}

void MainWindow::onProgramChange(int channel, int program)
{
    if (channel < 0 || channel >= 16) return;

    channelPrograms[channel] = program;
    channelHasProgram[channel] = true;
    channelIsActive[channel] = true;

    // Forward to channel monitor if it exists
    if (channelMonitor) {
        channelMonitor->onProgramChange(channel, program);
    }
}

void MainWindow::onControllerChange(int channel, int controller, int value)
{
    if (channel < 0 || channel >= 16) return;

    // Volume controller makes channel active
    if (controller == 7 || controller == 11) {
        channelIsActive[channel] = true;
    }

    // Forward to channel monitor if it exists
    if (channelMonitor) {
        channelMonitor->onControllerChange(channel, controller, value);
    }
}
