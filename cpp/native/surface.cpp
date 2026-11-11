// The WebGPU surface for an SDL window: the one place that touches the window system (Cocoa/Metal,
// X11, Wayland, Win32). It is its own file because <X11/Xlib.h> defines macros such as None, Bool
// and Status, which break any engine header included after it.
#include <cstdio>

#include <webgpu.h>

#include <SDL.h>
#include <SDL_syswm.h>
#if defined(__APPLE__)
#include <SDL_metal.h>
#endif

WGPUSurface make_surface(WGPUInstance instance, SDL_Window* window) {
    WGPUSurfaceDescriptor d = WGPU_SURFACE_DESCRIPTOR_INIT;
#if defined(__APPLE__)
    SDL_MetalView view = SDL_Metal_CreateView(window);
    WGPUSurfaceSourceMetalLayer src = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
    src.layer = SDL_Metal_GetLayer(view);
    d.nextInChain = &src.chain;
    return wgpuInstanceCreateSurface(instance, &d);
#else
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window, &wm)) return nullptr;
#if defined(_WIN32)
    WGPUSurfaceSourceWindowsHWND src = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
    src.hinstance = wm.info.win.hinstance;
    src.hwnd = wm.info.win.window;
    d.nextInChain = &src.chain;
    return wgpuInstanceCreateSurface(instance, &d);
#else
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    if (wm.subsystem == SDL_SYSWM_WAYLAND) {
        WGPUSurfaceSourceWaylandSurface src = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
        src.display = wm.info.wl.display;
        src.surface = wm.info.wl.surface;
        d.nextInChain = &src.chain;
        return wgpuInstanceCreateSurface(instance, &d);
    }
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
    if (wm.subsystem == SDL_SYSWM_X11) {
        WGPUSurfaceSourceXlibWindow src = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
        src.display = wm.info.x11.display;
        src.window = (uint64_t)wm.info.x11.window;
        d.nextInChain = &src.chain;
        return wgpuInstanceCreateSurface(instance, &d);
    }
#endif
    std::fprintf(stderr, "this SDL video driver (%s) is not supported; try SDL_VIDEODRIVER=x11\n", SDL_GetCurrentVideoDriver());
    return nullptr;
#endif
#endif
}
