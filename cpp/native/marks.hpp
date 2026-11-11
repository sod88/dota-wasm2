// Ground markings for the native app: health bars, selection rings, ability ranges and targeting.
//
// The renderer lays one square picture on the ground under the camera (canvas slot 1). In the browser
// the 2D canvas code paints it; here this small software rasteriser does. Everything is given in world
// units and drawn anti-aliased into an RGBA picture that covers what the camera sees this frame.
#pragma once
#include <cstdint>
#include <vector>

/** sRGB colour with straight (not premultiplied) alpha, all 0..1. */
struct Rgba {
    float r, g, b, a;
    Rgba with_alpha(float alpha) const { return {r, g, b, alpha}; }
};

class Marks {
   public:
    static constexpr int PX = 1024;

    /** Start a frame: clear the picture and lay it over the world square centred on (cx, cz). */
    void begin(float cx, float cz, float side);

    void rect(float x, float y, float w, float h, Rgba c);  // (x, y) is the corner with the smallest coordinates
    void disc(float x, float y, float radius, Rgba c);
    void ring(float x, float y, float radius, float width, Rgba c);
    void line(float x0, float y0, float x1, float y1, float width, Rgba c);

    const uint8_t* pixels() const { return px.data(); }

   private:
    std::vector<uint8_t> px = std::vector<uint8_t>((size_t)PX * PX * 4, 0);
    float ox = 0, oz = 0, s = 1;  // world (ox, oz) is pixel (0, 0); s pixels per world unit

    void blend(int x, int y, Rgba c, float coverage);
};
