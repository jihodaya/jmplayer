#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListView>
#include <QSortFilterProxyModel>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QTimer>
#include <QProgressBar>
#include <QComboBox>
#include <QLineEdit>
#include <QSettings>
#include "settingsmanager.h"
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMoveEvent>
#include <QDirIterator>
#include <QSet>
#include <QLocalServer>
#include <QLocalSocket>
#include "midiplayer.h"
#include "issfilehandler.h"

// Forward declaration
class ChannelMonitor;
class LyricsWindow;
class PianoRollWindow;
class OkaPlayer;
class PlaylistModel;

// Playlist tree structure
struct PlaylistTreeNode {
    QString name;           // Display name
    QString fullPath;       // Full file/folder path (empty for virtual nodes)
    bool isFolder;          // true for folders, false for files
    bool isVirtual;         // true for virtual nodes (like root)
    PlaylistTreeNode* parent;
    QList<PlaylistTreeNode*> children;

    PlaylistTreeNode(const QString &n = "", const QString &path = "", bool folder = false, bool virt = false)
        : name(n), fullPath(path), isFolder(folder), isVirtual(virt), parent(nullptr) {}

    ~PlaylistTreeNode() {
        qDeleteAll(children);
    }
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    enum PlaylistItemType {
        MIDI_FILE = 0,
        FOLDER = 1,
        PARENT_FOLDER = 2
    };

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void openFile();
    void openFolder();
    void removeFile();
    void sortFiles();
    void savePlaylist();
    void loadPlaylist();
    void showPlaylistMenu();
    void toggleChannelMonitor();
    void toggleLyricsWindow();
    void onLyricsEdited(const QStringList& newLyrics);
    void playPause();
    void stop();

    // Channel state tracking slots
    void onNoteOn(int channel, int note, int velocity);
    void onNoteOff(int channel, int note);
    void onProgramChange(int channel, int program);
    void onControllerChange(int channel, int controller, int value);
    void previousTrack();
    void nextTrack();
    void rewind();
    void fastForward();
    void onVolumeChanged(int value);
    void onPositionChanged(int value);
    void onFileSelected();
    void onFileDoubleClicked();
    void updatePosition();
    void forceChannelUpdate();
    void checkWindowPosition();
    void onDeviceChanged(int index);
    void onDeviceRefresh();
    void onCleanupPlaylist();
    void onSearchTextChanged();
    void onRepeatModeChanged();
    void onPlaybackFinished();
    void showFileInfo(const QString &filePath);
    void addMidiFiles(const QStringList &filePaths);
    QStringList findMidiFilesInDirectory(const QString &dirPath);
    void addFolderToPlaylist(const QString &folderPath);
    void navigateToFolder(const QString &folderPath);
    void navigateToFolderWithoutHistory(const QString &folderPath);
    void handleFolderDoubleClick(const QString &folderPath);
    QString getCurrentPath() const;
    void setCurrentPath(const QString &path);
    void updateAllowedPaths();
    bool isPathAllowed(const QString &path) const;
    bool isOplFile(const QString& filePath) const;
    bool isGybFile(const QString& filePath) const;
    bool isOkaFile(const QString& filePath) const;
    bool isOkaOplFile(const QString& filePath) const;
    // True when this .GYB/.OKA is set to play through a MIDI module rather than
    // the OPL engine. GAYOBANG and NORE45 both offered this; see gybokamidi.h.
    bool playsViaMidi(const QString& filePath) const;
    QString patchTargetPath();
    void toggleMidiMode();
    void updateMidiModeButtonStyle();
    void showPatchDialog();

    // The MT-32's front panel (mt32display.h). Created the first time it is
    // wanted and kept afterwards, so the user's chosen ROM and window position
    // survive switching devices back and forth. Shown only while the MT-32 is
    // the selected device.
    void showMt32Display(bool show);

    // Makes a folder scan's placeholder row report how far it has got.
    void wireScanProgress(class FolderScanner* scanner, PlaylistTreeNode* loadingNode);

