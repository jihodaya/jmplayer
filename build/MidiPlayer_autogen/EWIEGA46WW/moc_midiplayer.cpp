/****************************************************************************
** Meta object code from reading C++ file 'midiplayer.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../midiplayer.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'midiplayer.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN10MidiPlayerE_t {};
} // unnamed namespace

template <> constexpr inline auto MidiPlayer::qt_create_metaobjectdata<qt_meta_tag_ZN10MidiPlayerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "MidiPlayer",
        "positionChanged",
        "",
        "position",
        "finished",
        "errorOccurred",
        "error",
        "noteOn",
        "channel",
        "note",
        "velocity",
        "noteOff",
        "controllerChange",
        "controller",
        "value",
        "programChange",
        "program",
        "soundModeDetected",
        "mode",
        "soundModeReliabilityChanged",
        "SoundModeReliability",
        "reliability",
        "processEvents",
        "updateVolumeToDevice",
        "performSeek"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'positionChanged'
        QtMocHelpers::SignalData<void(unsigned long)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::ULong, 3 },
        }}),
        // Signal 'finished'
        QtMocHelpers::SignalData<void()>(4, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'errorOccurred'
        QtMocHelpers::SignalData<void(const QString &)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 6 },
        }}),
        // Signal 'noteOn'
        QtMocHelpers::SignalData<void(int, int, int)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 8 }, { QMetaType::Int, 9 }, { QMetaType::Int, 10 },
        }}),
        // Signal 'noteOff'
        QtMocHelpers::SignalData<void(int, int)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 8 }, { QMetaType::Int, 9 },
        }}),
        // Signal 'controllerChange'
        QtMocHelpers::SignalData<void(int, int, int)>(12, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 8 }, { QMetaType::Int, 13 }, { QMetaType::Int, 14 },
        }}),
        // Signal 'programChange'
        QtMocHelpers::SignalData<void(int, int)>(15, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 8 }, { QMetaType::Int, 16 },
        }}),
        // Signal 'soundModeDetected'
        QtMocHelpers::SignalData<void(int)>(17, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 18 },
        }}),
        // Signal 'soundModeReliabilityChanged'
        QtMocHelpers::SignalData<void(const SoundModeReliability &)>(19, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 20, 21 },
        }}),
        // Slot 'processEvents'
        QtMocHelpers::SlotData<void()>(22, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'updateVolumeToDevice'
        QtMocHelpers::SlotData<void()>(23, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'performSeek'
        QtMocHelpers::SlotData<void()>(24, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<MidiPlayer, qt_meta_tag_ZN10MidiPlayerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject MidiPlayer::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10MidiPlayerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10MidiPlayerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN10MidiPlayerE_t>.metaTypes,
    nullptr
} };

void MidiPlayer::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<MidiPlayer *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->positionChanged((*reinterpret_cast< std::add_pointer_t<ulong>>(_a[1]))); break;
        case 1: _t->finished(); break;
        case 2: _t->errorOccurred((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 3: _t->noteOn((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[3]))); break;
        case 4: _t->noteOff((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2]))); break;
        case 5: _t->controllerChange((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[3]))); break;
        case 6: _t->programChange((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2]))); break;
        case 7: _t->soundModeDetected((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 8: _t->soundModeReliabilityChanged((*reinterpret_cast< std::add_pointer_t<SoundModeReliability>>(_a[1]))); break;
        case 9: _t->processEvents(); break;
        case 10: _t->updateVolumeToDevice(); break;
        case 11: _t->performSeek(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (MidiPlayer::*)(unsigned long )>(_a, &MidiPlayer::positionChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (MidiPlayer::*)()>(_a, &MidiPlayer::finished, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (MidiPlayer::*)(const QString & )>(_a, &MidiPlayer::errorOccurred, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (MidiPlayer::*)(int , int , int )>(_a, &MidiPlayer::noteOn, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (MidiPlayer::*)(int , int )>(_a, &MidiPlayer::noteOff, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (MidiPlayer::*)(int , int , int )>(_a, &MidiPlayer::controllerChange, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (MidiPlayer::*)(int , int )>(_a, &MidiPlayer::programChange, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (MidiPlayer::*)(int )>(_a, &MidiPlayer::soundModeDetected, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (MidiPlayer::*)(const SoundModeReliability & )>(_a, &MidiPlayer::soundModeReliabilityChanged, 8))
            return;
    }
}

const QMetaObject *MidiPlayer::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MidiPlayer::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10MidiPlayerE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int MidiPlayer::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 12)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 12;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 12)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 12;
    }
    return _id;
}

// SIGNAL 0
void MidiPlayer::positionChanged(unsigned long _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void MidiPlayer::finished()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void MidiPlayer::errorOccurred(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void MidiPlayer::noteOn(int _t1, int _t2, int _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2, _t3);
}

// SIGNAL 4
void MidiPlayer::noteOff(int _t1, int _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1, _t2);
}

// SIGNAL 5
void MidiPlayer::controllerChange(int _t1, int _t2, int _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1, _t2, _t3);
}

// SIGNAL 6
void MidiPlayer::programChange(int _t1, int _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1, _t2);
}

// SIGNAL 7
void MidiPlayer::soundModeDetected(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 7, nullptr, _t1);
}

// SIGNAL 8
void MidiPlayer::soundModeReliabilityChanged(const SoundModeReliability & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 8, nullptr, _t1);
}
QT_WARNING_POP
