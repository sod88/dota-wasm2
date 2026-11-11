// The renderer's `gpu_*` functions (cpp/render/gpu.h) on a native WebGPU library (wgpu-native or
// Dawn: anything with the standard webgpu.h). This is the native twin of gpuBridge() in
// web/js/render3d.js and makes the same fixed choices: two vertex buffer layouts, a 4x multisampled
// screen target with its own depth buffer, depth-only shadow passes.
//
// The page's canvases become RGBA pixel buffers the app hands over with gpu_native_set_canvas().
#include "gpu_native.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../render/gpu.h"

namespace {

constexpr uint32_t SAMPLES = 4;

struct Obj {
    WGPUShaderModule shader = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPUBuffer buffer = nullptr;
    WGPUTexture texture = nullptr;
    WGPUBindGroup group = nullptr;
    uint64_t bytes = 0;  // buffer size
};

struct Canvas {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
};

struct State {
    WGPUInstance instance = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUSurface surface = nullptr;
    WGPUTextureFormat format = WGPUTextureFormat_BGRA8Unorm;
    int width = 0, height = 0;  // surface size in pixels

    std::vector<Obj> objs = std::vector<Obj>(1);  // handle 0 = none
    Canvas canvases[4];
    WGPUSampler samplers[2] = {};

    // multisampled screen targets, remade when the surface resizes
    WGPUTexture msaa_color = nullptr, msaa_depth = nullptr;
    int msaa_w = 0, msaa_h = 0;

    WGPUCommandEncoder encoder = nullptr;
    WGPURenderPassEncoder pass = nullptr;
    WGPUTexture frame = nullptr;  // this frame's surface texture, once a screen pass asked for it
    std::vector<WGPUBindGroupEntry> entries;
    std::vector<WGPUTextureView> views;  // bind group views, released with the bind groups' textures

    std::string capture_path;  // save the next presented frame here (gpu_native_capture)
    bool captured = false;
} S;

WGPUStringView sv(const char* p, size_t n) { return WGPUStringView{p, n}; }
WGPUStringView sv(const char* p) { return WGPUStringView{p, WGPU_STRLEN}; }

int add(const Obj& o) {
    S.objs.push_back(o);
    return (int)S.objs.size() - 1;
}

Obj& at(int h) {
    static Obj none;
    return h > 0 && h < (int)S.objs.size() ? S.objs[h] : none;
}

/** Write a canvas into `tex`, mip level by mip level (each level a 2x2 box filter of the previous). */
void upload_canvas(WGPUTexture tex, const Canvas& c, uint32_t mips) {
    std::vector<uint8_t> level = c.rgba, next;
    int w = c.w, h = c.h;
    for (uint32_t m = 0; m < mips; m++) {
        if (m) {
            int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
            next.assign((size_t)nw * nh * 4, 0);
            for (int y = 0; y < nh; y++)
                for (int x = 0; x < nw; x++)
                    for (int k = 0; k < 4; k++) {
                        auto px = [&](int sx, int sy) {
                            return (int)level[((size_t)std::min(sy, h - 1) * w + std::min(sx, w - 1)) * 4 + k];
                        };
                        int sum = px(2 * x, 2 * y) + px(2 * x + 1, 2 * y) + px(2 * x, 2 * y + 1) + px(2 * x + 1, 2 * y + 1);
                        next[((size_t)y * nw + x) * 4 + k] = (uint8_t)((sum + 2) / 4);
                    }
            level.swap(next);
            w = nw;
            h = nh;
        }
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = tex;
        dst.mipLevel = m;
        WGPUTexelCopyBufferLayout layout = {0, (uint32_t)w * 4, (uint32_t)h};
        WGPUExtent3D size = {(uint32_t)w, (uint32_t)h, 1};
        wgpuQueueWriteTexture(S.queue, &dst, level.data(), level.size(), &layout, &size);
    }
}

WGPUTexture make_texture(int w, int h, WGPUTextureFormat fmt, WGPUTextureUsage usage, uint32_t samples = 1, uint32_t mips = 1) {
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    d.size = {(uint32_t)w, (uint32_t)h, 1};
    d.format = fmt;
    d.usage = usage;
    d.sampleCount = samples;
    d.mipLevelCount = mips;
    return wgpuDeviceCreateTexture(S.device, &d);
}

void free_texture(WGPUTexture& t) {
    if (!t) return;
    wgpuTextureDestroy(t);
    wgpuTextureRelease(t);
    t = nullptr;
}

/** Copy the frame into a buffer before it is presented and write it out as a 24-bit BMP. */
void capture_frame() {
    const uint32_t w = (uint32_t)S.width, h = (uint32_t)S.height, row = (w * 4 + 255) & ~255u;
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = (uint64_t)row * h;
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(S.device, &bd);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(S.device, nullptr);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = S.frame;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buf;
    dst.layout.bytesPerRow = row;
    dst.layout.rowsPerImage = h;
    WGPUExtent3D size = {w, h, 1};
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &size);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(S.queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);

