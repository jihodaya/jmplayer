// GYB reader.
//
// Payload starts at 0x40 (magic 0x03) or 0x4F (magic 0x04):
//   u16 global event count, then (tick u16, value u16) pairs -- the tempo map
//   per channel 0..10:
//     u8  channel id
//     u16 end tick
//     (cmd, duration) pairs until the durations reach the end tick
//       cmd 0x00       rest
//       cmd 0x01..0x78 note, held for `duration`
//       cmd >= 0x79    note off, pitch = cmd - 0x79
//     u16 count + (tick u16, value u8, u8) records, three times:
//       program change, channel volume, pitch
//   instrument table, then the lyric stream
//
// Conversion rules, each verified against the .OKA files NORE45 produced:
//   SMF division  = (tick_beat & 0xFF) * 30
//   SMF tick      = GYB tick * 30
//   velocity      = channel volume in effect, scaled v * 128 / 100 clamped 127
//   program       -> Cn, volume -> Bn 07 (same scale), pitch -> En as
//                    8192 + (value - 10) * 300
//   control events at or past the channel end tick are dropped
//   note offs are dropped by NORE45 but we keep them for the MIDI side
#pragma once

#include <string>

#include "song.h"

namespace jmpconv {

struct GybFile {
    int tickBeat = 0;                 // low byte is the real value
    int headerBpm = 0;                // 0x32 -- NOT the playback tempo
    double baseBpm = 100.0;           // 0x34 / 100 -- the real base tempo
    int timeSigNum = 4, timeSigDen = 4;
    std::vector<TempoEvent> globals;  // usecPerQuarter left unresolved
    std::vector<int> globalValues;    // raw relative tempo values
    std::vector<uint32_t> globalTicks;
    std::vector<Track> tracks;
    std::vector<Instrument> instruments;
    Bytes lyricStream;
    bool valid = false;
};

bool gybRead(const Bytes& raw, GybFile& out);
bool gybReadFile(const std::string& path, GybFile& out);

inline int gybVolumeToMidi(int v) {
    int s = v * 128 / 100;
    return s > 127 ? 127 : s;
}

inline int gybPitchToBend(int v) { return 8192 + (v - 10) * 300; }

constexpr int kGybTickScale = 30;
constexpr int kGybNoteOffBias = 0x79;

// Global tempo values are relative; the base they scale against sits at 0x34
// as BPM x 100. Pass a positive `baseBpmOverride` to ignore the file's value.
Song gybToSong(const GybFile& g, double baseBpmOverride = 0.0);
Bytes gybWrite(const GybFile& g);
Bytes gybToOka(const GybFile& g, double baseBpmOverride = 0.0);

}  // namespace jmpconv
