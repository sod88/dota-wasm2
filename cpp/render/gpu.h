// The slice of WebGPU the renderer uses, imported from the page (web/js/render3d.js implements each
// function on a GPUDevice). Objects are plain integer handles; 0 means "none".
//
// The bridge fixes a few things so the C++ side stays small:
//  - every pipeline reads two vertex buffers: slot 0 = mesh vertices (position, normal, uv: 32 bytes),
//    slot 1 = per-instance data (mat4, colour, extra: 96 bytes) at @location(0..8);
//  - TARGET_SCREEN passes draw into a 4x multisampled colour + depth target the page manages and
//    resolve into the canvas; TARGET_SHADOW passes are depth-only into a texture made here.
#pragma once

#define GPU_IMPORT(name) extern "C" __attribute__((import_module("gpu"), import_name(#name)))

enum { TARGET_SCREEN = 0, TARGET_SHADOW = 1 };
enum { BLEND_NONE = 0, BLEND_ALPHA = 1, BLEND_ADD = 2 };
enum { SAMPLER_LINEAR = 0, SAMPLER_COMPARE = 1 };
enum { FORMAT_R8 = 0, FORMAT_DEPTH32 = 1 };

GPU_IMPORT(shader) int gpu_shader(const char* wgsl, int len);
/** Entry points are "vs_<name>" / "fs_<name>"; an empty fs name means no fragment stage. */
GPU_IMPORT(pipeline) int gpu_pipeline(int shader, const char* vs, int vs_len, const char* fs, int fs_len, int target, int blend, int depth_write, int depth_bias);

GPU_IMPORT(buffer) int gpu_buffer(int bytes, int is_uniform);
GPU_IMPORT(write_buffer) void gpu_write_buffer(int buffer, int offset, const void* data, int bytes);

GPU_IMPORT(texture) int gpu_texture(int w, int h, int format);
GPU_IMPORT(write_texture) void gpu_write_texture(int texture, const void* data, int w, int h, int bytes_per_row);
/** Make a texture from one of the page's canvases (sRGB, with mipmaps if asked). Frees `old`. */
GPU_IMPORT(canvas_texture) int gpu_canvas_texture(int old, int slot, int mipmaps);
/** Copy the canvas again into a texture made by gpu_canvas_texture (same size). */
GPU_IMPORT(update_canvas_texture) void gpu_update_canvas_texture(int texture, int slot);

GPU_IMPORT(group_begin) void gpu_group_begin();
GPU_IMPORT(group_buffer) void gpu_group_buffer(int binding, int buffer);
GPU_IMPORT(group_texture) void gpu_group_texture(int binding, int texture);
GPU_IMPORT(group_sampler) void gpu_group_sampler(int binding, int kind);
GPU_IMPORT(group_end) int gpu_group_end(int pipeline);

GPU_IMPORT(begin_pass) void gpu_begin_pass(int target, int depth_texture, float r, float g, float b);
GPU_IMPORT(set_pipeline) void gpu_set_pipeline(int pipeline);
GPU_IMPORT(set_group) void gpu_set_group(int group);
GPU_IMPORT(set_vertex_buffer) void gpu_set_vertex_buffer(int slot, int buffer);
GPU_IMPORT(draw) void gpu_draw(int vertices, int instances, int first_vertex, int first_instance);
GPU_IMPORT(end_pass) void gpu_end_pass();
GPU_IMPORT(submit) void gpu_submit();
