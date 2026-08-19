#pragma once

#include <array>
#include <string>
#include <vector>

#include "song.h"

namespace jmpconv {

// Looks up OPL 28-byte FM patch parameters by instrument name.
// Returns true if a match was found and sets instrument.flag = 1 & params.
bool fillOplParameters(Instrument& inst);

// Fills all instrument slots in a vector with matching or default OPL FM parameters.
void enrichInstruments(std::vector<Instrument>& instruments);

}  // namespace jmpconv
