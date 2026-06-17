#pragma once

namespace argus {

// A simple OpenGL RGBA texture. upload()/destroy() must be called on the thread
// that owns the GL context (the UI thread).
struct Texture {
    unsigned int id = 0;
    int w = 0;
    int h = 0;

    void upload(const unsigned char* rgba, int width, int height);
    void destroy();
    bool valid() const { return id != 0; }
};

}  // namespace argus