    void reloadCurrentSong(bool keepPosition = false);
    void onLyricChannelChanged(int newChannel); // NOB 채널 변경
    void updateWindowTitle();
    void toggleDsp();
    void togglePianoRoll();
    void updateDspButtonStyle();
    void toggleOplTunnel();
    void updateOplTunnelButtonStyle();
    void ensureJJoMeSynthReady();
    void onSelectBankFile();

    // Unregister the external OPL bank for the format in play and reload the
    // current song, so per-song .BNK files apply again.
    void clearExternalBank(bool bIsGyb, bool bIsOka);

    // Re-send bank name and instrument list to the channel monitor after a
    // mid-song bank change.
    void refreshOplChannelMonitor();

    // One-time cleanup of GYB/OKA external-bank settings, which no longer
    // apply and can no longer be cleared from the UI.
    void dropObsoleteOplBankSettings();
    void showHelpDialog();
    void toggleRecording();

private:
    QString resolvePlayablePath(const QString& path);
    void setupUI();
    void setupMenuBar();
    void connectSignals();
    void updatePlayButton();
    void loadMidiDevices();
    void updateTrackInfo();
    QStringList loadLyricsForNob(const QString& filePath, bool *usedExternal = nullptr) const;
    bool updateLyricsWindowContent(const QString& filePath, bool isNobFile, bool updateMarkers, const char* contextTag);
    void updateTimeDisplay();
    QString formatTime(unsigned long milliseconds);

    // Helper functions
    QString getActualExecutablePath();

    // Settings management
    void saveSettings();
    void loadSettings();
    void loadMidiDeviceSettings();
    // QSettings* createSettings(); // Replaced with SettingsManager singleton

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void moveEvent(QMoveEvent *event) override;

    // UI Components
    QWidget *centralWidget;
    QVBoxLayout *mainLayout;

    // File list (model/view: scales to 100k+ rows; only visible delegates render)
    QListView *fileList;
    PlaylistModel *playlistModel = nullptr;
    QSortFilterProxyModel *playlistProxy = nullptr;

    // Control buttons
    QHBoxLayout *buttonLayout;
    QPushButton *fileButton;
    QPushButton *folderButton;
    QPushButton *removeButton;
    QPushButton *sortButton;
    QPushButton *playlistButton;

    // Playback controls
    QHBoxLayout *playbackLayout;
    QPushButton *rewButton;
    QPushButton *previousButton;
    QPushButton *playButton;
    QPushButton *stopButton;
    QPushButton *nextButton;
    QPushButton *fwButton;
    QPushButton *repeatModeButton;
    QPushButton *channelButton;
    QPushButton *lyricsButton;
    QPushButton *dspButton;
    QPushButton *oplTunnelButton; // OPL register tunnel to the jukebox (shown with DSP)
    QPushButton *midiModeButton;  // .GYB/.OKA through MIDI instead of OPL (F5 edits the mapping)
    QPushButton *rollButton;
    QPushButton *bankButton;
    QPushButton *recordButton;

    // Volume control
    QHBoxLayout *volumeLayout;
    QLabel *volumeLabel;
    QSlider *volumeSlider;
    QLabel *volumeValue;

    // Progress control
    QSlider *progressSlider;
    QLabel *positionLabel;

    // Track info
    QLabel *trackInfoLabel;

    // Time display
    QLabel *timeDisplayLabel;

    // Device selection
    QHBoxLayout *deviceLayout;
    QPushButton *deviceRefreshButton;
    QPushButton *cleanupButton;
    QComboBox *deviceComboBox;
    QLineEdit *searchBox;

    // MIDI Player
    MidiPlayer *midiPlayer;
    class ImsPlayer *imsPlayer;
    class GybPlayer *gybPlayer;
    class OkaPlayer *okaPlayer;

