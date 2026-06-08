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

        // Drawing rectangles
        int filenameWidth = 130;
        QRect filenameRect = opt.rect.adjusted(5, 0, 0, 0);
        filenameRect.setWidth(filenameWidth);
        
        QRect titleRect = opt.rect;
        titleRect.setLeft(opt.rect.left() + filenameWidth + 5);

        // Set pen color
        painter->setPen(Qt::white);
        
        // Draw filename
        painter->drawText(filenameRect, Qt::AlignLeft | Qt::AlignVCenter, filenamePart);
        
        // Draw title/artist if exists
        if (!titlePart.isEmpty()) {
            painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, titlePart);
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
        SoundFontManagerDialog dialog(this);
        dialog.exec();
        
        // If SoundFont was changed and internal synth is active, update MidiPlayer
        if (deviceComboBox->currentText() == "[JJoMe Synth (SoundFont)]") {
            SettingsManager& settings = SettingsManager::instance();
            QString activeSf = settings.value("Synth/SoundFontPath", "").toString();
            midiPlayer->setUseInternalSynth(true, activeSf);
            updateWindowTitle();
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
    volumeSlider->setValue(96);
    volumeValue = new QLabel("96", this);

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

void MainWindow::loadMidiDevices()
{
    // Block signals to prevent onDeviceChanged from being triggered during device list update
    deviceComboBox->blockSignals(true);

    deviceComboBox->clear();
    QStringList devices = midiPlayer->getAvailableDevices();
    
    // Add internal synth option
    devices.insert(0, "[JJoMe Synth (SoundFont)]");

    deviceComboBox->addItems(devices);
    deviceComboBox->setEnabled(true);

    // Re-enable signals
    deviceComboBox->blockSignals(false);

}

// QSettings* MainWindow::createSettings() - Replaced with SettingsManager singleton

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
                JJoMeSynth::instance().setGybPlayer(gybPlayer);
                JJoMeSynth::instance().setImsPlayer(nullptr);
                JJoMeSynth::instance().setOkaPlayer(nullptr);
                gybPlayer->play();
                qDebug() << "[MainWindow] GYB play() called";
                dspButton->show(); bankButton->show();
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
                dspButton->show(); bankButton->show();
                updateDspButtonStyle(); // Update style
                if (channelMonitor) {
                    channelMonitor->setImsMode(true, imsPlayer->getBankName(), imsPlayer->getInstruments(), QFileInfo(currentFile).suffix().toUpper());
                    channelMonitor->updateVoiceInstrumentNames(imsPlayer->getVoiceInstrumentNames());
                }
            } else if (playOkaViaOpl) {
                qDebug() << "[MainWindow] Starting OKA OPL playback";
                JJoMeSynth::instance().setGybPlayer(nullptr);
                JJoMeSynth::instance().setImsPlayer(nullptr);
                JJoMeSynth::instance().setOkaPlayer(okaPlayer);
                okaPlayer->play();
                dspButton->show(); bankButton->show();
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
                dspButton->hide(); bankButton->hide();
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

void MainWindow::onVolumeChanged(int value)
{
    midiPlayer->setVolume(value);
    // AdPlug doesn't have a direct master volume in all players, 
    // but JJoMeSynth::setVolume affects both SoundFont and IMS
    JJoMeSynth::instance().setVolume(value / 127.0f);
    volumeValue->setText(QString::number(value));

    // Save volume setting immediately
    SettingsManager& settings = SettingsManager::instance();
    settings.setValue("General/volume", value);
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

void MainWindow::onFileSelected()
{
    // FIX: Do not update currentFile, TrackInfo, or ChannelMonitor while playing.
    // This keeps the display locked to the currently playing song even if the user browses the playlist.
    if (isPlaying) return;

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
            bankButton->show();
            updateDspButtonStyle();
        } else {
            dspButton->hide();
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
    if (fileList) {
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
                        uint64_t recTick = (uint64_t)rec.kasa_tick * 8;
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

void MainWindow::resetLyricSyncState()
{
    lastDisplayedLyricIndex = -1;
    lastIssLineIdx = -1;
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
        } else {
            midiPlayer->setUseInternalSynth(false);
            int actualIndex = index - 1; // Since index 0 is virtual synth
            midiPlayer->connectToDevice(actualIndex);
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
        
        // Only show SF info if playing MIDI and using internal synth (not for OPL/GYB)
        bool isIms = isOplFile(filePath);
        bool isGyb = isGybFile(filePath);
        bool isOka = isOkaFile(filePath);
        if (isPlaying && !isIms && !isGyb && !isOka && deviceComboBox->currentText() == "[JJoMe Synth (SoundFont)]") {
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
    int volume = settings.value("General/volume", 96).toInt();
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
    if (row < 0) return;
    int itemType = plRowType(row);
    if (itemType == FOLDER || itemType == PARENT_FOLDER) {
        handleFolderDoubleClick(plRowPath(row));
    } else {
        plSetCurrentRow(row);
        onFileDoubleClicked(); // stop (if playing) + play the selected file
    }
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

// ============================================================================
// New Playlist Tree Management System
// ============================================================================

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
    QString currentPath;
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
    QString currentPath;
    if (currentNode && currentNode != playlistRoot) {
        currentPath = currentNode->fullPath;
    }
    settings.setValue("currentNodePath", currentPath);
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
        dspButton->show(); bankButton->show();
        updateDspButtonStyle();
        if (channelMonitor)
            channelMonitor->setImsMode(true, gybPlayer->getBankName(),
                                       gybPlayer->getInstruments(), "GYB");
    } else if (isIms) {
        JJoMeSynth::instance().setGybPlayer(nullptr);
        JJoMeSynth::instance().setImsPlayer(imsPlayer);
        JJoMeSynth::instance().setOkaPlayer(nullptr);
        imsPlayer->play();
        dspButton->show(); bankButton->show();
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
        dspButton->show(); bankButton->show();
        updateDspButtonStyle();
        if (channelMonitor)
            channelMonitor->setImsMode(true, okaPlayer->getBankName(),
                                       okaPlayer->getInstruments(), "OKA");
    } else {
        JJoMeSynth::instance().setGybPlayer(nullptr);
        JJoMeSynth::instance().setImsPlayer(nullptr);
        JJoMeSynth::instance().setOkaPlayer(nullptr);
        midiPlayer->play();
        dspButton->hide(); bankButton->hide();
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
        // If the entire function fails, ensure we have a minimal working state
        if (playlistModel) {
            try { playlistModel->clearRows(); } catch (...) {}
        }
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

        // Add current node path to JSON
        QString currentPath;
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
    QString currentPath;
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
                            if (!currentPath.isEmpty()) {
                                PlaylistTreeNode* node = findNodeByPath(currentPath);
                                if (node) {
                                    currentNode = node;
                                    isInBrowsingMode = (node != playlistRoot);
                                } else {
                                    // If target node no longer exists, stay at root
                                    currentNode = playlistRoot;
                                    isInBrowsingMode = false;
                                }
                            }

                            updateUIFromCurrentNode();
                            return;
                        }
                    } catch (...) {
                        // If loading failed, clean up temp root and continue with fallback
                        if (tempRoot) {
                            delete tempRoot;
                        }
                    }
                }
            }
        }
    } catch (...) {
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
                        if (!currentPath.isEmpty()) {
                            PlaylistTreeNode* node = findNodeByPath(currentPath);
                            if (node) {
                                currentNode = node;
                                isInBrowsingMode = (node != playlistRoot);
                            } else {
                                // If target node no longer exists, stay at root
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
                    // If loading failed, clean up temp root and continue with fallback
                    if (tempRoot) {
                        delete tempRoot;
                    }
                }
            }
        }
    } catch (...) {
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
                // Skip invalid entries and continue
                continue;
            }
        }
        settings.endArray();

        // Restore current node position
        try {
            QString currentPath = settings.value("currentNodePath").toString();
            if (!currentPath.isEmpty()) {
                PlaylistTreeNode* node = findNodeByPath(currentPath);
                if (node) {
                    currentNode = node;
                    isInBrowsingMode = (node != playlistRoot);
                }
            }
        } catch (...) {
            // If currentPath restoration fails, stay at root
            currentNode = playlistRoot;
            isInBrowsingMode = false;
        }

        // If we loaded from legacy format, migrate to new format
        if (size > 0) {
            try {
                savePlaylistTree();
            } catch (...) {
                // Ignore save errors during migration
            }
        }
    } catch (...) {
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
        // If validation fails, reinitialize clean tree
        initializePlaylistTree();
    }

    // Ensure UI update (with safety check)
    try {
        updateUIFromCurrentNode();
    } catch (...) {
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
                // Continue even if deletion fails
            }
        }

        // Save playlist if changes were made (only at root level to avoid multiple saves)
        if (hasChanges && node == playlistRoot) {
            try {
                triggerSavePlaylistTree();
            } catch (...) {
                // Ignore save errors during validation cleanup
            }
        }
    } catch (...) {
        // If the entire validation process fails, at least ensure node integrity
        if (node && node->children.isEmpty() && node->isVirtual) {
            return;
        }
    }
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

    QString songTitle = QStringLiteral("Lyrics");
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
        if (QApplication::activeModalWidget() != nullptr) {
            return false;
        }

        bool inSearch = (searchBox && searchBox->hasFocus());

        // Enter: enter the selected folder / play the selected file. Consumed here
        // (before the list's own Enter/Space handling) so it works regardless of
        // which widget currently holds focus.
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) && !inSearch) {
            activateSelectedPlaylistRow();
            return true;
        }
        // Space: pause/resume the CURRENT track ONLY (never starts a new selection).
        if (keyEvent->key() == Qt::Key_Space && keyEvent->modifiers() == Qt::NoModifier && !inSearch) {
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
    QString title = "🎵 JJoMe MIDI Player R2.4d";
    
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
