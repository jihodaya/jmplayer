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
#include <QElapsedTimer>
#include <algorithm>
#include <climits>
#include "playlistmodel.h"

// Playlist Delegate for custom column alignment
class PlaylistDelegate : public QStyledItemDelegate {
public:
    PlaylistDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        // Draw selection background
        if (opt.state & QStyle::State_Selected) {
            painter->fillRect(opt.rect, QColor(0, 120, 212)); // Selection color
        } else if (opt.state & QStyle::State_MouseOver) {
            painter->fillRect(opt.rect, QColor(74, 74, 74)); // Hover color
        }

        QString fullText = opt.text;
        
        // Handle parent folder differently
        if (fullText.contains(" (Parent Folder)")) {
            painter->setPen(Qt::white);
            painter->drawText(opt.rect.adjusted(5, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, fullText);
            painter->restore();
            return;
        }

        // Split "??Filename" and " - Title - Artist"
        int firstDash = fullText.indexOf(" - ");
        QString filenamePart = (firstDash != -1) ? fullText.left(firstDash) : fullText;
        QString titlePart = (firstDash != -1) ? fullText.mid(firstDash) : "";

        // Drawing rectangles. The 130px filename column only matters when a
        // " - Title - Artist" part follows; otherwise let the filename use the
        // whole row so it isn't needlessly truncated in a wide window.
        int filenameWidth = 130;
        QRect filenameRect = opt.rect.adjusted(5, 0, 0, 0);
        if (!titlePart.isEmpty()) {
            filenameRect.setWidth(filenameWidth);
        }
        
        QRect titleRect = opt.rect;
        titleRect.setLeft(opt.rect.left() + filenameWidth + 5);

        // Set pen color
        painter->setPen(Qt::white);

        // Draw filename (elide with "..." instead of hard pixel clipping: a
        // clipped 'm' in ".mid" looked like ".r"/".rr" on narrow widths)
        painter->drawText(filenameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          opt.fontMetrics.elidedText(filenamePart, Qt::ElideRight, filenameRect.width()));

        // Draw title/artist if exists
        if (!titlePart.isEmpty()) {
            painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                              opt.fontMetrics.elidedText(titlePart, Qt::ElideRight, titleRect.width()));
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        return QSize(s.width(), 24); // Slightly larger items
    }
};

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

// Define the playlist root constant
const QString MainWindow::PLAYLIST_ROOT = "__PLAYLIST_ROOT__";

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , isPlaying(false)
    , m_isShuttingDown(false)
    , isUserDragging(false)
    , sortAscending(true)
    , repeatMode(0)
    , isInBrowsingMode(false)
    , playlistRoot(nullptr)
    , currentNode(nullptr)
    , channelMonitor(nullptr)
    , lyricsWindow(nullptr)
    , pianoRollWindow(nullptr)
    , lastDisplayedLyricIndex(-1)
    , lastIssLineIdx(-1)
    , ipcServer(new QLocalServer(this))
{
    // Set global error mode to suppress all Windows error dialogs and exceptions
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX | SEM_NOALIGNMENTFAULTEXCEPT);
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX | SEM_NOALIGNMENTFAULTEXCEPT, NULL);
    SetUnhandledExceptionFilter(NULL);

    // Start IPC Server
    const QString serverName = "JMPlayer_IPC_Server";
    QLocalServer::removeServer(serverName); // Clean up any stale server
    ipcServer->listen(serverName);
    connect(ipcServer, &QLocalServer::newConnection, this, &MainWindow::handleNewIpcConnection);

    // Initialize channel state tracking
    for (int i = 0; i < 16; ++i) {
        channelPrograms[i] = 0;
        channelHasProgram[i] = (i == 9); // Drum channel always has program
        channelIsActive[i] = false;
    }

    midiPlayer = new MidiPlayer(this);
    imsPlayer = new ImsPlayer(this);
    gybPlayer = new GybPlayer(this);
    okaPlayer = new OkaPlayer(this);
    positionTimer = new QTimer(this);
    channelUpdateTimer = new QTimer(this);
    windowPositionTimer = new QTimer(this);

    // Debounce the playlist saves
    playlistSaveTimer = new QTimer(this);
    playlistSaveTimer->setSingleShot(true);
    playlistSaveTimer->setInterval(2000);
    connect(playlistSaveTimer, &QTimer::timeout, this, &MainWindow::savePlaylistTree);

    // Debounce the per-selection preview (file parse + duration prescan). While
    // the user scrolls the playlist quickly, we only run the heavy preview once
    // they settle on an item — this removes the navigation jank.
    previewSelectTimer = new QTimer(this);
    previewSelectTimer->setSingleShot(true);
    previewSelectTimer->setInterval(180);
    connect(previewSelectTimer, &QTimer::timeout, this, &MainWindow::onFileSelected);

    setupUI();
    setupMenuBar();
    connectSignals();
    loadMidiDevices();
    // Settings will be loaded after UI initialization via QTimer

    // Set window icon
    QString iconPath = QApplication::applicationDirPath() + "/K_icon.ico";
    QIcon icon(iconPath);
    if (icon.isNull()) {
        // Fallback to resource path
        icon = QIcon(":/K_icon.ico");
    }
    setWindowIcon(icon);

    // Set window title with musical note
    updateWindowTitle();

    // Enable dark titlebar on Windows
    #ifdef _WIN32
    if (winId()) {
        HWND hwnd = (HWND)winId();
        BOOL value = TRUE;
        ::DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    }
    #endif

    // The original detailed dark theme is already applied in setupUI()
    // This ensures we don't override it

    positionTimer->setInterval(100); // Update every 100ms

    // Load saved settings (use new tree system)
    loadPlaylistTree();

    // Ensure settings are loaded after UI is fully initialized
    QTimer::singleShot(0, this, &MainWindow::loadSettings);

    // Check for initial arguments (file dropped on exe or double-clicked)
    QTimer::singleShot(500, this, [this]() {
        QStringList args = QApplication::arguments();
        if (args.size() > 1) {
            handleExternalFileLoad(args.at(1));
        }
    });

    // Make window appear on top when program starts - ensure visibility
    QTimer::singleShot(50, this, [this]() {
        show();
        raise();
        #ifdef _WIN32
        if (winId()) {
            HWND hwnd = (HWND)winId();
            // Force window to front with topmost temporarily
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            BringWindowToTop(hwnd);
        }
        #endif
    });

    // Additional attempt to ensure window is visible
    QTimer::singleShot(200, this, [this]() {
        raise();
        #ifdef _WIN32
        if (winId()) {
            HWND hwnd = (HWND)winId();
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        }
        #endif
    });
}

MainWindow::~MainWindow()
{
    if (m_tempZipFile) delete m_tempZipFile;
    if (m_tempIssFile) delete m_tempIssFile;
    delete playlistRoot;
}