    // Timer for position updates
    QTimer *positionTimer;
    QTimer *channelUpdateTimer;
    QTimer *previewSelectTimer = nullptr; // debounces the heavy per-selection preview load
    QTimer *playlistSaveTimer = nullptr; // debounces heavy playlist JSON saves
    QTimer *volumeSaveTimer = nullptr; // debounces the settings write while dragging the volume slider
    QTimer *searchDebounceTimer = nullptr; // debounces global playlist search

    // Timer for window position tracking
    QTimer *windowPositionTimer;
    QPoint lastWindowPosition;
    
    // IPC Server for single instance
    QLocalServer *ipcServer;

private slots:
    void handleNewIpcConnection();
public:
    void handleExternalFileLoad(const QString& filePath);
    void setPlaying(bool playing);

private:
    // MT-32 front panel, owned by this window (Qt parent) so it closes with it.
    class Mt32Display* m_pMt32Display = nullptr;
    // The song to start again once a ROM swap has finished. Empty when nothing
    // was playing, which is what makes the swap a no-op for the transport.
    QString m_mt32RomReloadPath;

    // Placeholder nodes standing in for a folder still being scanned. Their
    // names are a live progress counter, so savePlaylistTree() must not write
    // them out - "chiptune-midi (12,431 / 114,727)" would otherwise be the name
    // in playlist.json for good.
    QSet<PlaylistTreeNode*> m_scanningNodes;

    // Current state
    bool isPlaying;
    bool m_isShuttingDown;
    QString currentFile;
    QString currentRawPath;
    bool m_pausedByUser = false; // true only after Space-pause, so Space resume ≠ play-after-stop
    QTemporaryFile* m_tempZipFile = nullptr;
    QTemporaryFile* m_tempIssFile = nullptr;
    bool isUserDragging;
    bool sortAscending; // Track sort order for toggle functionality
    int repeatMode; // 0: play once, 1: repeat current, 2: repeat all, 3: shuffle
    QString currentFolderPath; // Current folder path for navigation
    QStringList navigationHistory; // History for back navigation
    bool isInBrowsingMode; // True when browsing a specific folder, false when in playlist mode
    QString browsingRootPath; // Root path limit when browsing from playlist folder
    QStringList allowedPaths; // Paths that are allowed for navigation (based on playlist)
    QStringList shuffleHistory; // History of played tracks in shuffle mode for PREV back navigation
    
    // Special constant for playlist root
    static const QString PLAYLIST_ROOT;

    // New playlist tree management system
    PlaylistTreeNode* playlistRoot;     // Virtual root of playlist tree
    PlaylistTreeNode* currentNode;      // Current position in tree

    // Channel monitor window
    ChannelMonitor* channelMonitor;

    // Lyrics window
    LyricsWindow* lyricsWindow;

    // Piano Roll window
    PianoRollWindow* pianoRollWindow;
    QString currentNobFilePath; // Current NOB file path being played
    QList<unsigned long> currentLyricMarkerTicks; // Marker tick list per lyric syllable
    QList<unsigned long> currentLyricMarkerMs;    // Marker ms list for IMS (ISS)
    QList<MidiPlayer::MarkerEvent> currentMarkerEvents; // Raw marker events for recalculation
    QStringList currentLyrics; // Currently displayed lyrics
    int lastDisplayedLyricIndex; // Last highlighted lyric index

    // ISS (IMS 가사) 데이터
    IssFileHandler::IssData currentIssData;
    int lastIssLineIdx;  // 현재 표시 중인 ISS 원본 줄 번호 (-1=초기)

    // Channel state tracking (always active)
    int channelPrograms[16];        // Current program for each channel
    bool channelHasProgram[16];     // Whether channel has received program change
    bool channelIsActive[16];       // Whether channel is currently active

    // Tree management functions
    void initializePlaylistTree();
    void addFileToCurrentNode(const QString &filePath);
    void addFolderToCurrentNode(const QString &folderPath);
    void addFileToCurrentNodeWithoutSave(const QString &filePath);

