// Renders the OPM half of an MLD `.mdz` to a WAV, so the FM engine can be
// listened to and measured on its own.
//
// There is no reference render for this format - `mld.x` is a resident X68000
// driver, so run68 cannot run it and nothing plays these the way mxwav plays an
// MDX. That is exactly why this exists: it makes the engine's output something
// to hear and compare between builds rather than something to reason about.
//
//   mldrender <song.mdz> <out.wav> [seconds]
//
// A `.pdx` / `.pdz` named by the song is looked for beside it.
#include "../mldfm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize(size_t(n));
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(got);
    return got > 0;
}

std::string folderOf(const std::string& p)
{
    size_t s = p.find_last_of("/\\");
    return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
}

// The sample-bank name sits after the title, NUL terminated.
std::string pdxNameOf(const std::vector<uint8_t>& d)
{
    size_t i = 0;
    while (i + 2 < d.size()) {
        if (d[i] == 0x0D && d[i + 1] == 0x0A && d[i + 2] == 0x1A) { i += 3; break; }
        if (d[i] == 0x1A) { i += 1; break; }
        ++i;
    }
    std::string n;
    while (i < d.size() && d[i] != 0x00) n += char(d[i++]);
    return n;
}

void writeLE(std::vector<uint8_t>& b, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i) b.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}

bool writeWav(const std::string& path, const std::vector<int16_t>& pcm, int rate)
{
    const uint32_t bytes = uint32_t(pcm.size() * 2);
    std::vector<uint8_t> h;
    h.insert(h.end(), {'R','I','F','F'});
    writeLE(h, 36 + bytes, 4);
    h.insert(h.end(), {'W','A','V','E','f','m','t',' '});
    writeLE(h, 16, 4); writeLE(h, 1, 2); writeLE(h, 2, 2);
    writeLE(h, uint32_t(rate), 4); writeLE(h, uint32_t(rate * 4), 4);
    writeLE(h, 4, 2); writeLE(h, 16, 2);
    h.insert(h.end(), {'d','a','t','a'});
    writeLE(h, bytes, 4);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(h.data(), 1, h.size(), f);
    fwrite(pcm.data(), 1, bytes, f);
    fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: mldrender <song.mdz> <out.wav> [seconds]\n"); return 2; }
    const std::string in = argv[1], out = argv[2];
    const int seconds = (argc >= 4) ? atoi(argv[3]) : 20;
    const int rate = 44100;

    std::vector<uint8_t> song;
    if (!readFile(in, song)) { printf("cannot read %s\n", in.c_str()); return 1; }

    std::vector<uint8_t> pdx;
    std::string pdxName = pdxNameOf(song);
    if (!pdxName.empty()) {
        const std::string dir = folderOf(in) + "/";
        std::vector<std::string> tries;
        tries.push_back(pdxName);
        if (pdxName.find('.') == std::string::npos) {
            tries.push_back(pdxName + ".pdx");
            tries.push_back(pdxName + ".pdz");
        } else {
            // The name in the file is not always the name on disk: this song
            // asks for "ken.pdz" and what was recovered is "KEN.PDX". Both
            // extensions hold the same ADPCM, so try the other one.
            const std::string stem = pdxName.substr(0, pdxName.find_last_of('.'));
            tries.push_back(stem + ".pdx");
            tries.push_back(stem + ".pdz");
        }
        const size_t n0 = tries.size();
        for (size_t i = 0; i < n0; ++i) {
            std::string up = tries[i], lo = tries[i];
            for (char& c : up) c = char(toupper((unsigned char)c));
            for (char& c : lo) c = char(tolower((unsigned char)c));
            tries.push_back(up); tries.push_back(lo);
        }
        for (const std::string& t : tries)
            if (readFile(dir + t, pdx)) { pdxName = t; break; }
    }

    MldFm fm;
    fm.init(rate);
    if (!fm.load(song.data(), song.size(),
                 pdx.empty() ? nullptr : pdx.data(), pdx.size())) {
        printf("no OPM tracks in %s\n", in.c_str());
        return 1;
    }
    printf("  OPM/PCM tracks: %d   pdx: %s\n",
           fm.trackCount(), pdx.empty() ? "(none)" : pdxName.c_str());

    fm.play();
    const int frames = rate * seconds;
    std::vector<int16_t> pcm(size_t(frames) * 2, 0);
    const int block = 1024;
    long stoppedAt = -1;
    for (int done = 0; done < frames; done += block) {
        const int n = (frames - done < block) ? (frames - done) : block;
        fm.render16(&pcm[size_t(done) * 2], n);
        if (stoppedAt < 0 && !fm.isPlaying()) stoppedAt = done;
    }
    if (!writeWav(out, pcm, rate)) { printf("cannot write %s\n", out.c_str()); return 1; }

    double sum = 0.0; int peak = 0;
    for (int16_t v : pcm) { sum += double(v) * v; if (abs(v) > peak) peak = abs(v); }
    const double rms = pcm.empty() ? 0.0 : sqrt(sum / double(pcm.size()));
    printf("  %s  peak %.4f  rms %.5f  keyons %ld  %s\n",
           out.c_str(), peak / 32768.0, rms / 32768.0, fm.keyOnCount(),
           stoppedAt >= 0 ? "engine stopped" : "still playing");
    return 0;
}