void MainWindow::setupUI()
{
    centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    mainLayout = new QVBoxLayout(centralWidget);

    // Device selection
    deviceLayout = new QHBoxLayout();
    deviceLayout->setSpacing(5);  // Reduce spacing
    deviceLayout->setContentsMargins(0, 0, 0, 0);  // Remove margins
    deviceComboBox = new QComboBox(this);
    deviceRefreshButton = new QPushButton("R", this);
    deviceRefreshButton->setFocusPolicy(Qt::NoFocus);
    cleanupButton = new QPushButton("L", this);
    cleanupButton->setFocusPolicy(Qt::NoFocus);

    // Set button to optimal height
    deviceRefreshButton->setFixedSize(30, 22);
    deviceRefreshButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    deviceRefreshButton->setMaximumHeight(22);
    deviceRefreshButton->setMinimumHeight(22);

    cleanupButton->setFixedSize(30, 22);
    cleanupButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    cleanupButton->setMaximumHeight(22);
    cleanupButton->setMinimumHeight(22);

    // Set font for button
    QFont refreshFont("Arial", 12, QFont::Bold);
    deviceRefreshButton->setFont(refreshFont);
    cleanupButton->setFont(refreshFont);

    // Set clean white text style
    deviceRefreshButton->setStyleSheet("QPushButton { color: white; max-height: 22px; min-height: 22px; height: 22px; padding: 0px; margin: 0px; }");
    deviceRefreshButton->setToolTip("Refresh MIDI devices");

    cleanupButton->setStyleSheet("QPushButton { color: white; max-height: 22px; min-height: 22px; height: 22px; padding: 0px; margin: 0px; }");
    cleanupButton->setToolTip("Refresh playlist");

    // Search box
    searchBox = new QLineEdit(this);
    searchBox->setPlaceholderText("Search playlist...");
    searchBox->setMaximumHeight(22);
    searchBox->setMinimumHeight(22);
    searchBox->setClearButtonEnabled(true);

    // SoundFont Manager Button
    QPushButton *sfManagerButton = new QPushButton(QString::fromUtf8("\xE2\x9A\x99\xEF\xB8\x8F"), this); // ?숋툘
    sfManagerButton->setFocusPolicy(Qt::NoFocus);
    sfManagerButton->setFixedSize(30, 22);
    sfManagerButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    sfManagerButton->setMaximumHeight(22);
    sfManagerButton->setMinimumHeight(22);
    sfManagerButton->setStyleSheet("QPushButton { color: white; max-height: 22px; min-height: 22px; height: 22px; padding: 0px; margin: 0px; background-color: transparent; border: none; }");
    sfManagerButton->setToolTip("Manage SoundFonts");
    
    connect(sfManagerButton, &QPushButton::clicked, this, [this]() {
        SettingsManager& settings = SettingsManager::instance();
        const QString beforeSf = settings.value("Synth/SoundFontPath", "").toString();

        SoundFontManagerDialog dialog(this);
        dialog.exec();

        if (deviceComboBox->currentText() != "[JJoMe Synth (SoundFont)]") return;

        const QString activeSf = settings.value("Synth/SoundFontPath", "").toString();
        if (activeSf.isEmpty()) return;

        // Nothing actually changed — leave the synth (and any playback) alone.
        if (QDir::cleanPath(activeSf).compare(QDir::cleanPath(beforeSf), Qt::CaseInsensitive) == 0) {
            updateWindowTitle();
            return;
        }

        // Applying a new SoundFont tears down and re-creates the shared audio
        // device + synth (JJoMeSynth::initialize → shutdown). Doing that while
        // audio is rendering breaks the sound and desyncs channel programs, so:
        // stop → apply → reload the same song → seek back → resume.
        const bool wasPlaying = isPlaying;
        const QString resumePath = !currentRawPath.isEmpty() ? currentRawPath : currentFile;
        unsigned long resumeMs = 0;
        if (wasPlaying) {
            if (isGybFile(currentFile))           resumeMs = gybPlayer->getPosition();
            else if (isOplFile(currentFile))      resumeMs = imsPlayer->getPosition();
            else if (isOkaOplFile(currentFile))   resumeMs = okaPlayer->getPosition();
            else                                  resumeMs = midiPlayer->getCurrentPosition();
            stop();
        }

        midiPlayer->setUseInternalSynth(true, activeSf);
        updateWindowTitle();

        if (wasPlaying && !resumePath.isEmpty() && loadAndPlayByRawPath(resumePath)) {
            // Seek back once the engine is rolling (players accept ms seeks
            // while playing — same path the progress slider uses).
            QTimer::singleShot(350, this, [this, resumeMs]() {
                if (!isPlaying || resumeMs == 0) return;
                if (isGybFile(currentFile))           gybPlayer->setPosition(resumeMs);
                else if (isOplFile(currentFile))      imsPlayer->setPosition(resumeMs);
                else if (isOkaOplFile(currentFile))   okaPlayer->setPosition(resumeMs);
                else                                  midiPlayer->setPosition(resumeMs);
            });
        }
    });

    // Help Button
    QPushButton *helpButton = new QPushButton(QString::fromUtf8("F1"), this);
    helpButton->setFocusPolicy(Qt::NoFocus);
    helpButton->setFixedSize(30, 22);
    helpButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    helpButton->setMaximumHeight(22);
    helpButton->setMinimumHeight(22);
    helpButton->setStyleSheet("QPushButton { color: white; background-color: #3a3a3a; border: 1px solid #555555; border-radius: 3px; max-height: 22px; min-height: 22px; height: 22px; padding: 0px 5px; margin: 0px; } "
                              "QPushButton:hover { background-color: #505050; } "
                              "QPushButton:pressed { background-color: #2b2b2b; }");
    helpButton->setToolTip("Show help dialog");
    connect(helpButton, &QPushButton::clicked, this, &MainWindow::showHelpDialog);

    deviceLayout->addWidget(deviceComboBox);
    deviceLayout->addWidget(sfManagerButton, 0, Qt::AlignVCenter);
    deviceLayout->addWidget(deviceRefreshButton, 0, Qt::AlignVCenter);  // Center vertically
    deviceLayout->addWidget(cleanupButton, 0, Qt::AlignVCenter);  // Center vertically
    deviceLayout->addSpacing(10);  // Add spacing between L button and search box
    deviceLayout->addWidget(searchBox);
    deviceLayout->addSpacing(5);
    deviceLayout->addWidget(helpButton, 0, Qt::AlignVCenter);
    mainLayout->addLayout(deviceLayout);

    // File control buttons
    buttonLayout = new QHBoxLayout();
    fileButton = new QPushButton("File", this);
    fileButton->setFocusPolicy(Qt::NoFocus);
    folderButton = new QPushButton("Folder", this);
    folderButton->setFocusPolicy(Qt::NoFocus);
    removeButton = new QPushButton("Remove", this);
    removeButton->setFocusPolicy(Qt::NoFocus);
    sortButton = new QPushButton("Sort", this);
    sortButton->setFocusPolicy(Qt::NoFocus);
    playlistButton = new QPushButton("Playlist", this);
    playlistButton->setFocusPolicy(Qt::NoFocus);

    // Let buttons expand evenly across the available width
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(8);
    fileButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    folderButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    removeButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sortButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    playlistButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    buttonLayout->setStretch(0, 1);
    buttonLayout->setStretch(1, 1);
    buttonLayout->setStretch(2, 1);
    buttonLayout->setStretch(3, 1);
    buttonLayout->setStretch(4, 1);
    buttonLayout->addWidget(fileButton);
    buttonLayout->addWidget(folderButton);
    buttonLayout->addWidget(removeButton);
    buttonLayout->addWidget(sortButton);
    buttonLayout->addWidget(playlistButton);

    mainLayout->addLayout(buttonLayout);

    // File list (model/view). PlaylistModel holds a flat vector; the QListView
    // only renders visible delegates, so this scales to 100k+ rows. A proxy in
    // front provides instant search filtering.
    playlistModel = new PlaylistModel(this);
    playlistProxy = new QSortFilterProxyModel(this);
    playlistProxy->setSourceModel(playlistModel);
    playlistProxy->setFilterRole(Qt::DisplayRole);
    playlistProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    playlistProxy->setDynamicSortFilter(false); // preserve folders-then-files order

    fileList = new QListView(this);
    fileList->setModel(playlistProxy);
    fileList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    fileList->setAcceptDrops(true);
    fileList->setItemDelegate(new PlaylistDelegate(this));
    fileList->setUniformItemSizes(true);          // large-list fast path
    fileList->setLayoutMode(QListView::Batched);  // non-blocking layout
    fileList->setBatchSize(256);
    fileList->setResizeMode(QListView::Adjust);
    fileList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    fileList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // Set playlist style with white text and white emoji filter
    fileList->setStyleSheet(
        "QListView {"
        "    background-color: #2b2b2b;"
        "    color: white;"
        "    border: 1px solid #555555;"
        "    selection-background-color: #0078d4;"
        "    selection-color: white;"
        "    outline: none;"
        "}"
        "QListView::item {"
        "    padding: 4px;"
        "    border-bottom: 1px solid #3a3a3a;"
        "    color: white;"
        "}"
        "QListView::item:hover {"
        "    background-color: #4a4a4a;"
        "    color: white;"
        "}"
        "QListView::item:selected {"
        "    background-color: #0078d4;"
        "    color: white;"
        "}"
    );

    mainLayout->addWidget(fileList);

    // Enable drag and drop for main window
    setAcceptDrops(true);

    // File name display (vanBasco style title bar)
    trackInfoLabel = new QLabel("No file selected", this);
    trackInfoLabel->setAlignment(Qt::AlignCenter);
    trackInfoLabel->setStyleSheet("font-family: Arial; color: #00FFFF; background-color: #000000; padding: 8px; border: 2px inset #666666;");
    trackInfoLabel->setFixedHeight(UI::MainWindow::TRACK_INFO_HEIGHT); // Fixed height to prevent resizing, larger than file info
    trackInfoLabel->setWordWrap(true); // Enable word wrapping for multi-line text
    trackInfoLabel->setTextFormat(Qt::RichText); // Enable HTML formatting
    mainLayout->addWidget(trackInfoLabel);

    // Time display (vanBasco style: 0001/0004 00:00 04:52 000)
    timeDisplayLabel = new QLabel("0001/0001 00:00 00:00 000", this);
    timeDisplayLabel->setAlignment(Qt::AlignCenter);
    timeDisplayLabel->setStyleSheet("font-family: 'Courier New', monospace; font-size: 14px; font-weight: bold; color: #00FF00; background-color: #000000; padding: 5px; border: 2px inset #666666;");
    mainLayout->addWidget(timeDisplayLabel);

    // Progress slider (restored to original full width)
    progressSlider = new QSlider(Qt::Horizontal, this);
    progressSlider->setRange(0, 100);
    progressSlider->setValue(0);
    mainLayout->addWidget(progressSlider);

    // Create channel monitor button
    channelButton = new QPushButton("📊", this);
    channelButton->setFocusPolicy(Qt::NoFocus);
    channelButton->setFixedSize(30, 30);
    channelButton->setStyleSheet(
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
    channelButton->setToolTip("Toggle Channel Monitor");

    // Create DSP button
    dspButton = new QPushButton("DSP", this);
    dspButton->setFocusPolicy(Qt::NoFocus);
    dspButton->setFixedSize(40, 26);
    dspButton->setStyleSheet(
        "QPushButton {"
        "    font-size: 16px;"
        "    font-weight: bold;"
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
    dspButton->setToolTip("Toggle Analog Simulation (LPF/Saturation)");
    dspButton->hide(); // Initially hidden, only shown for OPL files

    // Create Roll button
    rollButton = new QPushButton("🎹", this);
    rollButton->setFocusPolicy(Qt::NoFocus);
    rollButton->setFixedSize(30, 30);
    rollButton->setStyleSheet(
        "QPushButton {"
        "    font-size: 16px;"
        "    font-weight: bold;"
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
    rollButton->setToolTip("Toggle Piano Roll View");
    // rollButton->show(); // Always visible now

    // Create Bank button
    bankButton = new QPushButton("BNK", this);
    bankButton->setFocusPolicy(Qt::NoFocus);
    bankButton->setFixedSize(40, 26);
    bankButton->setStyleSheet(
        "QPushButton {"
        "    font-size: 11px;"
        "    font-weight: bold;"
        "    border: 2px solid #666666;"
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
    bankButton->setToolTip("Select External OPL Bank File (.BNK, .IBK)");
    bankButton->hide();
    bankButton->hide();


    // Create lyrics button
    lyricsButton = new QPushButton("📜", this);
    lyricsButton->setFocusPolicy(Qt::NoFocus);
    lyricsButton->setFixedSize(30, 30);
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
    lyricsButton->setToolTip("Toggle Lyrics Window");

    // Create record button
    recordButton = new QPushButton("⏺", this);
    recordButton->setFocusPolicy(Qt::NoFocus);
    recordButton->setFixedSize(30, 30);
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
        "QPushButton:pressed {"
        "    background-color: #2a2a2a;"
        "}"
    );
    recordButton->setToolTip("Record audio to WAV (Ctrl+R)");

    // Create repeat mode button with improved styling
    repeatModeButton = new QPushButton("▶️", this);
    repeatModeButton->setFocusPolicy(Qt::NoFocus);
    repeatModeButton->setFixedSize(30, 30); // Larger square button
    repeatModeButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed); // Force fixed size
    repeatModeButton->setIconSize(QSize(28, 28)); // Icon size to fill button
    repeatModeButton->setStyleSheet(
        "QPushButton {"
        "    font-size: 18px;"
        "    border: 1px solid #666666;"
        "    border-radius: 3px;"
        "    background-color: #3a3a3a;"
        "    color: white;"
        "    padding: 0px;"
        "    margin: 0px;"
        "    min-width: 30px;"
        "    max-width: 30px;"
        "    min-height: 30px;"
        "    max-height: 30px;"
        "    text-align: center;"
        "}"
        "QPushButton:hover {"
        "    background-color: #4a4a4a;"
        "    border: 2px solid #0078d4;"
        "}"
        "QPushButton:pressed {"
        "    background-color: #2a2a2a;"
        "}"
    );

    // Position label with repeat mode button on the same line
    QHBoxLayout *positionLayout = new QHBoxLayout();
    positionLabel = new QLabel("0%", this);
    positionLabel->setAlignment(Qt::AlignCenter); // Center text within fixed width
    positionLabel->setFixedWidth(50);
    positionLabel->setFixedHeight(30); // Match button height
    positionLabel->setStyleSheet("color: #cccccc; font-weight: bold;");

    positionLayout->addWidget(positionLabel);
    positionLayout->addStretch();
    positionLayout->addWidget(dspButton);
    positionLayout->addWidget(bankButton);
    positionLayout->addWidget(rollButton);
    positionLayout->addWidget(channelButton);
    positionLayout->addWidget(lyricsButton);
    positionLayout->addWidget(repeatModeButton);
    positionLayout->addWidget(recordButton);

    mainLayout->addLayout(positionLayout);

    // Playback controls
    playbackLayout = new QHBoxLayout();
    rewButton = new QPushButton("REW", this);
    rewButton->setFocusPolicy(Qt::NoFocus);
    previousButton = new QPushButton("PREV", this);
    previousButton->setFocusPolicy(Qt::NoFocus);
    playButton = new QPushButton("PLAY", this);
    playButton->setFocusPolicy(Qt::NoFocus);
    stopButton = new QPushButton("STOP", this);
    stopButton->setFocusPolicy(Qt::NoFocus);
    nextButton = new QPushButton("NEXT", this);
    nextButton->setFocusPolicy(Qt::NoFocus);
    fwButton = new QPushButton("FW", this);
    fwButton->setFocusPolicy(Qt::NoFocus);

    // Set fixed size for all playback buttons to prevent resizing
    QSize buttonSize(70, 30);
    rewButton->setFixedSize(buttonSize);
    previousButton->setFixedSize(buttonSize);
    playButton->setFixedSize(buttonSize);
    stopButton->setFixedSize(buttonSize);
    nextButton->setFixedSize(buttonSize);
    fwButton->setFixedSize(buttonSize);

    playbackLayout->addStretch();
    playbackLayout->addWidget(rewButton);
    playbackLayout->addWidget(previousButton);
    playbackLayout->addWidget(playButton);
    playbackLayout->addWidget(stopButton);
    playbackLayout->addWidget(nextButton);
    playbackLayout->addWidget(fwButton);
    playbackLayout->addStretch();

    mainLayout->addLayout(playbackLayout);

    // Volume control
    volumeLayout = new QHBoxLayout();
    volumeLabel = new QLabel("Volume:", this);
    volumeSlider = new QSlider(Qt::Horizontal, this);
    volumeSlider->setRange(0, 127);
    volumeSlider->setValue(114);   // default 90% of 127
    volumeValue = new QLabel("114", this);

    volumeLayout->addWidget(volumeLabel);
    volumeLayout->addWidget(volumeSlider);
    volumeLayout->addWidget(volumeValue);

    mainLayout->addLayout(volumeLayout);

    updateWindowTitle();
    resize(400, 600);

    // Apply vanBasco-style dark theme
    QString darkStyle = R"(
        QMainWindow {
            background-color: #2b2b2b;
            color: #ffffff;
        }
        QListWidget {
            background-color: #1e1e1e;
            color: #ffffff;
            border: 2px inset #555555;
            selection-background-color: #0078d4;
            selection-color: #ffffff;
        }
        QListWidget::item {
            padding: 3px;
            border-bottom: 1px solid #333333;
        }
        QListWidget::item:selected {
            background-color: #0078d4;
            color: #ffffff;
            font-weight: bold;
        }
        QListWidget::item:hover {
            background-color: #404040;
        }
        QPushButton {
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #5a5a5a, stop:1 #3a3a3a);
            border: 2px outset #666666;
            border-radius: 3px;
            color: #ffffff;
            font-weight: bold;
            padding: 5px 10px;
            min-height: 20px;
        }
        QPushButton:pressed {
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #3a3a3a, stop:1 #5a5a5a);
            border: 2px inset #666666;
        }
        QPushButton:hover {
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #7a7a7a, stop:1 #5a5a5a);
            border: 2px outset #0078d4;
            color: #ffffff;
        }
        QSlider::groove:horizontal {
            border: 1px solid #666666;
            height: 8px;
            background-color: #2b2b2b;
            border-radius: 4px;
        }
        QSlider::handle:horizontal {
            background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #5a5a5a, stop:1 #3a3a3a);
            border: 1px solid #666666;
            width: 18px;
            border-radius: 9px;
            margin: -5px 0;
        }
        QSlider::sub-page:horizontal {
            background-color: #0078d4;
            border-radius: 4px;
        }
        QLabel {
            color: #ffffff;
        }
        QComboBox {
            background-color: #3a3a3a;
            border: 1px solid #666666;
            color: #ffffff;
            padding: 3px;
            selection-background-color: #0078d4;
            selection-color: #ffffff;
        }
        QComboBox:hover {
            background-color: #4a4a4a;
            border: 2px solid #0078d4;
        }
        QComboBox:focus {
            border: 2px solid #0078d4;
            background-color: #4a4a4a;
        }
        QComboBox QAbstractItemView {
            background-color: #2b2b2b;
            color: #ffffff;
            selection-background-color: #0078d4;
            selection-color: #ffffff;
            border: 1px solid #666666;
        }
        QComboBox QAbstractItemView::item {
            padding: 5px;
            border-bottom: 1px solid #333333;
        }
        QComboBox QAbstractItemView::item:selected {
            background-color: #0078d4;
            color: #ffffff;
        }
        QComboBox QAbstractItemView::item:hover {
            background-color: #404040;
        }
        QComboBox::drop-down {
            border: none;
        }
        QComboBox::down-arrow {
            border: none;
            color: #ffffff;
        }
        QMessageBox {
            background-color: #2b2b2b;
            color: #ffffff;
        }
        QMessageBox QLabel {
            color: #ffffff;
        }
        QMessageBox QPushButton {
            background-color: #3a3a3a;
            border: 1px solid #666666;
            color: #ffffff;
            padding: 5px 10px;
            min-width: 80px;
        }
        QMessageBox QPushButton:hover {
            background-color: #4a4a4a;
            border: 2px solid #0078d4;
        }
        QMessageBox QPushButton:pressed {
            background-color: #2a2a2a;
        }
    )";
    setStyleSheet(darkStyle);

    // Remove focus from all UI elements to allow global arrow keys
    const auto buttons = findChildren<QPushButton*>();
    for (QPushButton* btn : buttons) {
        btn->setFocusPolicy(Qt::NoFocus);
    }
    const auto sliders = findChildren<QSlider*>();
    for (QSlider* slider : sliders) {
        slider->setFocusPolicy(Qt::NoFocus);
    }
    if (deviceComboBox) deviceComboBox->setFocusPolicy(Qt::NoFocus);

    // Install event filter to capture arrow keys globally
    qApp->installEventFilter(this);
}


void MainWindow::setupMenuBar()
{
    // Menu bar disabled - using Playlist button instead
}


void MainWindow::connectSignals()
{
    connect(fileButton, &QPushButton::clicked, this, &MainWindow::openFile);
    connect(folderButton, &QPushButton::clicked, this, &MainWindow::openFolder);
    connect(removeButton, &QPushButton::clicked, this, &MainWindow::removeFile);
    connect(sortButton, &QPushButton::clicked, this, &MainWindow::sortFiles);
    connect(playlistButton, &QPushButton::clicked, this, &MainWindow::showPlaylistMenu);
    connect(channelButton, &QPushButton::clicked, this, &MainWindow::toggleChannelMonitor);
    connect(lyricsButton, &QPushButton::clicked, this, &MainWindow::toggleLyricsWindow);
    connect(dspButton, &QPushButton::clicked, this, &MainWindow::toggleDsp);
    connect(rollButton, &QPushButton::clicked, this, &MainWindow::togglePianoRoll);
    connect(bankButton, &QPushButton::clicked, this, &MainWindow::onSelectBankFile);
    connect(repeatModeButton, &QPushButton::clicked, this, &MainWindow::onRepeatModeChanged);
    connect(recordButton, &QPushButton::clicked, this, &MainWindow::toggleRecording);

    connect(rewButton, &QPushButton::clicked, this, &MainWindow::rewind);
    connect(previousButton, &QPushButton::clicked, this, &MainWindow::previousTrack);
    connect(playButton, &QPushButton::clicked, this, &MainWindow::playPause);
    connect(stopButton, &QPushButton::clicked, this, &MainWindow::stop);
    connect(nextButton, &QPushButton::clicked, this, &MainWindow::nextTrack);
    connect(fwButton, &QPushButton::clicked, this, &MainWindow::fastForward);

    connect(volumeSlider, &QSlider::valueChanged, this, &MainWindow::onVolumeChanged);
    connect(progressSlider, &QSlider::valueChanged, this, &MainWindow::onPositionChanged);
    connect(progressSlider, &QSlider::sliderPressed, [this]() {
        isUserDragging = true;
        positionTimer->stop(); // Stop automatic updates while dragging
    });
    connect(progressSlider, &QSlider::sliderReleased, [this]() {
        isUserDragging = false;

        // Perform seek when drag is finished
        int value = progressSlider->value();
        bool isImsFile = isOplFile(currentFile);
        bool isGyb = isGybFile(currentFile);

        if (isGyb) {
            unsigned long dur = gybPlayer->getDuration();
            if (dur > 0) {
                unsigned long newPosition = (static_cast<unsigned long long>(value) * dur) / 100;
                gybPlayer->setPosition(newPosition);
            }
        } else if (isImsFile) {
            unsigned long dur = imsPlayer->getDuration();
            if (dur > 0) {
                unsigned long newPosition = (static_cast<unsigned long long>(value) * dur) / 100;
                imsPlayer->setPosition(newPosition);
            }
        } else if (isOkaOplFile(currentFile)) {
            // .oka uses OkaPlayer (.okm/.okw fall through to midiPlayer below).
            unsigned long dur = okaPlayer->getDuration();
            if (dur > 0) {
                unsigned long newPosition = (static_cast<unsigned long long>(value) * dur) / 100;
                okaPlayer->setPosition(newPosition);
            }
        } else if (midiPlayer->getTotalDuration() > 0) {
            unsigned long newPosition = (value * midiPlayer->getTotalDuration()) / 100;
            midiPlayer->setPosition(newPosition);
        }

        if (isPlaying) {
            positionTimer->start(); // Resume automatic updates
        }
    });
    // Route selection changes through a debounce timer instead of calling the
    // heavy onFileSelected() synchronously on every keypress/click.
    connect(fileList->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection &, const QItemSelection &) {
        if (previewSelectTimer) previewSelectTimer->start();
    });
    connect(fileList, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex &index) {
        if (!index.isValid()) return;

        QVariant itemTypeVariant = index.data(PlaylistModel::TypeRole);
        int itemType = itemTypeVariant.isValid() ? itemTypeVariant.toInt() : MIDI_FILE;

        if (itemType == FOLDER || itemType == PARENT_FOLDER) {
            handleFolderDoubleClick(index.data(PlaylistModel::PathRole).toString());
        } else {
            onFileDoubleClicked();
        }
    });
    connect(deviceComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onDeviceChanged);
    connect(deviceRefreshButton, &QPushButton::clicked, this, &MainWindow::onDeviceRefresh);
    connect(cleanupButton, &QPushButton::clicked, this, &MainWindow::onCleanupPlaylist);
    connect(searchBox, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);

    connect(positionTimer, &QTimer::timeout, this, &MainWindow::updatePosition);
    connect(channelUpdateTimer, &QTimer::timeout, this, &MainWindow::forceChannelUpdate);
    connect(windowPositionTimer, &QTimer::timeout, this, &MainWindow::checkWindowPosition);
    connect(midiPlayer, &MidiPlayer::finished, this, &MainWindow::onPlaybackFinished);
    connect(imsPlayer, &ImsPlayer::finished, this, &MainWindow::onPlaybackFinished);
    // Without this, GYB songs never trigger the end-of-track logic, so the
    // repeat / shuffle / next-track UI never advances.
    connect(gybPlayer, &GybPlayer::finished, this, &MainWindow::onPlaybackFinished);
    connect(okaPlayer, &OkaPlayer::finished, this, &MainWindow::onPlaybackFinished);

    connect(midiPlayer, &MidiPlayer::errorOccurred, [this](const QString &error) {
        QMessageBox::warning(this, "MIDI Player Error", error);
    });

    // Connect MIDI events for channel state tracking (always active)
    connect(midiPlayer, &MidiPlayer::noteOn, this, &MainWindow::onNoteOn);
    connect(midiPlayer, &MidiPlayer::noteOff, this, &MainWindow::onNoteOff);
    connect(midiPlayer, &MidiPlayer::programChange, this, &MainWindow::onProgramChange);
    connect(midiPlayer, &MidiPlayer::controllerChange, this, &MainWindow::onControllerChange);

    // Initialize PianoRollWindow and persist state
    pianoRollWindow = new PianoRollWindow(this);
    pianoRollWindow->setImsPlayer(imsPlayer);
    pianoRollWindow->setGybPlayer(gybPlayer);
    pianoRollWindow->setOkaPlayer(okaPlayer);
    pianoRollWindow->setMidiPlayer(midiPlayer);

    
    // Reliable MIDI connections (always active)
    connect(midiPlayer, &MidiPlayer::noteOn, pianoRollWindow, &PianoRollWindow::onNoteOn);
    connect(midiPlayer, &MidiPlayer::noteOff, pianoRollWindow, &PianoRollWindow::onNoteOff);

    connect(pianoRollWindow, &PianoRollWindow::windowClosed, this, [this]() {
        if (!m_isShuttingDown) {
            SettingsManager::instance().setValue("UI/showPianoRoll", false);
        }
        rollButton->setStyleSheet(
            "QPushButton { font-size: 16px; border: 1px solid #666666; border-radius: 3px; background-color: #3a3a3a; color: white; padding: 0px; }"
            "QPushButton:hover { background-color: #4a4a4a; border: 2px solid #0078d4; }"
        );
    });

    // Restore last state
    if (SettingsManager::instance().value("UI/showPianoRoll", false).toBool()) {
        togglePianoRoll(); // This will show it and set button style
    }
}


