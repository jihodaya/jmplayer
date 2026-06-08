#ifndef GYBFILEHANDLER_H
#define GYBFILEHANDLER_H

#include <QString>
#include <QByteArray>
#include <QList>

class GybFileHandler {
public:
    struct ChannelInfo {
        int channelId;          // 0..10 (logical channel index from the file)
        int noteStartOffset;    // absolute file offset where (cmd, dur) pairs start
        int noteEndOffset;      // absolute file offset where the note stream ends
        int endTick;            // total duration of this channel in 60Hz ticks
        int programCount;       // number of program-change events (4 bytes each)
        int programOffset;      // absolute file offset to program-change events
        int volumeCount;        // number of volume events
        int volumeOffset;       // absolute file offset to volume events
        int pitchCount;         // number of pitch events
        int pitchOffset;        // absolute file offset to pitch events
        bool isLyrics;          // unused for sequential GYB (kept for source compat)
        int headerOffset;       // unused (kept for source compat)
        int startOffset;        // alias for noteStartOffset (kept for source compat)
        int endOffset;          // alias for noteEndOffset (kept for source compat)
    };

    // Embedded OPL instrument data (38 bytes per slot: 9-byte name + 29-byte params).
    // The instrument table sits at the very end of the GYB payload, right after
    // channel 10's data. Header byte at offset 0x3C is the count (typically 13).
    struct InstrumentInfo {
        QString name;
        QByteArray oplParams; // 29 raw bytes (modulator 14 + carrier 14 + 1 extra)
    };

    static bool isGybFile(const QString& filePath);
    static QString extractTitle(const QString& filePath);
    static QString extractTitle(const QByteArray& fileData);
    static QString extractTitleFromLst(const QString& gybFilePath);

    // Detect the likely melody/score channel (1-based) by finding the channel
    // with the earliest Note On event.
    static int detectMelodyChannel(const QString& filePath);

    // Parse the channel pointer table at header[0x26..0x3F]. Returns valid channels
    // sorted by startOffset. End offsets are computed as the next channel's start.
    static QList<ChannelInfo> parseChannelTable(const QString& filePath);

    // Read the embedded instrument table that immediately follows channel 10's
    // pitch-event block. Returns all entries in file order (slot 0 is usually empty,
    // slots 1..N are named OPL patches matching the channel program-change indices).
    static QList<InstrumentInfo> parseInstrumentTable(const QString& filePath);

    // Extract lyrics (embedded at the end of the file after the instrument table)
    static QStringList extractLyrics(const QString& filePath);

    // Analyze GYB channels and return a list of ticks corresponding to lyric syllables
    static QList<unsigned long> extractLyricMarkerTicks(const QString& filePath, const QStringList& lyrics);

    // Line-level lyric sync: return one playback tick per displayed lyric LINE.
    // Hybrid model — anchors the first line to the melody channel's first vocal
    // note (intro end), then distributes the remaining lines proportionally by
    // their byte position in the lyric stream up to the song end. Far more
    // robust than per-syllable timing across thousands of songs; coarse line
    // granularity tolerates the ±1-2s absolute error that per-syllable sync
    // exposes. `lyrics` must be the displayed line list (1 entry per line).
    static QList<unsigned long> extractLyricLineTicks(const QString& filePath, const QStringList& lyrics);

    // Per-SYLLABLE GYB-tick list reproducing nore45's GYB->OKA byte-scroll:
    // each Johab syllable's tick = its byte offset in the lyric stream *
    // (tbDiv * 256 / 1200) GYB ticks/byte (0.2133 beats/byte, BPM-independent).
    // Returns one tick per Johab syllable in reading order.
    static QList<unsigned long> extractLyricSyllableTicks(const QString& filePath);

    // Read raw file data (for the player to access via offsets directly).
    static QByteArray readWholeFile(const QString& filePath);

    // Legacy / kept for compatibility — uses MAGIC byte at offset 0
    static QByteArray extractEventStream(const QString& filePath);
    static QString decodeJohab(const QByteArray& data);

private:

    // GYB file format constants (reverse-engineered from GAYOBANG.EXE)
    // Header layout:
    //   0x00-0x3F: 64-byte primary header
    //     0x00: Magic (0x04 or 0x03)
    //     0x01-0x19: Johab-encoded title (NUL-padded)
    //     ...other config bytes (instrument count, BPM, time-sig, transpose offset)
    //   0x40-0x4E: 15-byte secondary header
    //   0x4F-end: Sequence payload (parsed sequentially), then instrument bank trailer
    // Primary header is always 64 bytes (0x40). Magic 0x04 files add a 15-byte secondary
    // header (DAT_445a_8d6a = 0x0F in GAYOBANG.EXE FUN_28a1_113b), so the payload starts at
    // 0x4F. Magic 0x03 files skip the secondary header — payload starts at 0x40.
    static constexpr int GYB_HEADER_SIZE       = 0x40;  // 64 bytes primary header
    static constexpr int GYB_HEADER2_SIZE_V4   = 0x0F;
    static constexpr int GYB_PAYLOAD_OFFSET_V4 = 0x4F;
    static constexpr int GYB_PAYLOAD_OFFSET_V3 = 0x40;
    static constexpr int GYB_MAGIC_OFFSET    = 0x00;
    static constexpr int GYB_MAGIC_VALUE_A   = 0x04;
    static constexpr int GYB_MAGIC_VALUE_B   = 0x03;
    static constexpr int GYB_TITLE_OFFSET    = 0x01;
    static constexpr int GYB_TITLE_SIZE      = 0x19;
    static constexpr int GYB_NUM_CHANNELS    = 11;    // Channels 0..10
    // Header config bytes (uint16 LE unless noted):
    static constexpr int GYB_TS_NUM_OFFSET   = 0x2A;  // byte: time signature numerator
    static constexpr int GYB_TS_DEN_OFFSET   = 0x2B;  // byte: time signature denominator (= 4)
    static constexpr int GYB_BPM_OFFSET      = 0x32;  // uint16: default BPM (80, 100, 110…)
public:
    // Iyagi-style tickBeat divisor. Stored at byte 0x28 as `tickBeat / 60`
    // (so 4 → 240, 8 → 480, 12 → 720). The playback rate is
    //     rate_Hz = basicTempo × header[0x28] / 60
    // exactly mirroring AdPlug's IMS formula (`basicTempo × tickBeat / 60`).
    // This was missed in earlier reverse engineering because all the
    // sample songs we cross-validated (M_BORA pair) happened to use 4.
    static constexpr int GYB_TICKBEAT_OFFSET = 0x28;
private:
    static constexpr int GYB_INST_COUNT_OFFSET = 0x3C; // uint16: instrument count (= 13)
    static constexpr int GYB_INST_RECORD_SIZE  = 38;   // 9 name + 29 params
    static constexpr int GYB_INST_NAME_SIZE    = 9;
    static constexpr int GYB_INST_PARAM_SIZE   = 29;
};

#endif // GYBFILEHANDLER_H
