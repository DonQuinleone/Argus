// Embedded-artwork decoding via stb_image. This is the single TU that pulls in the
// implementation; we only need JPEG/PNG (the formats used for cover art).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

#include "ImageDecode.h"

namespace argus {

bool decodeImageRGBA(const unsigned char* data, int len, std::vector<unsigned char>& rgba,
                     int& w, int& h) {
    int comp = 0;
    unsigned char* px = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (!px) return false;
    rgba.assign(px, px + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

}  // namespace argus