// QSettings* MainWindow::createSettings() - Replaced with SettingsManager singleton


void MainWindow::checkWindowPosition()
{
    // Check if window position has changed
    QPoint currentPosition = pos();
    if (currentPosition != lastWindowPosition) {
        lastWindowPosition = currentPosition;

        // If channel monitor is open and visible, reposition it
        if (channelMonitor && channelMonitor->isVisible()) {
            channelMonitor->positionBesideMainWindow();
        }

        // If lyrics window is open and visible, reposition it to the right
        if (lyricsWindow && lyricsWindow->isVisible()) {
            lyricsWindow->positionBesideMainWindow();
        }
    }
}


QString MainWindow::getActualExecutablePath()
{
    // For PyInstaller executables, we cannot reliably get the actual .exe location
    // Instead, use user-friendly default locations

    // Try Desktop first (most common place users put downloaded files)
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (!desktopPath.isEmpty() && QDir(desktopPath).exists()) {
        return desktopPath;
    }

    // Fallback to Documents folder
    QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!documentsPath.isEmpty() && QDir(documentsPath).exists()) {
        return documentsPath;
    }

    // Final fallback to Home directory
    QString homePath = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (!homePath.isEmpty() && QDir(homePath).exists()) {
        return homePath;
    }

    // Last resort: Qt default
    return QApplication::applicationDirPath();
}


