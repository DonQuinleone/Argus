// PNG export of the rendered spectrogram raster. Uses the bundled stb_image_write
// (header-only); this is the single translation unit that pulls in its implementation.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "Exports.h"

namespace argus {

bool exportSpectrogramPng(const Report& rep, const std::string& path) {
    if (rep.specWidth <= 0 || rep.specHeight <= 0 || rep.specRGBA.empty()) return false;
    // RGBA8, row-major, top = high freq - exactly what the on-screen view uses.
    return stbi_write_png(path.c_str(), rep.specWidth, rep.specHeight, 4, rep.specRGBA.data(),
                          rep.specWidth * 4) != 0;
}

}  // namespace argus
