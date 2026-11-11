// Native backend for the 3D renderer's gpu.h functions (see gpu_native.cpp).
#pragma once
#include <cstdint>

#include <webgpu.h>

/** Hand the bridge a ready device and a surface to draw into (not yet configured). */
void gpu_native_init(WGPUInstance instance, WGPUDevice device, WGPUSurface surface, WGPUTextureFormat format);
/** (Re)configure the surface; call before the first frame and whenever the window's pixel size changes. */
void gpu_native_resize(int width, int height);
/** Save the next frame to `path` as a BMP; gpu_native_captured() turns true once it is written. */
void gpu_native_capture(const char* path);
bool gpu_native_captured();
/** The pixels behind canvas `slot` (0 ground, 1 ground markings, 2 hero emblems), RGBA8 sRGB, row-major. */
void gpu_native_set_canvas(int slot, const uint8_t* rgba, int w, int h);