    bool done = false;
    WGPUBufferMapCallbackInfo mc = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mc.mode = WGPUCallbackMode_AllowProcessEvents;
    mc.callback = [](WGPUMapAsyncStatus, WGPUStringView, void* ud, void*) { *(bool*)ud = true; };
    mc.userdata1 = &done;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, (size_t)bd.size, mc);
    while (!done) wgpuInstanceProcessEvents(S.instance);
    const uint8_t* px = (const uint8_t*)wgpuBufferGetConstMappedRange(buf, 0, (size_t)bd.size);

    const bool bgra = S.format == WGPUTextureFormat_BGRA8Unorm || S.format == WGPUTextureFormat_BGRA8UnormSrgb;
    const uint32_t out_row = (w * 3 + 3) & ~3u, img = out_row * h;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) hdr[at + i] = (uint8_t)(v >> (8 * i)); };
    put32(2, 54 + img); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    hdr[26] = 1; hdr[28] = 24; put32(34, img);
    if (FILE* f = std::fopen(S.capture_path.c_str(), "wb")) {
        std::fwrite(hdr, 1, 54, f);
        std::vector<uint8_t> line(out_row, 0);
        for (uint32_t y = h; y-- > 0;) {  // BMP rows go bottom-up, pixels as B, G, R
            const uint8_t* s = px + (size_t)y * row;
            for (uint32_t x = 0; x < w; x++) {
                line[x * 3] = bgra ? s[x * 4] : s[x * 4 + 2];
                line[x * 3 + 1] = s[x * 4 + 1];
                line[x * 3 + 2] = bgra ? s[x * 4 + 2] : s[x * 4];
            }
            std::fwrite(line.data(), 1, out_row, f);
        }
        std::fclose(f);
    }
    wgpuBufferUnmap(buf);
    wgpuBufferRelease(buf);
    S.capture_path.clear();
    S.captured = true;
}

}  // namespace

// ------------------------------------------------------------------------------------- app side

void gpu_native_init(WGPUInstance instance, WGPUDevice device, WGPUSurface surface, WGPUTextureFormat format) {
    S.instance = instance;
    S.device = device;
    S.queue = wgpuDeviceGetQueue(device);
    S.surface = surface;
    S.format = format;

    WGPUSamplerDescriptor lin = WGPU_SAMPLER_DESCRIPTOR_INIT;
    lin.magFilter = WGPUFilterMode_Linear;
    lin.minFilter = WGPUFilterMode_Linear;
    lin.mipmapFilter = WGPUMipmapFilterMode_Linear;
    lin.maxAnisotropy = 8;
    S.samplers[SAMPLER_LINEAR] = wgpuDeviceCreateSampler(device, &lin);
    WGPUSamplerDescriptor cmp = WGPU_SAMPLER_DESCRIPTOR_INIT;
    cmp.magFilter = WGPUFilterMode_Linear;
    cmp.minFilter = WGPUFilterMode_Linear;
    cmp.compare = WGPUCompareFunction_LessEqual;
    S.samplers[SAMPLER_COMPARE] = wgpuDeviceCreateSampler(device, &cmp);
}

