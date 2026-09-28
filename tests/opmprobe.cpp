// Plays one voice on one channel and prints where its energy lands.
//
// SF2_GUIE renders with the second and fourth harmonics of its bass but no
// fundamental at all, 38 dB below the reference. Everything about the voice
// record checks out on paper, so this puts the registers into the chip by hand
// and looks at what comes out - the twenty-line probe rather than more reading.
//
//   opmprobe [alg] [tlmask]
#include "../mdxcore/ym2151.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv)
{
    // What frequency does the chip actually give for a key code?
    // One operator, MUL 1, ALG 7 (everything a carrier), no envelope decay -
    // then find the fundamental by counting zero crossings.
    const int rate = 44100;
    printf("  KC   oct note   measured Hz   MDX note that asks for it\n");
    for (int mdxnote = 12; mdxnote <= 72; mdxnote += 6) {
        const int pitch = mdxnote * 64 + 5;
        const int semi = (pitch * 4) >> 8;
        static const uint8_t field[12] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
        const uint8_t kc = uint8_t((semi / 12) * 16 + field[semi % 12]);

        Ym2151 opm;
        opm.init(4000000, rate);
        opm.writeReg(0x20, uint8_t((3 << 6) | 7));        // pan both, ALG 7
        for (int op = 0; op < 4; ++op) {
            const int o = op * 8;
            opm.writeReg(0x40 + o, uint8_t(op == 3 ? 1 : 0));   // MUL 1 on one, 0.5 elsewhere
            opm.writeReg(0x60 + o, uint8_t(op == 3 ? 0 : 127)); // only that one audible
            opm.writeReg(0x80 + o, 31);
            opm.writeReg(0xA0 + o, 0);
            opm.writeReg(0xC0 + o, 0);
            opm.writeReg(0xE0 + o, 0);
        }
        opm.writeReg(0x30, uint8_t((pitch * 4) & 0xFF));
        opm.writeReg(0x28, kc);
        opm.writeReg(0x08, 0x78);

        const int n = rate;
        std::vector<int32_t> l(n, 0), r(n, 0);
        opm.render(l.data(), r.data(), n);

        int crossings = 0;
        const int from = rate / 4;
        for (int i = from + 1; i < n; ++i)
            if (l[i - 1] <= 0 && l[i] > 0) ++crossings;
        const double hz = double(crossings) * rate / double(n - from);
        printf("  %02X   %2d  %2d      %8.1f      note %d\n",
               kc, kc >> 4, kc & 15, hz, mdxnote);
    }
    return 0;
}