void MainWindow::saveSettings()
{
    SettingsManager& settings = SettingsManager::instance();

    // Save General settings first
    int currentRow = plCurrentRow();
    settings.setValue("General/currentTrackIndex", currentRow);
    settings.setValue("General/volume", volumeSlider->value());
    settings.setValue("General/repeatMode", repeatMode);
    settings.setValue("General/selectedDevice", deviceComboBox->currentIndex());
    settings.setValue("General/lastUsedDevice", deviceComboBox->currentText());
    settings.setValue("General/channelMonitorVisible", channelMonitor != nullptr);
    settings.setValue("General/lyricsWindowVisible", lyricsWindow != nullptr);
    settings.setValue("UI/showPianoRoll", pianoRollWindow != nullptr && pianoRollWindow->isVisible());
    
    bool gybActive = !currentFile.isEmpty() && isGybFile(currentFile);
    bool okaOplActive = !currentFile.isEmpty() && isOkaFile(currentFile);
    int activeDspLevel = gybActive ? gybPlayer->getDspLevel()
                                   : (okaOplActive ? okaPlayer->getDspLevel() : imsPlayer->getDspLevel());
    settings.setValue("Synth/DspLevel", activeDspLevel);
    settings.setValue("Synth/OplStereoMode", JJoMeSynth::instance().getOplStereoMode());

    // Save window geometry
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());

    // Save playlist and navigation settings last
    settings.setValue("currentFolderPath", currentFolderPath);
    settings.setValue("navigationHistory", navigationHistory);
    settings.setValue("isInBrowsingMode", isInBrowsingMode);
    settings.setValue("browsingRootPath", browsingRootPath);

}