void gpu_native_resize(int width, int height) {
    if (width <= 0 || height <= 0 || (width == S.width && height == S.height)) return;
    S.width = width;
    S.height = height;
    WGPUSurfaceConfiguration c = WGPU_SURFACE_CONFIGURATION_INIT;
    c.device = S.device;
    c.format = S.format;
    c.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;  // CopySrc: gpu_native_capture
    c.width = (uint32_t)width;
    c.height = (uint32_t)height;
    c.alphaMode = WGPUCompositeAlphaMode_Auto;
    c.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(S.surface, &c);
}

void gpu_native_capture(const char* path) { S.capture_path = path; }
bool gpu_native_captured() { return S.captured; }

void gpu_native_set_canvas(int slot, const uint8_t* rgba, int w, int h) {
    if (slot < 0 || slot >= 4) return;
    Canvas& c = S.canvases[slot];
    c.w = w;
    c.h = h;
    c.rgba.assign(rgba, rgba + (size_t)w * h * 4);
}

// ------------------------------------------------------------------------------- gpu.h imports

extern "C" {

int gpu_shader(const char* wgsl, int len) {
    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = sv(wgsl, (size_t)len);
    WGPUShaderModuleDescriptor d = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    d.nextInChain = &src.chain;
    d.label = sv("render3d");
    Obj o;
    o.shader = wgpuDeviceCreateShaderModule(S.device, &d);
    return o.shader ? add(o) : 0;
}

int gpu_pipeline(int shader, const char* vs, int vs_len, const char* fs, int fs_len, int target, int blend, int depth_write, int depth_bias) {
    const bool shadow = target == TARGET_SHADOW;
    static const WGPUVertexAttribute mesh_attrs[] = {
        {nullptr, WGPUVertexFormat_Float32x3, 0, 0},
        {nullptr, WGPUVertexFormat_Float32x3, 12, 1},
        {nullptr, WGPUVertexFormat_Float32x2, 24, 2},
    };
    static const WGPUVertexAttribute inst_attrs[] = {
        {nullptr, WGPUVertexFormat_Float32x4, 0, 3},  {nullptr, WGPUVertexFormat_Float32x4, 16, 4},
        {nullptr, WGPUVertexFormat_Float32x4, 32, 5}, {nullptr, WGPUVertexFormat_Float32x4, 48, 6},
        {nullptr, WGPUVertexFormat_Float32x4, 64, 7}, {nullptr, WGPUVertexFormat_Float32x4, 80, 8},
    };
    const WGPUVertexBufferLayout buffers[] = {
        {nullptr, WGPUVertexStepMode_Vertex, 32, 3, mesh_attrs},
        {nullptr, WGPUVertexStepMode_Instance, 96, 6, inst_attrs},
    };

    WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    d.vertex.module = at(shader).shader;
    d.vertex.entryPoint = sv(vs, (size_t)vs_len);
    d.vertex.bufferCount = 2;
    d.vertex.buffers = buffers;
    d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    d.primitive.cullMode = WGPUCullMode_None;

    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    ds.format = shadow ? WGPUTextureFormat_Depth32Float : WGPUTextureFormat_Depth24Plus;
    ds.depthWriteEnabled = depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    ds.depthCompare = WGPUCompareFunction_Less;
    if (depth_bias) {
        ds.depthBias = 2;
        ds.depthBiasSlopeScale = 2.5f;
    }
    d.depthStencil = &ds;
    d.multisample.count = shadow ? 1 : SAMPLES;

    WGPUBlendState blends[3] = {};
    blends[BLEND_ALPHA].color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha};
    blends[BLEND_ALPHA].alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha};
    blends[BLEND_ADD].color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_One};
    blends[BLEND_ADD].alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One};
    WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
    ct.format = S.format;
    ct.blend = blend == BLEND_NONE ? nullptr : &blends[blend];
    WGPUFragmentState fsd = WGPU_FRAGMENT_STATE_INIT;
    fsd.module = at(shader).shader;
    fsd.entryPoint = sv(fs, (size_t)fs_len);
    fsd.targetCount = 1;
    fsd.targets = &ct;
    if (fs_len > 0) d.fragment = &fsd;

    Obj o;
    o.pipeline = wgpuDeviceCreateRenderPipeline(S.device, &d);
    return o.pipeline ? add(o) : 0;
}

