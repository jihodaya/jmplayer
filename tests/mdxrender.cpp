// Renders an MDX to a WAV with our own engine, so it can be put beside the
// reference the real driver produced.
//
// This is the MDX counterpart of rcpdump: MDX was reported as "almost
// unintelligible noise", and that is not a thing to chase by ear when
// mxwav.exe - the one part of MXDRVg that is explicitly free to use - can
// render the same songs through the actual X68000 driver. Render both, compare.
//
//   mdxrender <file.mdx> <out.wav> [seconds]
//
// The .PDX, if the song names one, is looked for beside it.
#include "../mdxcore/mxdrv.h"

#include <cstdio>
#include <cmath>
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
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize(size_t(n));
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(got);
    return got > 0;
}

std::string folderOf(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
}

// The PDX name the song asks for sits after the title, NUL terminated.
std::string pdxNameOf(const std::vector<uint8_t>& mdx)
{
    size_t i = 0;
    while (i + 2 < mdx.size()) {
        if (mdx[i] == 0x0D && mdx[i + 1] == 0x0A && mdx[i + 2] == 0x1A) { i += 3; break; }
        if (mdx[i] == 0x1A) { i += 1; break; }
        ++i;
    }
    std::string name;
    while (i < mdx.size() && mdx[i] != 0x00) name += char(mdx[i++]);
    return name;
}

void writeLE(std::vector<uint8_t>& b, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i) b.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}

bool writeWav(const std::string& path, const std::vector<int16_t>& pcm, int rate)
{
    const uint32_t dataBytes = uint32_t(pcm.size() * 2);
    std::vector<uint8_t> h;
    h.insert(h.end(), {'R','I','F','F'});
    writeLE(h, 36 + dataBytes, 4);
    h.insert(h.end(), {'W','A','V','E','f','m','t',' '});
    writeLE(h, 16, 4);
    writeLE(h, 1, 2);                       // PCM
    writeLE(h, 2, 2);                       // stereo
    writeLE(h, uint32_t(rate), 4);
    writeLE(h, uint32_t(rate * 4), 4);
    writeLE(h, 4, 2);
    writeLE(h, 16, 2);
    h.insert(h.end(), {'d','a','t','a'});
    writeLE(h, dataBytes, 4);

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(h.data(), 1, h.size(), f);
    fwrite(pcm.data(), 1, dataBytes, f);
    fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("usage: mdxrender <file.mdx> <out.wav> [seconds]\n");
        return 2;
    }
    const std::string in = argv[1];
    const std::string out = argv[2];
    const int seconds = (argc >= 4) ? atoi(argv[3]) : 30;
    const int rate = 44100;   // matches the mxwav reference renders

    std::vector<uint8_t> mdx;
    if (!readFile(in, mdx)) { printf("cannot read %s\n", in.c_str()); return 1; }

    std::vector<uint8_t> pdx;
    std::string pdxName = pdxNameOf(mdx);
    if (!pdxName.empty()) {
        // The name in the file is not always the name on disk. Most of this
        // corpus - 126 of the songs - asks for "ssf2_x68" with no extension at
        // all, and the file beside them is "ssf2_x68.pdx"; others differ only
        // in case. Looking for the literal string found the sample data for
        // just 32 of 101 songs, so every drum track in the rest went missing.
        const std::string dir = folderOf(in) + "/";
        std::vector<std::string> tries;
        tries.push_back(pdxName);
        if (pdxName.find('.') == std::string::npos) {
            tries.push_back(pdxName + ".pdx");
            tries.push_back(pdxName + ".PDX");
        }
        const size_t n0 = tries.size();
        for (size_t i = 0; i < n0; ++i) {
            std::string up = tries[i], lo = tries[i];
            for (char& c : up) c = char(toupper((unsigned char)c));
            for (char& c : lo) c = char(tolower((unsigned char)c));
            tries.push_back(up);
            tries.push_back(lo);
        }
        for (const std::string& t2 : tries) {
            if (readFile(dir + t2, pdx)) { pdxName = t2; break; }
        }
    }

    Mxdrv drv;
    drv.init(rate);
    if (!drv.loadMdx(mdx.data(), mdx.size(), pdx.empty() ? nullptr : pdx.data(), pdx.size())) {
        printf("load failed\n");
        return 1;
    }
    { const char* e = getenv("MDX_TEMPO"); if (e) drv.setTempoScale(atoi(e)); }
    { const char* e = getenv("MDX_SEEK");  if (e) drv.seekMs(uint32_t(atoi(e))); }

    drv.play();

    const int frames = rate * seconds;
    std::vector<int16_t> pcm(size_t(frames) * 2, 0);

    const int block = 1024;
    long stoppedAt = -1;
    for (int done = 0; done < frames; done += block) {
        const int n = (frames - done < block) ? (frames - done) : block;
        drv.render16(&pcm[size_t(done) * 2], n);
        if (stoppedAt < 0 && !drv.isPlaying()) stoppedAt = done;
    }
    if (stoppedAt >= 0)
        printf("   engine stopped at %.2fs (total says %ums)\n", double(stoppedAt) / rate, drv.getTotalMs());
    else
        printf("   engine still playing after %ds (total says %ums)\n", seconds, drv.getTotalMs());

    if (!writeWav(out, pcm, rate)) { printf("cannot write %s\n", out.c_str()); return 1; }

    // key-on times beside the wav, one per line in seconds
    {
        const std::string kp = out + ".keyons";
        FILE* kf = fopen(kp.c_str(), "w");
        if (kf) {
            for (long v : drv.keyOnTimes())
                fprintf(kf, "%.6f %ld\n", double(v / 100) / rate, v % 100);
            fclose(kf);
        }
    }

    // A one-line verdict, so a batch run is readable without opening anything.
    double sum = 0.0; int peak = 0;
    for (int16_t v : pcm) { sum += double(v) * v; if (abs(v) > peak) peak = abs(v); }
    const double rms = (pcm.empty() ? 0.0 : sqrt(sum / double(pcm.size())));
    printf("%s  %ds  peak %.4f  rms %.5f  keyons %ld  pdx=%s\n",
           out.c_str(), seconds,
           peak / 32768.0, rms / 32768.0, drv.keyOnCount(),
           pdx.empty() ? "(none)" : pdxName.c_str());
    if (getenv("MDX_CMDHIST")) {
        extern long g_cmdHist[32];
        for (int i = 0; i < 32; ++i)
            if (g_cmdHist[i]) fprintf(stderr, "CMD %02X %ld\n", 0xE0 + i, g_cmdHist[i]);
    }
    return 0;
}