void MainWindow::loadSettings()
{
    SettingsManager& settings = SettingsManager::instance();

    // Auto-discover SoundFonts in "SoundFonts" directory and add them if not present
    QStringList sfList = settings.value("Synth/SoundFontList", QStringList()).toStringList();
    bool sfListChanged = false;
    
    QDir sfDir(QApplication::applicationDirPath() + "/SoundFonts");
    if (sfDir.exists()) {
        QStringList filters;
        filters << "*.sf2";
        QFileInfoList sfFiles = sfDir.entryInfoList(filters, QDir::Files);
        for (const QFileInfo& sfFile : sfFiles) {
            QString path = QDir::cleanPath(sfFile.absoluteFilePath());
            bool exists = false;
            for (const QString& existing : sfList) {
                if (QDir::cleanPath(existing).compare(path, Qt::CaseInsensitive) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                sfList.append(path);
                sfListChanged = true;
            }
        }
    }
    
    if (sfListChanged || sfList.isEmpty()) {
        settings.setValue("Synth/SoundFontList", sfList);
        // Set the GS font or the first one as active if no active soundfont is set
        QString activeSf = settings.value("Synth/SoundFontPath", "").toString();
        if (activeSf.isEmpty() && !sfList.isEmpty()) {
            // Prefer GS font if available
            for (const QString& sf : sfList) {
                if (sf.contains("GS", Qt::CaseInsensitive)) {
                    activeSf = sf;
                    break;
                }
            }
            if (activeSf.isEmpty()) activeSf = sfList.first();
            
            settings.setValue("Synth/SoundFontPath", activeSf);
        }
        settings.sync();
    }

    // Load General settings first
    int volume = settings.value("General/volume", 114).toInt();   // default 90% of 127
    volumeSlider->setValue(volume);
    midiPlayer->setVolume(volume);

    int dspLevel = settings.value("Synth/DspLevel", 0).toInt();
    imsPlayer->setDspLevel(dspLevel);
    // Apply to GYB/OKA too so toggling between players doesn't appear to drop
    // DSP back to OFF on file selection (updateDspButtonStyle reads the active
    // player's current level).
    gybPlayer->setDspLevel(dspLevel);
    okaPlayer->setDspLevel(dspLevel);
    updateDspButtonStyle();

    int oplStereoMode = settings.value("Synth/OplStereoMode", 1).toInt();
    JJoMeSynth::instance().setOplStereoMode(oplStereoMode);

    repeatMode = settings.value("General/repeatMode", 0).toInt();
    // Update button text based on loaded mode (without incrementing)
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

    // Load window geometry
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());

    // Restore channel monitor state
    bool shouldShowChannelMonitor = settings.value("General/channelMonitorVisible", false).toBool();
    bool shouldShowLyricsWindow = settings.value("General/lyricsWindowVisible", false).toBool();

    // Show channel monitor if it was visible when program was closed
    if (shouldShowChannelMonitor) {
        // Use QTimer to ensure UI is fully initialized first
        QTimer::singleShot(100, this, [this]() {
            if (!channelMonitor) {
                toggleChannelMonitor();
            }
        });
    }

    // Show lyrics window if it was visible when program was closed
    if (shouldShowLyricsWindow) {
        QTimer::singleShot(100, this, [this]() {
            if (!lyricsWindow) {
                toggleLyricsWindow();
            }
        });
    }

    // Restore piano roll state
    bool shouldShowPianoRoll = settings.value("UI/showPianoRoll", false).toBool();
    if (shouldShowPianoRoll) {
        QTimer::singleShot(150, this, [this]() {
            if (pianoRollWindow && !pianoRollWindow->isVisible()) {
                togglePianoRoll();
            }
        });
    }

    // Clean up old playlist format
    SettingsManager& settingsForCleanup = SettingsManager::instance();
    settingsForCleanup.remove("playlist");

    // Load playlist and navigation settings last
    SettingsManager& playlistSettings = SettingsManager::instance();
    currentFolderPath = playlistSettings.value("currentFolderPath", "").toString();
    navigationHistory = playlistSettings.value("navigationHistory", QStringList()).toStringList();
    isInBrowsingMode = playlistSettings.value("isInBrowsingMode", false).toBool();
    browsingRootPath = playlistSettings.value("browsingRootPath", "").toString();

    // Load playlist tree (new format)
    loadPlaylistTree();

    // Update window title if in browsing mode
    if (!currentFolderPath.isEmpty() && isInBrowsingMode) {
        updateWindowTitle();
    }

    // Load and set current track (after playlist is loaded)
    SettingsManager& trackSettings = SettingsManager::instance();
    int currentTrackIndex = trackSettings.value("General/currentTrackIndex", -1).toInt();

    if (plCount() > 0) {
        if (currentTrackIndex >= 0 && currentTrackIndex < plCount()) {
            plSetCurrentRow(currentTrackIndex);
        } else {
            // If no valid last track, select first track
            plSetCurrentRow(0);
        }

        // Load the selected track (but don't start playing).
        // NOTE: onFileSelected() already fires when setCurrentRow() is called above,
        // so GYB/IMS/OKA files are already loaded through their dedicated players.
        // Only call midiPlayer->loadMidiFile() for plain MIDI files to avoid
        // double-loading and heap corruption.
        if (plHasCurrent()) {
            QString rawPath = plCurrentPath();
            QString filePath = resolvePlayablePath(rawPath);
            bool isGyb = isGybFile(filePath);
            bool isIms = isOplFile(filePath);
            bool isOka = isOkaFile(filePath);
            if (!isGyb && !isIms && !isOka) {
                if (midiPlayer->loadMidiFile(filePath)) {
                    updateTrackInfo();
                    progressSlider->setValue(0);
                    positionLabel->setText("0%");
                }
            } else {
                // GYB/IMS/OKA are already loaded by onFileSelected()
                updateTrackInfo();
            }
        }
    }

    // Update allowed paths based on loaded playlist structure
    updateAllowedPaths();

    // Reload MIDI device settings after all other settings are loaded
    loadMidiDeviceSettings();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_isShuttingDown = true;
    if (playlistSaveTimer && playlistSaveTimer->isActive()) {
        playlistSaveTimer->stop();
        savePlaylistTree();
    }
    saveSettings();
    event->accept();
}

void MainWindow::moveEvent(QMoveEvent *event)
{
    QMainWindow::moveEvent(event);

    // If channel monitor is open and visible, reposition it relative to the new main window position
    if (channelMonitor && channelMonitor->isVisible()) {
        channelMonitor->positionBesideMainWindow();
    }
}


void MainWindow::keyPressEvent(QKeyEvent *event)
{
    // Handle Delete key for playlist management
    if (event->key() == Qt::Key_Delete) {
        // Only trigger if not typing in the search box
        if (!searchBox->hasFocus()) {
            removeFile();
            return;
        }
    }

    // Only handle other keyboard shortcuts when fileList has focus or is the active widget
    if (fileList->hasFocus() || fileList == focusWidget()) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            activateSelectedPlaylistRow();   // enter folder / play selected file
            return;
        }
        else if (event->key() == Qt::Key_Space) {
            spacePauseResume();              // pause/resume only
            return;
        }
        else if (event->matches(QKeySequence::SelectAll)) {
            // Ctrl+A: select all items
            fileList->selectAll();
            return;
        }
    }

    // For global shortcuts (work regardless of focus)
    if (event->key() == Qt::Key_R && event->modifiers() == Qt::ControlModifier) {
        toggleRecording();
        return;
    }

    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        spacePauseResume();                  // global Space: pause/resume only
        return;
    }

    // Pass to parent for default handling
    QMainWindow::keyPressEvent(event);
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    // Handle Alt+Click for file info
    if (event->modifiers() == Qt::AltModifier && event->button() == Qt::LeftButton) {
        // Check if click is on fileList
        QWidget *widget = childAt(event->pos());
        while (widget && widget != fileList) {
            widget = widget->parentWidget();
        }

        if (widget == fileList) {
            // Get the item at the click position
            QPoint relativePos = fileList->mapFrom(this, event->pos());
            QModelIndex idx = fileList->indexAt(relativePos);
            if (idx.isValid()) {
                QString path = idx.data(PlaylistModel::PathRole).toString();
                showFileInfo(path);
                return;
            }
        }
    }

    // Pass to parent for default handling
    QMainWindow::mousePressEvent(event);
}

void MainWindow::showFileInfo(const QString &filePath)
{
    if (filePath.isEmpty()) return;

    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        QMessageBox::warning(this, "File Information", "File does not exist: " + filePath);
        return;
    }

    QString info = QString("File Information\n\n")
                 + QString("Name: %1\n").arg(fileInfo.fileName())
                 + QString("Path: %1\n").arg(fileInfo.absolutePath())
                 + QString("Size: %1 bytes\n").arg(fileInfo.size())
                 + QString("Last Modified: %1\n").arg(fileInfo.lastModified().toString())
                 + QString("Full Path: %1").arg(fileInfo.absoluteFilePath());

    QMessageBox::information(this, "File Information", info);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    // Accept drag if it contains file URLs
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();

    if (mimeData->hasUrls()) {
        QStringList pathList;
        QList<QUrl> urlList = mimeData->urls();

        // Extract file paths from URLs
        for (const QUrl &url : urlList) {
            // toLocalFile()???곗꽑 ?쒕룄: A:\, B:\ 媛숈? ?뚮줈???쒕씪?대툕 寃쎈줈??泥섎━
            QString localPath = url.toLocalFile();
            if (!localPath.isEmpty()) {
                pathList.append(localPath);
            }
        }

        if (!pathList.isEmpty()) {
            addMidiFiles(pathList);
            event->acceptProposedAction();
        } else {
            event->ignore();
        }
    } else {
        event->ignore();
    }
}


// ============================================================================
// New Playlist Tree Management System
// ============================================================================