int gpu_buffer(int bytes, int is_uniform) {
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.size = (uint64_t)std::max(16, (bytes + 15) & ~15);
    d.usage = (is_uniform ? WGPUBufferUsage_Uniform : WGPUBufferUsage_Vertex) | WGPUBufferUsage_CopyDst;
    Obj o;
    o.buffer = wgpuDeviceCreateBuffer(S.device, &d);
    o.bytes = d.size;
    return add(o);
}

void gpu_write_buffer(int buffer, int offset, const void* data, int bytes) {
    // WebGPU wants writes in whole 4-byte words
    size_t n = ((size_t)bytes + 3) & ~(size_t)3;
    if (n == (size_t)bytes) {
        wgpuQueueWriteBuffer(S.queue, at(buffer).buffer, (uint64_t)offset, data, n);
        return;
    }
    std::vector<uint8_t> tmp(n, 0);
    std::memcpy(tmp.data(), data, (size_t)bytes);
    wgpuQueueWriteBuffer(S.queue, at(buffer).buffer, (uint64_t)offset, tmp.data(), n);
}

int gpu_texture(int w, int h, int format) {
    Obj o;
    if (format == FORMAT_DEPTH32)
        o.texture = make_texture(w, h, WGPUTextureFormat_Depth32Float, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment);
    else
        o.texture = make_texture(w, h, WGPUTextureFormat_R8Unorm, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
    return add(o);
}

void gpu_write_texture(int texture, const void* data, int w, int h, int bytes_per_row) {
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = at(texture).texture;
    WGPUTexelCopyBufferLayout layout = {0, (uint32_t)bytes_per_row, (uint32_t)h};
    WGPUExtent3D size = {(uint32_t)w, (uint32_t)h, 1};
    wgpuQueueWriteTexture(S.queue, &dst, data, (size_t)bytes_per_row * h, &layout, &size);
}

int gpu_canvas_texture(int old, int slot, int mipmaps) {
    const Canvas& c = S.canvases[slot & 3];
    if (c.w <= 0 || c.h <= 0) {
        std::fprintf(stderr, "render3d: canvas %d was never set\n", slot);
        return 0;
    }
    uint32_t mips = 1;
    if (mipmaps)
        for (int s = std::max(c.w, c.h); s > 1; s >>= 1) mips++;
    WGPUTexture tex = make_texture(c.w, c.h, WGPUTextureFormat_RGBA8UnormSrgb,
                                   WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, 1, mips);
    upload_canvas(tex, c, mips);
    if (old > 0 && old < (int)S.objs.size()) {
        free_texture(S.objs[old].texture);
        S.objs[old].texture = tex;
        return old;
    }
    Obj o;
    o.texture = tex;
    return add(o);
}

void gpu_update_canvas_texture(int texture, int slot) { upload_canvas(at(texture).texture, S.canvases[slot & 3], 1); }

void gpu_group_begin() { S.entries.clear(); }

void gpu_group_buffer(int binding, int buffer) {
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = (uint32_t)binding;
    e.buffer = at(buffer).buffer;
    e.size = at(buffer).bytes;
    S.entries.push_back(e);
}

void gpu_group_texture(int binding, int texture) {
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = (uint32_t)binding;
    e.textureView = wgpuTextureCreateView(at(texture).texture, nullptr);
    S.views.push_back(e.textureView);
    S.entries.push_back(e);
}

void gpu_group_sampler(int binding, int kind) {
    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
    e.binding = (uint32_t)binding;
    e.sampler = S.samplers[kind & 1];
    S.entries.push_back(e);
}

int gpu_group_end(int pipeline) {
    WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(at(pipeline).pipeline, 0);
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = layout;
    d.entryCount = S.entries.size();
    d.entries = S.entries.data();
    Obj o;
    o.group = wgpuDeviceCreateBindGroup(S.device, &d);
    wgpuBindGroupLayoutRelease(layout);
    return add(o);
}

void gpu_begin_pass(int target, int depth_texture, float r, float g, float b) {
    if (!S.encoder) S.encoder = wgpuDeviceCreateCommandEncoder(S.device, nullptr);
    WGPURenderPassDescriptor d = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.depthLoadOp = WGPULoadOp_Clear;
    depth.depthClearValue = 1.0f;
    d.depthStencilAttachment = &depth;

    if (target == TARGET_SHADOW) {
        WGPUTextureView v = wgpuTextureCreateView(at(depth_texture).texture, nullptr);
        depth.view = v;
        depth.depthStoreOp = WGPUStoreOp_Store;
        S.pass = wgpuCommandEncoderBeginRenderPass(S.encoder, &d);
        wgpuTextureViewRelease(v);
        return;
    }

    if (!S.msaa_color || S.msaa_w != S.width || S.msaa_h != S.height) {
        free_texture(S.msaa_color);
        free_texture(S.msaa_depth);
        S.msaa_color = make_texture(S.width, S.height, S.format, WGPUTextureUsage_RenderAttachment, SAMPLES);
        S.msaa_depth = make_texture(S.width, S.height, WGPUTextureFormat_Depth24Plus, WGPUTextureUsage_RenderAttachment, SAMPLES);
        S.msaa_w = S.width;
        S.msaa_h = S.height;
    }
    if (!S.frame) {
        WGPUSurfaceTexture st = {};
        wgpuSurfaceGetCurrentTexture(S.surface, &st);
        if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            if (st.texture) wgpuTextureRelease(st.texture);
            S.pass = nullptr;  // skip drawing this frame (window minimised, surface being resized...)
            return;
        }
        S.frame = st.texture;
    }
    WGPUTextureView color_v = wgpuTextureCreateView(S.msaa_color, nullptr);
    WGPUTextureView resolve_v = wgpuTextureCreateView(S.frame, nullptr);
    WGPUTextureView depth_v = wgpuTextureCreateView(S.msaa_depth, nullptr);
    WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ca.view = color_v;
    ca.resolveTarget = resolve_v;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Discard;
    ca.clearValue = {r, g, b, 1.0};
    d.colorAttachmentCount = 1;
    d.colorAttachments = &ca;
    depth.view = depth_v;
    depth.depthStoreOp = WGPUStoreOp_Discard;
    S.pass = wgpuCommandEncoderBeginRenderPass(S.encoder, &d);
    wgpuTextureViewRelease(color_v);
    wgpuTextureViewRelease(resolve_v);
    wgpuTextureViewRelease(depth_v);
}