    // Portable mode only: rescan the Music folder next to the executable so
    // files dropped onto the stick show up on the next launch.
    void refreshPortableMusicFolder();
    bool m_portableMusicScanRunning = false;

    // Set just before a save that the user's own deletion may legitimately have
    // emptied; savePlaylistTree otherwise refuses to write an empty list over a
    // populated one. Cleared by that save.
    bool m_allowEmptyPlaylistSave = false;
    void addFolderToCurrentNodeWithoutSave(const QString &folderPath);
    void navigateToNode(PlaylistTreeNode* node);
    void updateUIFromCurrentNode();
    // Global search: walks the WHOLE playlist tree and lists every matching file
    // (with its folder for context). Empty text restores the current folder view.
    void performGlobalSearch(const QString& searchText);

    // --- Playlist view bridge helpers (operate on proxy/visible rows, mirroring
    //     the old QListWidget row API so call sites stay 1:1) ---
    int     plCount() const;                 // visible row count
    int     plCurrentRow() const;            // -1 if none
    void    plSetCurrentRow(int row);        // select + scroll into view
    int     plRowType(int row) const;        // PlaylistItemType (MIDI_FILE default)
    QString plRowPath(int row) const;        // file/folder path of row
    QString plRowText(int row) const;        // display text of row
    int     plCurrentType() const;
    QString plCurrentPath() const;
    QString plCurrentText() const;
    bool    plHasCurrent() const { return plCurrentRow() >= 0; }
    int     plFirstSelectedRow() const;      // lowest selected row, -1 if none

    // Prev/Next/Repeat/Shuffle must navigate relative to the PLAYING file
    // (currentRawPath), not the highlighted row, even after the user clicks
    // away or browses into another folder.
    int  plRowOfPlayingFile() const;         // row of currentRawPath in current view, -1 if absent
    PlaylistTreeNode* findParentNodeOfFile(const QString& filePath) const;
    // Ordered playable paths in the PLAYING file's folder (node), independent of
    // the displayed view; *playingIndex = index of the playing file (-1 if none).
    QStringList playingQueue(int* playingIndex) const;
    // Load+play a file by its raw playlist path WITHOUT moving the view; only
    // highlights the row if it already happens to be visible. Returns success.
    bool loadAndPlayByRawPath(const QString& rawPath);

    // Keyboard helpers (Space = pause/resume only; Enter = play file / enter folder;
    // Up/Down browse the playlist while stopped).
    void spacePauseResume();             // pause if playing, resume current if paused, else nothing
    void activateSelectedPlaylistRow();  // Enter: enter folder or play selected file
    void movePlaylistSelection(int delta); // move selection by delta rows (browsing while stopped)
    void savePlaylistTree();
    void triggerSavePlaylistTree();
    void loadPlaylistTree();
    PlaylistTreeNode* findNodeByPath(const QString &path);
    void addFolderStructureToTree(PlaylistTreeNode* parentNode, const QString &folderPath);
    void removeItemFromCurrentNode(const QString &itemPath);
    void savePlaylistToFile(const QString &filePath);
    void loadPlaylistFromFile(const QString &filePath);
    QJsonObject nodeToJson(PlaylistTreeNode* node);
    PlaylistTreeNode* nodeFromJson(const QJsonObject &json, PlaylistTreeNode* parent = nullptr);
    void validateAndCleanPlaylistTree(PlaylistTreeNode* node);
    void resetLyricSyncState();
    QStringList expandLyricsForRepeat(const QStringList& originalLyrics, bool isNobFile) const;
    // Trim intro notes off a guide channel that plays from the top of the song.
    // Returns how many were removed; modifies the list in place.
    int dropIntroMarkers(QList<MidiPlayer::MarkerEvent>& markers) const;

    QList<MidiPlayer::MarkerEvent> adjustMarkersForLyrics(const QList<MidiPlayer::MarkerEvent>& markers,
                                                          const QStringList& lyrics) const;
};

#endif // MAINWINDOW_H
