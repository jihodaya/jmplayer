/****************************************************************************
** Meta object code from reading C++ file 'mainwindow.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../mainwindow.h"
#include <QtGui/qtextcursor.h>
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'mainwindow.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.9.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN10MainWindowE_t {};
} // unnamed namespace

template <> constexpr inline auto MainWindow::qt_create_metaobjectdata<qt_meta_tag_ZN10MainWindowE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "MainWindow",
        "openFile",
        "",
        "openFolder",
        "removeFile",
        "sortFiles",
        "savePlaylist",
        "loadPlaylist",
        "showPlaylistMenu",
        "toggleChannelMonitor",
        "toggleLyricsWindow",
        "onLyricsEdited",
        "newLyrics",
        "playPause",
        "stop",
        "onNoteOn",
        "channel",
        "note",
        "velocity",
        "onNoteOff",
        "onProgramChange",
        "program",
        "onControllerChange",
        "controller",
        "value",
        "previousTrack",
        "nextTrack",
        "rewind",
        "fastForward",
        "onVolumeChanged",
        "onPositionChanged",
        "onFileSelected",
        "onFileDoubleClicked",
        "updatePosition",
        "forceChannelUpdate",
        "checkWindowPosition",
        "onDeviceChanged",
        "index",
        "onDeviceRefresh",
        "onCleanupPlaylist",
        "onSearchTextChanged",
        "onRepeatModeChanged",
        "onPlaybackFinished",
        "showFileInfo",
        "filePath",
        "addMidiFiles",
        "filePaths",
        "findMidiFilesInDirectory",
        "dirPath",
        "addFolderToPlaylist",
        "folderPath",
        "navigateToFolder",
        "navigateToFolderWithoutHistory",
        "handleFolderDoubleClick",
        "getCurrentPath",
        "setCurrentPath",
        "path",
        "updateAllowedPaths",
        "isPathAllowed",
        "isOplFile",
        "isGybFile",
        "isOkaFile",
        "isOkaOplFile",
        "onLyricChannelChanged",
        "newChannel",
        "updateWindowTitle",
        "toggleDsp",
        "togglePianoRoll",
        "updateDspButtonStyle",
        "onSelectBankFile",
        "showHelpDialog",
        "toggleRecording",
        "handleNewIpcConnection"
    };

    QtMocHelpers::UintData qt_methods {
        // Slot 'openFile'
        QtMocHelpers::SlotData<void()>(1, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'openFolder'
        QtMocHelpers::SlotData<void()>(3, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'removeFile'
        QtMocHelpers::SlotData<void()>(4, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'sortFiles'
        QtMocHelpers::SlotData<void()>(5, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'savePlaylist'
        QtMocHelpers::SlotData<void()>(6, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'loadPlaylist'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'showPlaylistMenu'
        QtMocHelpers::SlotData<void()>(8, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'toggleChannelMonitor'
        QtMocHelpers::SlotData<void()>(9, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'toggleLyricsWindow'
        QtMocHelpers::SlotData<void()>(10, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onLyricsEdited'
        QtMocHelpers::SlotData<void(const QStringList &)>(11, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QStringList, 12 },
        }}),
        // Slot 'playPause'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'stop'
        QtMocHelpers::SlotData<void()>(14, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onNoteOn'
        QtMocHelpers::SlotData<void(int, int, int)>(15, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 16 }, { QMetaType::Int, 17 }, { QMetaType::Int, 18 },
        }}),
        // Slot 'onNoteOff'
        QtMocHelpers::SlotData<void(int, int)>(19, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 16 }, { QMetaType::Int, 17 },
        }}),
        // Slot 'onProgramChange'
        QtMocHelpers::SlotData<void(int, int)>(20, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 16 }, { QMetaType::Int, 21 },
        }}),
        // Slot 'onControllerChange'
        QtMocHelpers::SlotData<void(int, int, int)>(22, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 16 }, { QMetaType::Int, 23 }, { QMetaType::Int, 24 },
        }}),
        // Slot 'previousTrack'
        QtMocHelpers::SlotData<void()>(25, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'nextTrack'
        QtMocHelpers::SlotData<void()>(26, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'rewind'
        QtMocHelpers::SlotData<void()>(27, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'fastForward'
        QtMocHelpers::SlotData<void()>(28, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onVolumeChanged'
        QtMocHelpers::SlotData<void(int)>(29, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 24 },
        }}),
        // Slot 'onPositionChanged'
        QtMocHelpers::SlotData<void(int)>(30, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 24 },
        }}),
        // Slot 'onFileSelected'
        QtMocHelpers::SlotData<void()>(31, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onFileDoubleClicked'
        QtMocHelpers::SlotData<void()>(32, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updatePosition'
        QtMocHelpers::SlotData<void()>(33, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'forceChannelUpdate'
        QtMocHelpers::SlotData<void()>(34, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'checkWindowPosition'
        QtMocHelpers::SlotData<void()>(35, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onDeviceChanged'
        QtMocHelpers::SlotData<void(int)>(36, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 37 },
        }}),
        // Slot 'onDeviceRefresh'
        QtMocHelpers::SlotData<void()>(38, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onCleanupPlaylist'
        QtMocHelpers::SlotData<void()>(39, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onSearchTextChanged'
        QtMocHelpers::SlotData<void()>(40, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onRepeatModeChanged'
        QtMocHelpers::SlotData<void()>(41, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onPlaybackFinished'
        QtMocHelpers::SlotData<void()>(42, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'showFileInfo'
        QtMocHelpers::SlotData<void(const QString &)>(43, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 44 },
        }}),
        // Slot 'addMidiFiles'
        QtMocHelpers::SlotData<void(const QStringList &)>(45, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QStringList, 46 },
        }}),
        // Slot 'findMidiFilesInDirectory'
        QtMocHelpers::SlotData<QStringList(const QString &)>(47, 2, QMC::AccessPrivate, QMetaType::QStringList, {{
            { QMetaType::QString, 48 },
        }}),
        // Slot 'addFolderToPlaylist'
        QtMocHelpers::SlotData<void(const QString &)>(49, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 50 },
        }}),
        // Slot 'navigateToFolder'
        QtMocHelpers::SlotData<void(const QString &)>(51, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 50 },
        }}),
        // Slot 'navigateToFolderWithoutHistory'
        QtMocHelpers::SlotData<void(const QString &)>(52, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 50 },
        }}),
        // Slot 'handleFolderDoubleClick'
        QtMocHelpers::SlotData<void(const QString &)>(53, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 50 },
        }}),
        // Slot 'getCurrentPath'
        QtMocHelpers::SlotData<QString() const>(54, 2, QMC::AccessPrivate, QMetaType::QString),
        // Slot 'setCurrentPath'
        QtMocHelpers::SlotData<void(const QString &)>(55, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 56 },
        }}),
        // Slot 'updateAllowedPaths'
        QtMocHelpers::SlotData<void()>(57, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'isPathAllowed'
        QtMocHelpers::SlotData<bool(const QString &) const>(58, 2, QMC::AccessPrivate, QMetaType::Bool, {{
            { QMetaType::QString, 56 },
        }}),
        // Slot 'isOplFile'
        QtMocHelpers::SlotData<bool(const QString &) const>(59, 2, QMC::AccessPrivate, QMetaType::Bool, {{
            { QMetaType::QString, 44 },
        }}),
        // Slot 'isGybFile'
        QtMocHelpers::SlotData<bool(const QString &) const>(60, 2, QMC::AccessPrivate, QMetaType::Bool, {{
            { QMetaType::QString, 44 },
        }}),
        // Slot 'isOkaFile'
        QtMocHelpers::SlotData<bool(const QString &) const>(61, 2, QMC::AccessPrivate, QMetaType::Bool, {{
            { QMetaType::QString, 44 },
        }}),
        // Slot 'isOkaOplFile'
        QtMocHelpers::SlotData<bool(const QString &) const>(62, 2, QMC::AccessPrivate, QMetaType::Bool, {{
            { QMetaType::QString, 44 },
        }}),
        // Slot 'onLyricChannelChanged'
        QtMocHelpers::SlotData<void(int)>(63, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 64 },
        }}),
        // Slot 'updateWindowTitle'
        QtMocHelpers::SlotData<void()>(65, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'toggleDsp'
        QtMocHelpers::SlotData<void()>(66, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'togglePianoRoll'
        QtMocHelpers::SlotData<void()>(67, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateDspButtonStyle'
        QtMocHelpers::SlotData<void()>(68, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onSelectBankFile'
        QtMocHelpers::SlotData<void()>(69, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'showHelpDialog'
        QtMocHelpers::SlotData<void()>(70, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'toggleRecording'
        QtMocHelpers::SlotData<void()>(71, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'handleNewIpcConnection'
        QtMocHelpers::SlotData<void()>(72, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<MainWindow, qt_meta_tag_ZN10MainWindowE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject MainWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10MainWindowE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10MainWindowE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN10MainWindowE_t>.metaTypes,
    nullptr
} };

void MainWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<MainWindow *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->openFile(); break;
        case 1: _t->openFolder(); break;
        case 2: _t->removeFile(); break;
        case 3: _t->sortFiles(); break;
        case 4: _t->savePlaylist(); break;
        case 5: _t->loadPlaylist(); break;
        case 6: _t->showPlaylistMenu(); break;
        case 7: _t->toggleChannelMonitor(); break;
        case 8: _t->toggleLyricsWindow(); break;
        case 9: _t->onLyricsEdited((*reinterpret_cast< std::add_pointer_t<QStringList>>(_a[1]))); break;
        case 10: _t->playPause(); break;
        case 11: _t->stop(); break;
        case 12: _t->onNoteOn((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[3]))); break;
        case 13: _t->onNoteOff((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2]))); break;
        case 14: _t->onProgramChange((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2]))); break;
        case 15: _t->onControllerChange((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[3]))); break;
        case 16: _t->previousTrack(); break;
        case 17: _t->nextTrack(); break;
        case 18: _t->rewind(); break;
        case 19: _t->fastForward(); break;
        case 20: _t->onVolumeChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 21: _t->onPositionChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 22: _t->onFileSelected(); break;
        case 23: _t->onFileDoubleClicked(); break;
        case 24: _t->updatePosition(); break;
        case 25: _t->forceChannelUpdate(); break;
        case 26: _t->checkWindowPosition(); break;
        case 27: _t->onDeviceChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 28: _t->onDeviceRefresh(); break;
        case 29: _t->onCleanupPlaylist(); break;
        case 30: _t->onSearchTextChanged(); break;
        case 31: _t->onRepeatModeChanged(); break;
        case 32: _t->onPlaybackFinished(); break;
        case 33: _t->showFileInfo((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 34: _t->addMidiFiles((*reinterpret_cast< std::add_pointer_t<QStringList>>(_a[1]))); break;
        case 35: { QStringList _r = _t->findMidiFilesInDirectory((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast< QStringList*>(_a[0]) = std::move(_r); }  break;
        case 36: _t->addFolderToPlaylist((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 37: _t->navigateToFolder((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 38: _t->navigateToFolderWithoutHistory((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 39: _t->handleFolderDoubleClick((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 40: { QString _r = _t->getCurrentPath();
            if (_a[0]) *reinterpret_cast< QString*>(_a[0]) = std::move(_r); }  break;
        case 41: _t->setCurrentPath((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 42: _t->updateAllowedPaths(); break;
        case 43: { bool _r = _t->isPathAllowed((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast< bool*>(_a[0]) = std::move(_r); }  break;
        case 44: { bool _r = _t->isOplFile((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast< bool*>(_a[0]) = std::move(_r); }  break;
        case 45: { bool _r = _t->isGybFile((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast< bool*>(_a[0]) = std::move(_r); }  break;
        case 46: { bool _r = _t->isOkaFile((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast< bool*>(_a[0]) = std::move(_r); }  break;
        case 47: { bool _r = _t->isOkaOplFile((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast< bool*>(_a[0]) = std::move(_r); }  break;
        case 48: _t->onLyricChannelChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 49: _t->updateWindowTitle(); break;
        case 50: _t->toggleDsp(); break;
        case 51: _t->togglePianoRoll(); break;
        case 52: _t->updateDspButtonStyle(); break;
        case 53: _t->onSelectBankFile(); break;
        case 54: _t->showHelpDialog(); break;
        case 55: _t->toggleRecording(); break;
        case 56: _t->handleNewIpcConnection(); break;
        default: ;
        }
    }
}

const QMetaObject *MainWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MainWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10MainWindowE_t>.strings))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int MainWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 57)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 57;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 57)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 57;
    }
    return _id;
}
QT_WARNING_POP