void MainWindow::toggleChannelMonitor()
{
    if (!channelMonitor) {
        // Create and show channel monitor
        channelMonitor = new ChannelMonitor(this);
        channelMonitor->installEventFilter(this);

        // Connect MIDI signals to channel monitor
        connect(midiPlayer, &MidiPlayer::noteOn, channelMonitor, &ChannelMonitor::onNoteOn);
        connect(midiPlayer, &MidiPlayer::noteOff, channelMonitor, &ChannelMonitor::onNoteOff);
        connect(midiPlayer, &MidiPlayer::controllerChange, channelMonitor, &ChannelMonitor::onControllerChange);
        connect(midiPlayer, &MidiPlayer::programChange, channelMonitor, &ChannelMonitor::onProgramChange);

        // Connect sound mode reliability signal (must come first)
        connect(midiPlayer, &MidiPlayer::soundModeReliabilityChanged, [this](const SoundModeReliability& reliability) {
            if (channelMonitor) {
                channelMonitor->updateSoundModeReliability(reliability);
                // Update sound mode after reliability info is set
                channelMonitor->setSoundMode(static_cast<ChannelWidget::SoundMode>(reliability.detectedMode));
            }
        });

        // Keep legacy signal for compatibility but don't use it when reliability is available
        connect(midiPlayer, &MidiPlayer::soundModeDetected, [this](int mode) {
            // This will be called but setSoundMode will be called again by reliability signal
        });

        // Connect close signal
        connect(channelMonitor, &ChannelMonitor::windowClosed, [this]() {
            channelMonitor = nullptr;
            windowPositionTimer->stop(); // Stop position tracking when window is closed
            channelButton->setStyleSheet(
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

        // Update button style to show it's active
        channelButton->setStyleSheet(
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

        channelMonitor->show();

        bool currentlyIms = isOplFile(currentFile);
        bool currentlyGyb = isGybFile(currentFile);
        bool currentlyOka = isOkaFile(currentFile);
        if (currentlyGyb && gybPlayer) {
            channelMonitor->setImsMode(true, gybPlayer->getBankName(), gybPlayer->getInstruments(), "GYB");
            channelMonitor->updateVoiceInstrumentNames(gybPlayer->getVoiceInstrumentNames());
        } else if (currentlyOka && okaPlayer) {
            // OKA renders through OPL like GYB — use the same instrument-slot
            // monitor layout (not the GM MIDI grid) and refresh per-voice patches.
            channelMonitor->setImsMode(true, okaPlayer->getBankName(), okaPlayer->getInstruments(), "OKA");
            channelMonitor->updateVoiceInstrumentNames(okaPlayer->getVoiceInstrumentNames());
        } else if (currentlyIms && imsPlayer) {
            channelMonitor->setImsMode(true, imsPlayer->getBankName(), imsPlayer->getInstruments(), QFileInfo(currentFile).suffix().toUpper());
            channelMonitor->updateVoiceInstrumentNames(imsPlayer->getVoiceInstrumentNames());
        } else {
            // Apply stored channel states when monitor is opened (MIDI mode)
            for (int i = 0; i < 16; ++i) {
                if (channelHasProgram[i]) {
                    channelMonitor->onProgramChange(i, channelPrograms[i]);
                }
                if (channelIsActive[i]) {
                    channelMonitor->onNoteOn(i, 60, 64);
                    channelMonitor->onNoteOff(i, 60);
                }
            }
        }

        // Start window position tracking timer
        lastWindowPosition = pos();
        windowPositionTimer->start(50); // Check every 50ms for smooth tracking

        // Apply matching titlebar style to main window for consistency
        #ifdef _WIN32
        if (winId()) {
            HWND hwnd = (HWND)winId();
            BOOL value = TRUE;
            ::DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value)); // DWMWA_USE_IMMERSIVE_DARK_MODE

            // Ensure main window has consistent titlebar styling
            LONG_PTR style = ::GetWindowLongPtr(hwnd, GWL_STYLE);
            style |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
            ::SetWindowLongPtr(hwnd, GWL_STYLE, style);
        }
        #endif
    } else {
        // Close channel monitor
        channelMonitor->close();
        channelMonitor = nullptr;

        // Stop window position tracking timer
        windowPositionTimer->stop();

        // Reset button style
        channelButton->setStyleSheet(
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


void MainWindow::handleNewIpcConnection()
{
    QLocalSocket *clientSocket = ipcServer->nextPendingConnection();
    if (!clientSocket) return;

    connect(clientSocket, &QLocalSocket::readyRead, this, [this, clientSocket]() {
        QByteArray data = clientSocket->readAll();
        QString filePath = QString::fromUtf8(data);
        
        if (!filePath.isEmpty()) {
            // Restore window if minimized and bring to front
            showNormal();
            raise();
            activateWindow();
            
            // Handle the external file load
            handleExternalFileLoad(filePath);
        }
    });

    connect(clientSocket, &QLocalSocket::disconnected, clientSocket, &QLocalSocket::deleteLater);
}


bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);

        if (keyEvent->key() == Qt::Key_Up || keyEvent->key() == Qt::Key_Down || keyEvent->key() == Qt::Key_Left || keyEvent->key() == Qt::Key_Right) {
            qDebug() << "[JJoMeDebug] Arrow key =" << keyEvent->key()
                     << "obj =" << (obj ? obj->metaObject()->className() : "null")
                     << "focusWidget =" << (focusWidget() ? focusWidget()->metaObject()->className() : "none")
                     << "isPlaying =" << isPlaying;
        }

        if (keyEvent->key() == Qt::Key_F1) {
            showHelpDialog();
            return true;
        }

        if (keyEvent->key() == Qt::Key_F12) {
            static bool isOplStereoOpen = false;
            if (isOplStereoOpen) return true;
            isOplStereoOpen = true;

            int currentMode = JJoMeSynth::instance().getOplStereoMode();
            OplStereoDialog dlg(currentMode, this);
            if (dlg.exec() == QDialog::Accepted) {
                int newMode = dlg.getSelectedMode();
                JJoMeSynth::instance().setOplStereoMode(newMode);
                JJoMeSynth::instance().forceApplyOplStereo();
                saveSettings();
            }
            isOplStereoOpen = false;
            return true;
        }

        // ?대깽?멸? searchBox濡?吏곸젒 ?꾨떖?섎뒗 寃쎌슦?먮쭔 諛⑺뼢???덉랬 ?쒖쇅
        // (hasFocus()??searchBox媛€ 湲곕낯 ?ъ빱?ㅻ? 媛€吏?????긽 true媛€ ?섏뼱 ?ㅼ옉??
        // While a modal dialog (file picker, message box, settings dialog) is open,
        // do NOT hijack Enter/Space/Delete/arrows — the dialog must keep them
        // (e.g. Enter = OK). F1/F12 above keep their own re-entry guards.
        static QElapsedTimer modalGraceTimer;   // restarted on every key seen while a modal is up
        if (QApplication::activeModalWidget() != nullptr) {
            modalGraceTimer.restart();
            return false;
        }
        // Just after a modal closed (e.g. the F12 OPL dialog accepted on Enter),
        // auto-repeat / trailing presses of the SAME Enter keystroke arrive here
        // with the modal already gone and used to re-trigger playlist playback.
        // Swallow Enter/Space during a short grace window after any modal key.
        const bool justLeftModal = modalGraceTimer.isValid() && modalGraceTimer.elapsed() < 300;

        bool inSearch = (searchBox && searchBox->hasFocus());

        // Esc in the search box: clear the search (restores the folder view)
        // and hand focus back to the playlist.
        if (inSearch && keyEvent->key() == Qt::Key_Escape) {
            searchBox->clear();
            if (fileList) fileList->setFocus();
            return true;
        }
        // Down in the search box: move into the result list (select first row).
        // Search results are intentionally not auto-selected while typing.
        if (inSearch && keyEvent->key() == Qt::Key_Down) {
            if (fileList && plCount() > 0) {
                fileList->setFocus();
                if (plCurrentRow() < 0) plSetCurrentRow(0);
            }
            return true;
        }

        // Enter: enter the selected folder / play the selected file. Consumed here
        // (before the list's own Enter/Space handling) so it works regardless of
        // which widget currently holds focus. Auto-repeat is ignored: holding
        // Enter must not machine-gun playback restarts.
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) && !inSearch) {
            if (keyEvent->isAutoRepeat() || justLeftModal) return true;
            activateSelectedPlaylistRow();
            return true;
        }
        // Space: pause/resume the CURRENT track ONLY (never starts a new selection).
        if (keyEvent->key() == Qt::Key_Space && keyEvent->modifiers() == Qt::NoModifier && !inSearch) {
            if (keyEvent->isAutoRepeat() || justLeftModal) return true;
            spacePauseResume();
            return true;
        }
        // Up/Down: playlist navigation ONLY — whether playing or not, and even
        // when the list doesn't hold keyboard focus.
        if (!inSearch) {
            if (keyEvent->key() == Qt::Key_Up)   { movePlaylistSelection(-1); return true; }
            if (keyEvent->key() == Qt::Key_Down) { movePlaylistSelection(+1); return true; }
        }
        // Master volume: + / = (up), - (down). Adjustable anytime.
        if (!inSearch) {
            if (keyEvent->key() == Qt::Key_Plus || keyEvent->key() == Qt::Key_Equal) {
                volumeSlider->setValue(qBound(0, volumeSlider->value() + 5, 127));
                return true;
            }
            if (keyEvent->key() == Qt::Key_Minus) {
                volumeSlider->setValue(qBound(0, volumeSlider->value() - 5, 127));
                return true;
            }
        }

        // Let searchBox / playlist handle their remaining keys (native arrow nav) themselves.
        if (obj == searchBox || obj == fileList || (fileList && obj == fileList->viewport())) {
            return false;
        }

        if (keyEvent->key() == Qt::Key_Delete) {
            if (!inSearch) {
                removeFile();
                return true;
            }
        }

        // Seek + tempo/key hotkeys while a track is playing. (Up/Down and +/- are
        // already handled above as playlist navigation / volume.)
        bool hasTrack = (midiPlayer && midiPlayer->getTotalDuration() > 0)
                     || (imsPlayer && imsPlayer->getDuration() > 0 && isOplFile(currentFile))
                     || (gybPlayer && gybPlayer->getDuration() > 0 && isGybFile(currentFile))
                     || (okaPlayer && okaPlayer->getDuration() > 0 && isOkaFile(currentFile));
        if (hasTrack && isPlaying) {
            if (keyEvent->key() == Qt::Key_Left) {
                rewind();
                return true;
            }
            else if (keyEvent->key() == Qt::Key_Right) {
                fastForward();
                return true;
            }
            // Hotkeys (F7-F11) for Tempo and Key transpose (universal support)
            else if (keyEvent->key() == Qt::Key_F7 || keyEvent->key() == Qt::Key_F8 ||
                     keyEvent->key() == Qt::Key_F9 || keyEvent->key() == Qt::Key_F10 ||
                     keyEvent->key() == Qt::Key_F11) {
                bool isIms = isOplFile(currentFile);
                bool isGyb = isGybFile(currentFile);
                bool isOka = isOkaFile(currentFile);
                bool playOkaViaOpl = isOkaOplFile(currentFile);

                if (keyEvent->key() == Qt::Key_F7) {
                    if (isGyb && gybPlayer) {
                        int currentScale = gybPlayer->getUserTempoScale();
                        gybPlayer->setUserTempoScale(qMax(50, currentScale - 5));
                    } else if (isIms && imsPlayer) {
                        int currentScale = imsPlayer->getUserTempoScale();
                        imsPlayer->setUserTempoScale(qMax(50, currentScale - 5));
                    } else if (playOkaViaOpl && okaPlayer) {
                        int currentScale = okaPlayer->getUserTempoScale();
                        okaPlayer->setUserTempoScale(qMax(50, currentScale - 5));
                    } else if (midiPlayer) {
                        int currentScale = midiPlayer->getUserTempoScale();
                        midiPlayer->setUserTempoScale(qMax(50, currentScale - 5));
                    }
                    updateTimeDisplay();
                    return true;
                }
                else if (keyEvent->key() == Qt::Key_F8) {
                    if (isGyb && gybPlayer) {
                        int currentScale = gybPlayer->getUserTempoScale();
                        gybPlayer->setUserTempoScale(qMin(150, currentScale + 5));
                    } else if (isIms && imsPlayer) {
                        int currentScale = imsPlayer->getUserTempoScale();
                        imsPlayer->setUserTempoScale(qMin(150, currentScale + 5));
                    } else if (playOkaViaOpl && okaPlayer) {
                        int currentScale = okaPlayer->getUserTempoScale();
                        okaPlayer->setUserTempoScale(qMin(150, currentScale + 5));
                    } else if (midiPlayer) {
                        int currentScale = midiPlayer->getUserTempoScale();
                        midiPlayer->setUserTempoScale(qMin(150, currentScale + 5));
                    }
                    updateTimeDisplay();
                    return true;
                }
                else if (keyEvent->key() == Qt::Key_F9) {
                    if (isIms && imsPlayer) {
                        int currentTranspose = imsPlayer->getUserKeyTranspose();
                        imsPlayer->setUserKeyTranspose(qMax(-6, currentTranspose - 1));
                    } else if (isGyb && gybPlayer) {
                        int currentTranspose = gybPlayer->getUserKeyTranspose();
                        gybPlayer->setUserKeyTranspose(qMax(-6, currentTranspose - 1));
                    } else if (playOkaViaOpl && okaPlayer) {
                        int currentTranspose = okaPlayer->getUserKeyTranspose();
                        okaPlayer->setUserKeyTranspose(qMax(-6, currentTranspose - 1));
                    } else if (midiPlayer) {
                        int currentTranspose = midiPlayer->getUserKeyTranspose();
                        midiPlayer->setUserKeyTranspose(qMax(-6, currentTranspose - 1));
                    }
                    updateTimeDisplay();
                    return true;
                }
                else if (keyEvent->key() == Qt::Key_F10) {
                    if (isIms && imsPlayer) {
                        int currentTranspose = imsPlayer->getUserKeyTranspose();
                        imsPlayer->setUserKeyTranspose(qMin(6, currentTranspose + 1));
                    } else if (isGyb && gybPlayer) {
                        int currentTranspose = gybPlayer->getUserKeyTranspose();
                        gybPlayer->setUserKeyTranspose(qMin(6, currentTranspose + 1));
                    } else if (playOkaViaOpl && okaPlayer) {
                        int currentTranspose = okaPlayer->getUserKeyTranspose();
                        okaPlayer->setUserKeyTranspose(qMin(6, currentTranspose + 1));
                    } else if (midiPlayer) {
                        int currentTranspose = midiPlayer->getUserKeyTranspose();
                        midiPlayer->setUserKeyTranspose(qMin(6, currentTranspose + 1));
                    }
                    updateTimeDisplay();
                    return true;
                }
                else if (keyEvent->key() == Qt::Key_F11) {
                    if (isGyb && gybPlayer) {
                        gybPlayer->setUserTempoScale(100);
                        gybPlayer->setUserKeyTranspose(0);
                    } else if (isIms && imsPlayer) {
                        imsPlayer->setUserTempoScale(100);
                        imsPlayer->setUserKeyTranspose(0);
                    } else if (playOkaViaOpl && okaPlayer) {
                        okaPlayer->setUserTempoScale(100);
                        okaPlayer->setUserKeyTranspose(0);
                    } else if (midiPlayer) {
                        midiPlayer->setUserTempoScale(100);
                        midiPlayer->setUserKeyTranspose(0);
                    }
                    updateTimeDisplay();
                    return true;
                }
            }
        }
        return false;
    } else if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
        if (obj == channelMonitor && pianoRollWindow && pianoRollWindow->isVisible()) {
            QRect cmRect = channelMonitor->frameGeometry();
            QRect mainRect = frameGeometry();
            int prHeight = mainRect.bottom() - cmRect.bottom();
            if (prHeight < 60) prHeight = 60;
            
            int titleBarHeight = pianoRollWindow->frameGeometry().height() - pianoRollWindow->geometry().height();
            if (titleBarHeight <= 0) titleBarHeight = 31;
            
            pianoRollWindow->move(cmRect.left(), cmRect.bottom());
            pianoRollWindow->resize(cmRect.width(), prHeight - titleBarHeight);
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::updateWindowTitle()
{
    QString title = "🎵 JJoMe MIDI Player v2.4e.4";
    
    if (currentNode) {
        if (currentNode->isFolder) {
            title += QString(" - %1").arg(QFileInfo(currentNode->fullPath).fileName());
        } else {
            title += QString(" - %1").arg(QFileInfo(currentNode->fullPath).baseName());
        }
    }

    setWindowTitle(title);
}

void MainWindow::toggleDsp()
{
    // GYB, OKA (OPL), and IMS use the same DSP chain (LPF + soft-clip).
    QString targetFile = currentFile;
    if (!isPlaying && plHasCurrent()) {
        targetFile = plCurrentPath();
    }

    bool gybActive = !targetFile.isEmpty() && isGybFile(targetFile);
    bool okaOplActive = !targetFile.isEmpty() && isOkaFile(targetFile);
    int currentLevel = gybActive ? gybPlayer->getDspLevel()
                                 : (okaOplActive ? okaPlayer->getDspLevel() : imsPlayer->getDspLevel());
    int nextLevel;
    if (currentLevel == 0) nextLevel = 1;
    else if (currentLevel == 3) nextLevel = 0;
    else nextLevel = currentLevel + 1;

    imsPlayer->setDspLevel(nextLevel);
    gybPlayer->setDspLevel(nextLevel);
    okaPlayer->setDspLevel(nextLevel);
    updateDspButtonStyle();
}

void MainWindow::togglePianoRoll()
{
    if (!pianoRollWindow) return;

    if (pianoRollWindow->isVisible()) {
        pianoRollWindow->hide();
        SettingsManager::instance().setValue("UI/showPianoRoll", false);
        rollButton->setStyleSheet(
            "QPushButton { font-size: 16px; border: 1px solid #666666; border-radius: 3px; background-color: #3a3a3a; color: white; padding: 0px; }"
            "QPushButton:hover { background-color: #4a4a4a; border: 2px solid #0078d4; }"
        );
    } else {
        SettingsManager::instance().setValue("UI/showPianoRoll", true);
        if (channelMonitor && channelMonitor->isVisible()) {
            QRect cmRect = channelMonitor->frameGeometry();
            QRect mainRect = frameGeometry();
            int prX = cmRect.left();
            int prY = cmRect.bottom();
            int prWidth = cmRect.width();
            int prHeight = mainRect.bottom() - cmRect.bottom();
            // Minimum height to ensure keys are still playable/visible
            if (prHeight < 60) prHeight = 60; 
            
            int titleBarHeight = pianoRollWindow->frameGeometry().height() - pianoRollWindow->geometry().height();
            if (titleBarHeight <= 0) titleBarHeight = 31;
            
            pianoRollWindow->move(prX, prY);
            pianoRollWindow->resize(prWidth, prHeight - titleBarHeight);
        } else {
            QRect mainRect = frameGeometry();
            pianoRollWindow->move(mainRect.left(), mainRect.bottom());
            pianoRollWindow->resize(mainRect.width(), 80);
        }
        
        pianoRollWindow->show();
        rollButton->setStyleSheet(
            "QPushButton { font-size: 16px; border: 2px solid #0078d4; border-radius: 3px; background-color: #4a4a4a; color: white; padding: 0px; }"
            "QPushButton:hover { background-color: #0078d4; border: 2px solid #0096ff; }"
        );
    }
}

void MainWindow::updateDspButtonStyle()
{
    QString targetFile = currentFile;
    if (!isPlaying && plHasCurrent()) {
        targetFile = plCurrentPath();
    }

    bool gybActive = !targetFile.isEmpty() && isGybFile(targetFile);
    bool okaOplActive = !targetFile.isEmpty() && isOkaFile(targetFile);
    int level = gybActive ? gybPlayer->getDspLevel()
                          : (okaOplActive ? okaPlayer->getDspLevel() : imsPlayer->getDspLevel());
    QString text = "DSP";
    QString style;
    
    // Base style for all states to prevent layout jumping
    QString baseStyle = "QPushButton { font-weight: bold; border-radius: 3px; padding: 0px; margin: 0px; min-height: 26px; max-height: 26px; color: white; } ";

    if (level == 1) {
        text = "DSP";
        style = baseStyle + "QPushButton { font-size: 11px; border: 2px solid #0078d4; background-color: #005a9e; } "
                "QPushButton:hover { background-color: #0078d4; border: 2px solid #0096ff; }";
    } else if (level == 2) {
        text = "DSP2";
        style = baseStyle + "QPushButton { font-size: 10px; border: 2px solid #ff8c00; background-color: #d35400; } "
                "QPushButton:hover { background-color: #e67e22; border: 2px solid #ffaf4d; }";
    } else if (level == 3) {
        text = "DSP3";
        style = baseStyle + "QPushButton { font-size: 10px; border: 2px solid #ff00ff; background-color: #a000a0; } "
                "QPushButton:hover { background-color: #c000c0; border: 2px solid #ff66ff; }";
    } else {
        text = "DSP";
        style = baseStyle + "QPushButton { font-size: 11px; border: 2px solid #666666; background-color: #3a3a3a; color: #888888; } "
                "QPushButton:hover { background-color: #4a4a4a; border: 2px solid #0078d4; }";
    }
    
    dspButton->setText(text);
    dspButton->setStyleSheet(style);
}


void MainWindow::onSelectBankFile()
{
    QString targetFile = currentFile;
    if (targetFile.isEmpty()) {
        if (plHasCurrent()) {
            targetFile = plCurrentPath();
        }
    }
    bool isGyb = !targetFile.isEmpty() && targetFile.toLower().endsWith(".gyb");
    bool isOka = !targetFile.isEmpty() && isOkaFile(targetFile);

    QString currentBank = isGyb ? gybPlayer->getExternalBankPath()
                                : (isOka ? okaPlayer->getExternalBankPath()
                                         : imsPlayer->getExternalBankPath());
    QString initialDir;
    if (currentBank.isEmpty()) {
        initialDir = QApplication::applicationDirPath();
    } else {
        initialDir = QFileInfo(currentBank).absolutePath();
    }

    QString fileName = QFileDialog::getOpenFileName(this,
        "Select OPL Bank File", initialDir, "Bank Files (*.BNK *.IBK *.SND *.TIM *.TBR);;All Files (*.*)");

    if (!fileName.isEmpty()) {
        SettingsManager::instance().setValue("General/lastOpenDirectory", QFileInfo(fileName).absolutePath());

        if (isGyb) {
            gybPlayer->setExternalBankPath(fileName);
            SettingsManager::instance().setValue("Synth/ExternalGybBank", fileName);

            if (!currentFile.isEmpty() && currentFile.toLower().endsWith(".gyb")) {
                bool wasPlaying = isPlaying;
                unsigned long lastPos = gybPlayer->getPosition();
                gybPlayer->stop();
                if (pianoRollWindow) pianoRollWindow->clearNotes();

                if (gybPlayer->loadFile(currentFile)) {
                    if (wasPlaying) {
                        gybPlayer->setPosition(lastPos);
                        gybPlayer->play();
                    }
                    updateTrackInfo();
                }
            }
        } else if (isOka) {
            okaPlayer->setExternalBankPath(fileName);
            SettingsManager::instance().setValue("Synth/ExternalOkaBank", fileName);

            if (!currentFile.isEmpty() && isOkaFile(currentFile)) {
                bool wasPlaying = isPlaying;
                unsigned long lastPos = okaPlayer->getPosition();
                okaPlayer->stop();
                if (pianoRollWindow) pianoRollWindow->clearNotes();

                if (okaPlayer->loadFile(currentFile)) {
                    if (wasPlaying) {
                        okaPlayer->setPosition(lastPos);
                        okaPlayer->play();
                    }
                    updateTrackInfo();
                }
            }
        } else {
            imsPlayer->setExternalBankPath(fileName);
            SettingsManager::instance().setValue("Synth/ExternalImsBank", fileName);
            
            if (!currentFile.isEmpty() && isOplFile(currentFile)) {
                bool wasPlaying = isPlaying;
                unsigned long lastPos = imsPlayer->getPosition();
                imsPlayer->stop();
                if (pianoRollWindow) pianoRollWindow->clearNotes();
                
                if (imsPlayer->loadFile(currentFile)) {
                    if (wasPlaying) {
                        imsPlayer->setPosition(lastPos);
                        imsPlayer->play();
                    }
                    updateTrackInfo();
                    if (channelMonitor) {
                        channelMonitor->setImsMode(true, imsPlayer->getBankName(), imsPlayer->getInstruments(), QFileInfo(currentFile).suffix().toUpper());
                        channelMonitor->updateVoiceInstrumentNames(imsPlayer->getVoiceInstrumentNames());
                    }
                }
            }
        }
        
        QMessageBox::information(this, "Bank Selection",
            "External bank set to: " + QFileInfo(fileName).fileName() +
            "\nIt applies to " + (isGyb ? "GYB" : (isOka ? "OKA" : "IMS/ROL/SOP")) + " files.");
    }
}


void MainWindow::showHelpDialog()
{
    static bool isHelpOpen = false;
    if (isHelpOpen) return;
    isHelpOpen = true;

    QMessageBox helpBox(this);
    helpBox.setIcon(QMessageBox::Information);

#ifdef ENGLISH_UI
    helpBox.setWindowTitle(QString::fromUtf8("Keyboard Shortcuts & Features"));

    QString helpText = QString::fromUtf8(
        "<h3>🎵 JJoMe MIDI Player &mdash; Shortcuts &amp; Features</h3>"
        "<p>The full list of keyboard shortcuts and special features available in the player.</p>"
        "<table border='1' cellpadding='5' cellspacing='0' style='border-collapse: collapse; border-color: #666666; font-family: sans-serif; font-size: 12px;'>"
        "<tr bgcolor='#2c3e50' style='color: white;'><th align='left'><b>Shortcut</b></th><th align='left'><b>Description</b></th></tr>"
        "<tr><td><b>F1</b></td><td>Open this keyboard shortcuts &amp; features help window</td></tr>"
        "<tr><td><b>Space</b></td><td>Pause / Resume the current song &mdash; press Enter to start a new song</td></tr>"
        "<tr><td><b>Enter / Return</b></td><td>Enter the selected folder / Start playing the selected song</td></tr>"
        "<tr><td><b>Delete</b></td><td>Permanently remove the selected song from the playlist</td></tr>"
        "<tr><td><b>Left Arrow (&#8592;)</b></td><td>Rewind 5 seconds</td></tr>"
        "<tr><td><b>Right Arrow (&#8594;)</b></td><td>Fast-forward 5 seconds</td></tr>"
        "<tr><td><b>Up / Down Arrows (&#8593; &#8595;)</b></td><td>Move up/down in the playlist (whether playing or not)</td></tr>"
        "<tr><td><b>+ / = key</b></td><td>Master volume +5%</td></tr>"
        "<tr><td><b>- key</b></td><td>Master volume -5%</td></tr>"
        "<tr><td><b>F7</b></td><td>Tempo (playback speed) -5% (min 50%)</td></tr>"
        "<tr><td><b>F8</b></td><td>Tempo (playback speed) +5% (max 150%)</td></tr>"
        "<tr><td><b>F9</b></td><td>Key transpose -1 semitone (min -6, all formats)</td></tr>"
        "<tr><td><b>F10</b></td><td>Key transpose +1 semitone (max +6, all formats)</td></tr>"
        "<tr><td><b>F11</b></td><td>Reset tempo &amp; key to the original speed/pitch</td></tr>"
        "<tr><td><b>F12</b></td><td>Open the OPL-like stereo &amp; performance mode settings (1-9)</td></tr>"
        "</table>"
        "<br/>"
        "<b>[Supported music file extensions]</b><br/>"
        "&#8226; <b>*.mid, *.midi, *.nob, *.ims, *.rol, *.sop, *.gyb, *.oka, *.okm, *.vgm, *.vgz</b><br/>"
    );
#else
    helpBox.setWindowTitle(QString::fromUtf8("단축키 및 기능 도움말"));

    QString helpText = QString::fromUtf8(
        "<h3>🎵 JJoMe MIDI Player 단축키 및 기능 안내</h3>"
        "<p>플레이어에서 사용할 수 있는 전체 단축키와 특수 기능 목록입니다.</p>"
        "<table border='1' cellpadding='5' cellspacing='0' style='border-collapse: collapse; border-color: #666666; font-family: sans-serif; font-size: 12px;'>"
        "<tr bgcolor='#2c3e50' style='color: white;'><th align='left'><b>단축키</b></th><th align='left'><b>기능 설명</b></th></tr>"
        "<tr><td><b>F1</b></td><td>단축키 및 기능 도움말 안내 창 열기</td></tr>"
        "<tr><td><b>Space</b></td><td>현재 곡 일시정지 / 재개 (Pause / Resume) — 새 곡 시작은 Enter</td></tr>"
        "<tr><td><b>Enter / Return</b></td><td>선택한 폴더로 이동 / 선택한 곡 재생 시작</td></tr>"
        "<tr><td><b>Delete</b></td><td>선택한 곡을 플레이리스트에서 영구 삭제</td></tr>"
        "<tr><td><b>방향키 왼쪽 (←)</b></td><td>이전 구간으로 5초 되감기 (Rewind)</td></tr>"
        "<tr><td><b>방향키 오른쪽 (→)</b></td><td>다음 구간으로 5초 빨리감기 (Fast Forward)</td></tr>"
        "<tr><td><b>방향키 위/아래 (↑ ↓)</b></td><td>플레이리스트 위/아래로 이동 (재생 여부와 무관)</td></tr>"
        "<tr><td><b>+ / = 키</b></td><td>마스터 볼륨 5% 증가</td></tr>"
        "<tr><td><b>- 키</b></td><td>마스터 볼륨 5% 감소</td></tr>"
        "<tr><td><b>F7</b></td><td>연주 템포(재생 속도) 5% 감소 (최소 50%)</td></tr>"
        "<tr><td><b>F8</b></td><td>연주 템포(재생 속도) 5% 증가 (최대 150%)</td></tr>"
        "<tr><td><b>F9</b></td><td>연주 음정(Key Transpose) 1음 감소 (최소 -6, 전 포맷 지원)</td></tr>"
        "<tr><td><b>F10</b></td><td>연주 음정(Key Transpose) 1음 증가 (최대 +6, 전 포맷 지원)</td></tr>"
        "<tr><td><b>F11</b></td><td>변경된 템포 및 연주 키를 원곡 속도/음정으로 초기화 (템포/키 변경 지원)</td></tr>"
        "<tr><td><b>F12</b></td><td>OPL 유사 스테레오 및 연주모드 설정 창 열기 (1~9)</td></tr>"
        "</table>"
        "<br/>"
        "<b>[지원 음악 파일 확장자]</b><br/>"
        "• <b>*.mid, *.midi, *.nob, *.ims, *.rol, *.sop, *.gyb, *.oka, *.okm, *.vgm, *.vgz</b><br/>"
    );
#endif
    
    helpBox.setText(helpText);
    helpBox.exec();
    isHelpOpen = false;
}


