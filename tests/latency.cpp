// Measures how long a sound takes to reach the speakers by two routes - jmp's
// own audio device (the route the OPL, MDX and MLD-FM engines take) and
// Nuked-SC55 through the named pipe (the route the MIDI half of an MLD song
// takes when that device is selected) - by recording the machine's own output
// with a WASAPI loopback capture and timing each stimulus against it.
//
// The difference between the two is the offset an MLD song's FM half has to be
// delayed by so that it lands with its MIDI half: the two halves start on the
// same wall-clock instant and then travel through different buffers.
//
//   latency.exe            needs NukedSC55\ beside it, like the player
//
// Prints both latencies and their difference. Run it on a quiet machine - it
// records whatever else is playing.
#define MINIAUDIO_IMPLEMENTATION
#include "../miniaudio.h"

#include "../sc55bridge.h"

#include <QApplication>
#include <QThread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

namespace {

using clock = std::chrono::steady_clock;

double secondsSince(clock::time_point t0, clock::time_point t)
{
    return std::chrono::duration<double>(t - t0).count();
}

// ---- loopback capture ------------------------------------------------------

struct CaptureChunk { double time; size_t endFrame; size_t frames; };

std::mutex g_capLock;
std::vector<float> g_capture;            // mono mix
std::vector<CaptureChunk> g_chunks;
clock::time_point g_t0;
int g_capRate = 48000;

void captureCb(ma_device*, void*, const void* input, ma_uint32 frameCount)
{
    const float* in = static_cast<const float*>(input);
    const auto now = clock::now();
    std::lock_guard<std::mutex> g(g_capLock);
    for (ma_uint32 i = 0; i < frameCount; ++i)
        g_capture.push_back(0.5f * (in[i * 2] + in[i * 2 + 1]));
    g_chunks.push_back({ secondsSince(g_t0, now), g_capture.size(), frameCount });
}

// The wall time at which captured frame `idx` was delivered: chunks arrive when
// they are complete, so a frame sits (end - idx) frames before its chunk's
// timestamp.
double frameTime(size_t idx)
{
    for (const CaptureChunk& c : g_chunks) {
        if (idx < c.endFrame)
            return c.time - double(c.endFrame - idx) / g_capRate;
    }
    return -1.0;
}

// First frame after wall time `from` whose 2 ms window rises well above the
// floor measured in the 200 ms before it.
double onsetAfter(double from, double maxWait)
{
    std::lock_guard<std::mutex> g(g_capLock);
    size_t start = 0;
    while (start < g_capture.size() && frameTime(start) < from) ++start;
    const size_t win = size_t(g_capRate * 0.002);
    // noise floor
    double floor = 0.0; size_t n = 0;
    for (size_t i = (start > size_t(g_capRate * 0.2) ? start - size_t(g_capRate * 0.2) : 0); i + 1 < start; ++i) {
        floor += double(g_capture[i]) * g_capture[i]; ++n;
    }
    floor = n ? std::sqrt(floor / n) : 0.0;
    const double thr = std::max(0.004, floor * 8.0);
    for (size_t i = start; i + win < g_capture.size(); i += win / 2) {
        double e = 0.0;
        for (size_t k = 0; k < win; ++k) e += double(g_capture[i + k]) * g_capture[i + k];
        e = std::sqrt(e / win);
        if (e > thr) return frameTime(i);
        if (frameTime(i) > from + maxWait) break;
    }
    return -1.0;
}

// ---- jmp's own playback route ----------------------------------------------

std::atomic<bool> g_fire{false};
std::atomic<double> g_fireTime{-1.0};
int g_burstLeft = 0;
double g_phase = 0.0;

void playbackCb(ma_device* dev, void* output, const void*, ma_uint32 frameCount)
{
    float* out = static_cast<float*>(output);
    const int rate = int(dev->sampleRate);
    for (ma_uint32 i = 0; i < frameCount; ++i) {
        float v = 0.0f;
        if (g_fire.exchange(false)) {
            g_fireTime.store(secondsSince(g_t0, clock::now()));
            g_burstLeft = rate / 10;                       // 100 ms
            g_phase = 0.0;
        }
        if (g_burstLeft > 0) {
            v = 0.5f * float(std::sin(g_phase));
            g_phase += 2.0 * 3.14159265358979 * 1000.0 / rate;
            --g_burstLeft;
        }
        out[i * 2] = out[i * 2 + 1] = v;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    g_t0 = clock::now();

    // Loopback capture of the default output.
    ma_device cap;
    {
        ma_device_config cfg = ma_device_config_init(ma_device_type_loopback);
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = 2;
        cfg.sampleRate = 48000;
        cfg.dataCallback = captureCb;
        if (ma_device_init(nullptr, &cfg, &cap) != MA_SUCCESS) {
            fprintf(stderr, "loopback capture failed (WASAPI only)\n");
            return 1;
        }
        g_capRate = int(cap.sampleRate);
        ma_device_start(&cap);
    }

    // --- route 1: a playback device configured the way JJoMeSynth opens its own
    double ownLatency = -1.0;
    {
        ma_device dev;
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate = 49716;
        cfg.periodSizeInMilliseconds = 40;
        cfg.periods = 3;
        cfg.dataCallback = playbackCb;
        if (ma_device_init(nullptr, &cfg, &dev) != MA_SUCCESS) {
            fprintf(stderr, "playback device failed\n");
            return 1;
        }
        ma_device_start(&dev);
        QThread::msleep(1500);
        double sum = 0.0; int got = 0;
        for (int rep = 0; rep < 5; ++rep) {
            g_fire.store(true);
            QThread::msleep(700);
            const double t = g_fireTime.load();
            const double on = onsetAfter(t - 0.02, 1.0);
            if (t >= 0 && on >= 0) {
                printf("own   : written at %.3f  heard at %.3f  latency %6.1f ms\n", t, on, (on - t) * 1000);
                sum += on - t; ++got;
            } else {
                printf("own   : no onset detected\n");
            }
            QThread::msleep(300);
        }
        if (got) ownLatency = sum / got;
        ma_device_uninit(&dev);
    }

    // --- route 2: Nuked-SC55 through the pipe
    double scLatency = -1.0;
    {
        Sc55Bridge bridge;
        if (!bridge.Start()) {
            fprintf(stderr, "SC-55 did not start: %s\n", bridge.errorString().toUtf8().constData());
        } else {
            QThread::msleep(1500);
            double sum = 0.0; int got = 0;
            for (int rep = 0; rep < 5; ++rep) {
                const double t = secondsSince(g_t0, clock::now());
                bridge.SendShort(0x99, 37, 127);               // side stick, channel 10
                QThread::msleep(700);
                bridge.SendShort(0x89, 37, 0);
                const double on = onsetAfter(t - 0.02, 1.0);
                if (on >= 0) {
                    printf("sc55  : sent at    %.3f  heard at %.3f  latency %6.1f ms\n", t, on, (on - t) * 1000);
                    sum += on - t; ++got;
                } else {
                    printf("sc55  : no onset detected\n");
                }
                QThread::msleep(500);
            }
            if (got) scLatency = sum / got;
            bridge.Stop();
        }
    }

    ma_device_uninit(&cap);

    printf("\n");
    if (ownLatency >= 0) printf("own device latency : %6.1f ms\n", ownLatency * 1000);
    if (scLatency >= 0)  printf("SC-55 latency      : %6.1f ms\n", scLatency * 1000);
    if (ownLatency >= 0 && scLatency >= 0)
        printf("SC-55 minus own    : %+6.1f ms  (delay the FM half by this much)\n",
               (scLatency - ownLatency) * 1000);
    return 0;
}
