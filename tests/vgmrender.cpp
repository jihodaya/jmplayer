// Renders a .vgm/.vgz with our own engine, and checks the file parsed the way
// its own header says it should.
//
// A VGM carries its own answer to the one question that matters for the parser:
// the header declares the total number of samples the song lasts. Walking the
// command stream and adding up the waits has to arrive at that number. If it
// does not, some command was consumed at the wrong width - which is the failure
// that turned MDX into noise, and the check that would have caught it in a day.
//
//   vgmrender <file.vgm|.vgz> <out.wav> [seconds]
#include "../vgmcore/vgmbackend.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize(size_t(n));
    const size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(got);
    return got > 0;
}

void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

bool writeWav(const std::string& path, const std::vector<int16_t>& pcm, int rate)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t bytes = uint32_t(pcm.size() * 2);
    fwrite("RIFF", 1, 4, f);  put32(f, 36 + bytes);   fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);  put32(f, 16);           put16(f, 1);  put16(f, 2);
    put32(f, uint32_t(rate)); put32(f, uint32_t(rate * 4));
    put16(f, 4);              put16(f, 16);
    fwrite("data", 1, 4, f);  put32(f, bytes);
    fwrite(pcm.data(), 1, bytes, f);
    fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("usage: vgmrender <file.vgm|.vgz> <out.wav> [seconds]\n");
        return 2;
    }
    const std::string in = argv[1], out = argv[2];
    const int seconds = (argc >= 4) ? atoi(argv[3]) : 15;
    const int rate = 49716;   // the rate the app actually runs at

    std::vector<uint8_t> data;
    if (!readFile(in, data)) { printf("cannot read %s\n", in.c_str()); return 1; }

    VgmBackend p;
    p.init(rate);
    if (!p.load(data.data(), data.size())) { printf("load failed: %s\n", in.c_str()); return 1; }
    p.setMaxLoops(2);

    const VgmBackend::Check c = p.verify();

    p.play();
    const auto t0 = std::chrono::steady_clock::now();
    const int frames = rate * seconds;
    std::vector<float> mix(size_t(frames) * 2, 0.0f);
    const int block = 1024;
    // What the channel monitor would be shown, sampled as often as the GUI
    // polls it. A bar that never rises is a level function, not a mixer, and
    // the two are easy to confuse by ear.
    std::vector<int> voicePeak(size_t(p.voiceCount() > 0 ? p.voiceCount() : 1), 0);
    for (int done = 0; done < frames; done += block) {
        const int n = (frames - done < block) ? (frames - done) : block;
        p.render(&mix[size_t(done) * 2], n);
        for (int v = 0; v < p.voiceCount() && v < int(voicePeak.size()); ++v) {
            const int lv = p.voiceLevel(v);
            if (lv > voicePeak[size_t(v)]) voicePeak[size_t(v)] = lv;
        }
    }

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("   render %.2fs of audio in %.2fs wall = %.1fx realtime\n", double(seconds), secs, double(seconds)/secs);

    std::vector<int16_t> pcm(mix.size());
    double sum = 0.0; int peak = 0;
    for (size_t i = 0; i < mix.size(); ++i) {
        float s = mix[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = int16_t(s * 32767.0f);
        sum += double(pcm[i]) * pcm[i];
        if (abs(pcm[i]) > peak) peak = abs(pcm[i]);
    }
    if (!writeWav(out, pcm, rate)) { printf("cannot write %s\n", out.c_str()); return 1; }

    const double rms = pcm.empty() ? 0.0 : sqrt(sum / double(pcm.size()));
    printf("%-40s %-22s peak %.4f rms %.5f total=%ums  parse:%s samples %ld/%ld%s\n",
           in.substr(in.find_last_of("/\\") + 1).c_str(),
           p.chips().c_str(),
           peak / 32768.0, rms / 32768.0, p.getTotalMs(),
           c.reachedEnd ? "end" : "NO-END",
           c.countedSamples, c.declaredSamples,
           c.unknownCommands ? "  UNKNOWN-CMD" : (c.samplesMatch ? "" : "  MISMATCH"));
    if (c.unknownCommands) printf("   first unknown command 0x%02X\n", c.firstUnknown);
    // The GD3 tag, as UTF-8. Worth printing: it is the only way to see whether
    // it decoded at all, and a console on another codepage will mangle the
    // display without that meaning the decode was wrong.
    if (!p.title().empty() || !p.game().empty())
        printf("   gd3: \"%s\" / \"%s\"\n", p.title().c_str(), p.game().c_str());

    // Peak monitor level per voice, 0-127. These drive the bars, so a chip
    // whose bars barely move shows up here as small numbers.
    if (!voicePeak.empty()) {
        int hi = 0;
        printf("   monitor peaks:");
        for (size_t v = 0; v < voicePeak.size(); ++v) {
            printf(" %d", voicePeak[v]);
            if (voicePeak[v] > hi) hi = voicePeak[v];
        }
        printf("   (max %d of 127)\n", hi);
    }
    return 0;
}