// While a frame is skipped there is no pass; the draw calls below then do nothing.
void gpu_set_pipeline(int pipeline) {
    if (S.pass) wgpuRenderPassEncoderSetPipeline(S.pass, at(pipeline).pipeline);
}
void gpu_set_group(int group) {
    if (S.pass) wgpuRenderPassEncoderSetBindGroup(S.pass, 0, at(group).group, 0, nullptr);
}
void gpu_set_vertex_buffer(int slot, int buffer) {
    if (S.pass) wgpuRenderPassEncoderSetVertexBuffer(S.pass, (uint32_t)slot, at(buffer).buffer, 0, WGPU_WHOLE_SIZE);
}
void gpu_draw(int vertices, int instances, int first_vertex, int first_instance) {
    if (S.pass && instances > 0)
        wgpuRenderPassEncoderDraw(S.pass, (uint32_t)vertices, (uint32_t)instances, (uint32_t)first_vertex, (uint32_t)first_instance);
}
void gpu_end_pass() {
    if (!S.pass) return;
    wgpuRenderPassEncoderEnd(S.pass);
    wgpuRenderPassEncoderRelease(S.pass);
    S.pass = nullptr;
}

void gpu_submit() {
    if (S.encoder) {
        WGPUCommandBuffer cb = wgpuCommandEncoderFinish(S.encoder, nullptr);
        wgpuQueueSubmit(S.queue, 1, &cb);
        wgpuCommandBufferRelease(cb);
        wgpuCommandEncoderRelease(S.encoder);
        S.encoder = nullptr;
    }
    if (S.frame) {
        if (!S.capture_path.empty()) capture_frame();
        wgpuSurfacePresent(S.surface);
        wgpuTextureRelease(S.frame);
        S.frame = nullptr;
    }
}

}  // extern "C"
