#pragma once

#include <vector>

namespace argus {

// Decode an in-memory image (JPEG/PNG) to RGBA8. Returns false on failure.
bool decodeImageRGBA(const unsigned char* data, int len, std::vector<unsigned char>& rgba,
                     int& w, int& h);

}  // namespace argus
