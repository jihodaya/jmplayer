#ifndef CONSTANTS_H
#define CONSTANTS_H

// Channel Monitor UI Constants
namespace UI {
    namespace ChannelMonitor {
        constexpr int CHANNEL_LABEL_WIDTH = 25;
        constexpr int PROGRAM_LABEL_WIDTH = 30;
        constexpr int INSTRUMENT_LABEL_WIDTH = 160;
        constexpr int NOTES_LABEL_WIDTH = 150;
        constexpr int VOLUME_BAR_WIDTH = 80;
        constexpr int VOLUME_BAR_HEIGHT = 16;
        constexpr int CHANNEL_WIDGET_HEIGHT = 24;
        constexpr int DECAY_TIMER_INTERVAL = 200;
    }

    namespace MainWindow {
        constexpr int TRACK_INFO_HEIGHT = 80;
    }
}

// MIDI Constants
namespace MIDI {
    constexpr int HIGH_VELOCITY_THRESHOLD = 80;
    constexpr int MAX_CHANNELS = 16;
}

// CSS Style Constants
namespace Styles {
    namespace ChannelMonitor {
        constexpr const char* CHANNEL_LABEL_DEFAULT = "font-weight: bold; color: white;";
        constexpr const char* PROGRAM_LABEL_DEFAULT = "color: #00FFFF; font-size: 11px;";
        constexpr const char* INSTRUMENT_LABEL_DEFAULT = "color: #FFAA00; font-size: 10px; font-family: monospace;";
        constexpr const char* NOTES_LABEL_DEFAULT = "color: #00FF00; font-family: monospace; font-size: 10px;";

        constexpr const char* CHANNEL_LABEL_HIGH = "font-weight: bold; color: #FF4444; background-color: #444444;";
        constexpr const char* CHANNEL_LABEL_MEDIUM = "font-weight: bold; color: #FFFF44; background-color: #333333;";
        constexpr const char* CHANNEL_LABEL_LOW = "font-weight: bold; color: #44FF44; background-color: #222222;";

        constexpr const char* PROGRAM_ACTIVE = "color: #FF88FF; font-size: 11px; font-weight: bold; background-color: #333333;";
        constexpr const char* INSTRUMENT_ACTIVE = "color: #FF88FF; font-size: 10px; font-weight: bold; background-color: #333333; font-family: monospace;";
    }
}

#endif // CONSTANTS_H