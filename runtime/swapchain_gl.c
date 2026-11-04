// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// OPENGL SWAPCHAINS: textures in the application's context, read back with a framebuffer and
// glReadPixels at release (DESIGN.md, "Frames: copied, not shared"). XR_KHR_opengl_enable has the
// application call the swapchain functions with its context current on the calling thread, which is
// what makes it possible to touch GL here at all; every binding this changes is put back.
//
// NO GL HEADERS: the handful of enums are spelt out, and the functions come from the binding's own
// getProcAddress (EGL), or else from whichever of EGL and GLX holds the context current on this thread
// (current_gpa) -- the same dispatch the application's calls go through, so they reach that context,
// whichever vendor's it is.
//
// A WINDOWS APPLICATION UNDER WINE (2026-10-21): an OpenGL program saw opengl_enable
// (wineopenxr passes it through), made an XrGraphicsBindingOpenGLWin32KHR -- an HDC and an HGLRC -- and
// wineopenxr, which converts only Vulkan and Direct3D bindings, handed it on unchanged: the runtime knew
// no such binding and said GRAPHICS_DEVICE_INVALID. Behind WGL, Wine's context IS a host EGL or GLX
// context, current on this same thread, and its texture names are the host's. So the Win32 binding is
// taken as "the context that is current", like the others; the handles in it are never used.
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// XR_MNDX_egl_enable's binding, mirrored: openxr_platform.h declares it only with EGL's headers.
typedef void (*(*oxs_egl_proc)(const char *))(void);
struct binding_egl_mndx {
    XrStructureType type;
    const void *next;
    oxs_egl_proc getProcAddress;
    void *display, *config, *context;
};

enum {
    GL_NO_ERROR_ = 0, GL_TEXTURE_2D_ = 0x0DE1, GL_TEXTURE_2D_ARRAY_ = 0x8C1A,
    GL_TEXTURE_BINDING_2D_ = 0x8069, GL_TEXTURE_BINDING_2D_ARRAY_ = 0x8C1D,
    GL_TEXTURE_MIN_FILTER_ = 0x2801, GL_TEXTURE_MAG_FILTER_ = 0x2800, GL_TEXTURE_MAX_LEVEL_ = 0x813D, GL_LINEAR_ = 0x2601,
    GL_RGBA_ = 0x1908, GL_UNSIGNED_BYTE_ = 0x1401, GL_FLOAT_ = 0x1406, GL_UNSIGNED_INT_ = 0x1405, GL_UNSIGNED_SHORT_ = 0x1403,
    GL_DEPTH_COMPONENT_ = 0x1902, GL_DEPTH_STENCIL_ = 0x84F9, GL_UNSIGNED_INT_24_8_ = 0x84FA,
    GL_FLOAT_32_UNSIGNED_INT_24_8_REV_ = 0x8DAD,
    GL_SRGB8_ALPHA8_ = 0x8C43, GL_RGBA8_ = 0x8058, GL_DEPTH_COMPONENT32F_ = 0x8CAC, GL_DEPTH24_STENCIL8_ = 0x88F0,
    GL_DEPTH_COMPONENT24_ = 0x81A6, GL_DEPTH_COMPONENT16_ = 0x81A5, GL_DEPTH32F_STENCIL8_ = 0x8CAD,
    GL_READ_FRAMEBUFFER_ = 0x8CA8, GL_READ_FRAMEBUFFER_BINDING_ = 0x8CAA, GL_COLOR_ATTACHMENT0_ = 0x8CE0,
    GL_PIXEL_PACK_BUFFER_ = 0x88EB, GL_PIXEL_PACK_BUFFER_BINDING_ = 0x88ED,
    GL_PACK_ALIGNMENT_ = 0x0D05, GL_PACK_ROW_LENGTH_ = 0x0D02, GL_FRAMEBUFFER_SRGB_ = 0x8DB9, GL_VERSION_ = 0x1F02,
};

typedef unsigned int GLenum_;
typedef unsigned int GLuint_;
typedef int GLint_;
typedef int GLsizei_;

struct gl {
    void (*(*gpa)(const char *))(void);
    bool es;
    GLuint_ fbo;
    GLenum_ (*GetError)(void);
    const unsigned char *(*GetString)(GLenum_);
    void (*GetIntegerv)(GLenum_, GLint_ *);
    void (*GenTextures)(GLsizei_, GLuint_ *);
    void (*DeleteTextures)(GLsizei_, const GLuint_ *);
    void (*BindTexture)(GLenum_, GLuint_);
    void (*TexParameteri)(GLenum_, GLenum_, GLint_);
    void (*TexStorage2D)(GLenum_, GLsizei_, GLenum_, GLsizei_, GLsizei_);
    void (*TexStorage3D)(GLenum_, GLsizei_, GLenum_, GLsizei_, GLsizei_, GLsizei_);
    void (*TexImage2D)(GLenum_, GLint_, GLint_, GLsizei_, GLsizei_, GLint_, GLenum_, GLenum_, const void *);
    void (*TexImage3D)(GLenum_, GLint_, GLint_, GLsizei_, GLsizei_, GLsizei_, GLint_, GLenum_, GLenum_, const void *);
    void (*GenFramebuffers)(GLsizei_, GLuint_ *);
    void (*BindFramebuffer)(GLenum_, GLuint_);
    void (*FramebufferTexture2D)(GLenum_, GLenum_, GLenum_, GLuint_, GLint_);
    void (*FramebufferTextureLayer)(GLenum_, GLenum_, GLuint_, GLint_, GLint_);
    void (*ReadBuffer)(GLenum_);
    void (*ReadPixels)(GLint_, GLint_, GLsizei_, GLsizei_, GLenum_, GLenum_, void *);
    void (*PixelStorei)(GLenum_, GLint_);
    void (*BindBuffer)(GLenum_, GLuint_);
    unsigned char (*IsEnabled)(GLenum_);
    void (*Enable)(GLenum_);
    void (*Disable)(GLenum_);
};

// THE API WHOSE CONTEXT IS CURRENT on this thread: EGL's or GLX's getProcAddress (WGL's on Windows), or
// NULL with none.
#ifdef _WIN32
// wglGetProcAddress answers only what is newer than OpenGL 1.1; the rest is opengl32.dll's own export.
static void (*wgl_gpa_fn(const char *name))(void)
{
    static const char *const gl[] = { "opengl32.dll", NULL };
    static void *(__stdcall *wgl_gpa)(const char *);
    if (wgl_gpa == NULL) wgl_gpa = (void *(__stdcall *)(const char *))plat_symbol(gl, "wglGetProcAddress");
    void *f = wgl_gpa != NULL ? wgl_gpa(name) : NULL;
    // A driver may answer 1, 2, 3 or -1 for "no such function".
    if ((uintptr_t)f <= 3 || (intptr_t)f == -1) f = plat_symbol(gl, name);
    return (void (*)(void))f;
}

static oxs_egl_proc current_gpa(const char **api)
{
    static const char *const gl[] = { "opengl32.dll", NULL };
    void *(__stdcall *wgl_current)(void) = (void *(__stdcall *)(void))plat_symbol(gl, "wglGetCurrentContext");
    if (wgl_current != NULL && wgl_current() != NULL) {
        *api = "WGL";
        return wgl_gpa_fn;
    }
    return NULL;
}
#else
static oxs_egl_proc current_gpa(const char **api)
{
    static const char *const egl[] = { "libEGL.so.1", NULL };
    static const char *const glx[] = { "libGLX.so.0", "libGL.so.1", NULL };
    void *(*egl_current)(void) = (void *(*)(void))plat_symbol(egl, "eglGetCurrentContext");
    if (egl_current != NULL && egl_current() != NULL) {
        *api = "EGL";
        return (oxs_egl_proc)plat_symbol(egl, "eglGetProcAddress");
    }
    void *(*glx_current)(void) = (void *(*)(void))plat_symbol(glx, "glXGetCurrentContext");
    if (glx_current != NULL && glx_current() != NULL) {
        *api = "GLX";
        return (oxs_egl_proc)plat_symbol(glx, "glXGetProcAddressARB");
    }
    return NULL;
}
#endif

XrResult gl_session_init(struct session *s, const void *binding)
{
    const XrBaseInStructure *b = binding;
    struct gl *gl = calloc(1, sizeof *gl);
    if (gl == NULL) return XR_ERROR_OUT_OF_MEMORY;
    s->gl = gl;
    const char *api = "the binding's";
    if ((int)b->type == XR_TYPE_GRAPHICS_BINDING_EGL_MNDX) gl->gpa = ((const struct binding_egl_mndx *)b)->getProcAddress;
    else gl->gpa = current_gpa(&api);
    if (gl->gpa == NULL) {
        fprintf(stderr, "openxr-sim: OpenGL: no context is current on this thread, in EGL, GLX or WGL\n");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
#define L(name) gl->name = (__typeof__(gl->name))gl->gpa("gl" #name)
    L(GetError); L(GetString); L(GetIntegerv); L(GenTextures); L(DeleteTextures); L(BindTexture); L(TexParameteri);
    L(TexStorage2D); L(TexStorage3D); L(TexImage2D); L(TexImage3D); L(GenFramebuffers); L(BindFramebuffer);
    L(FramebufferTexture2D); L(FramebufferTextureLayer); L(ReadBuffer); L(ReadPixels); L(PixelStorei); L(BindBuffer);
    L(IsEnabled); L(Enable); L(Disable);
#undef L
    if (gl->GetError == NULL || gl->GenTextures == NULL || gl->BindTexture == NULL || gl->TexImage2D == NULL ||
        gl->GenFramebuffers == NULL || gl->BindFramebuffer == NULL || gl->FramebufferTexture2D == NULL ||
        gl->ReadPixels == NULL || gl->GetIntegerv == NULL || gl->GetString == NULL) {
        say("OpenGL: a function is missing");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    // THE CONTEXT MUST BE CURRENT, as XR_KHR_opengl_enable says; without one nothing here can work.
    const char *version = (const char *)gl->GetString(GL_VERSION_);
    if (version == NULL) { say("OpenGL: no context is current"); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
    gl->es = strncmp(version, "OpenGL ES", 9) == 0;
    say("OpenGL: %s, through %s getProcAddress", version, api);
    return XR_SUCCESS;
}

void gl_session_fini(struct session *s)
{
    // The framebuffer belongs to the application's context, which need not be current now: it goes with
    // the context.
    free(s->gl);
    s->gl = NULL;
}

static const int64_t FORMATS[] = {
    GL_SRGB8_ALPHA8_, GL_RGBA8_, GL_DEPTH_COMPONENT32F_, GL_DEPTH24_STENCIL8_, GL_DEPTH_COMPONENT24_, GL_DEPTH_COMPONENT16_,
};

const int64_t *gl_formats(uint32_t *n)
{
    *n = sizeof FORMATS / sizeof FORMATS[0];
    return FORMATS;
}

// What glTexImage needs beside the internal format, where glTexStorage is not there.
static void transfer_of(int64_t f, GLenum_ *format, GLenum_ *type)
{
    switch (f) {
        case GL_DEPTH_COMPONENT32F_: *format = GL_DEPTH_COMPONENT_; *type = GL_FLOAT_; break;
        case GL_DEPTH24_STENCIL8_: *format = GL_DEPTH_STENCIL_; *type = GL_UNSIGNED_INT_24_8_; break;
        case GL_DEPTH_COMPONENT24_: *format = GL_DEPTH_COMPONENT_; *type = GL_UNSIGNED_INT_; break;
        case GL_DEPTH_COMPONENT16_: *format = GL_DEPTH_COMPONENT_; *type = GL_UNSIGNED_SHORT_; break;
        default: *format = GL_RGBA_; *type = GL_UNSIGNED_BYTE_; break;
    }
}

XrResult gl_swapchain_init(struct swapchain *sc)
{
    struct gl *gl = sc->session->gl;
    const int64_t f = sc->info.format;
    sc->color = f == GL_SRGB8_ALPHA8_ || f == GL_RGBA8_;
    sc->linear = f == GL_RGBA8_;
    sc->gl_flipped = true;
    const bool array = sc->info.arraySize > 1;
    const GLenum_ target = array ? GL_TEXTURE_2D_ARRAY_ : GL_TEXTURE_2D_;
    if (array && gl->TexStorage3D == NULL && gl->TexImage3D == NULL) return XR_ERROR_FEATURE_UNSUPPORTED;
    GLint_ previous = 0;
    gl->GetIntegerv(array ? GL_TEXTURE_BINDING_2D_ARRAY_ : GL_TEXTURE_BINDING_2D_, &previous);
    while (gl->GetError() != GL_NO_ERROR_) {}
    GLenum_ format, type;
    transfer_of(f, &format, &type);
    const GLsizei_ w = (GLsizei_)sc->info.width, h = (GLsizei_)sc->info.height, layers = (GLsizei_)sc->info.arraySize;
    const GLsizei_ levels = (GLsizei_)sc->info.mipCount;
    for (uint32_t i = 0; i < sc->count; i++) {
        gl->GenTextures(1, &sc->gl_texture[i]);
        gl->BindTexture(target, sc->gl_texture[i]);
        if (array && gl->TexStorage3D != NULL) gl->TexStorage3D(target, levels, (GLenum_)f, w, h, layers);
        else if (!array && gl->TexStorage2D != NULL) gl->TexStorage2D(target, levels, (GLenum_)f, w, h);
        else
            for (GLint_ m = 0; m < levels; m++) {
                const GLsizei_ mw = w >> m > 0 ? w >> m : 1, mh = h >> m > 0 ? h >> m : 1;
                if (array) gl->TexImage3D(target, m, (GLint_)f, mw, mh, layers, 0, format, type, NULL);
                else gl->TexImage2D(target, m, (GLint_)f, mw, mh, 0, format, type, NULL);
            }
        // Complete without mipmaps, whatever the application samples it with.
        gl->TexParameteri(target, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
        gl->TexParameteri(target, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
        gl->TexParameteri(target, GL_TEXTURE_MAX_LEVEL_, levels - 1);
        if (sc->color && (sc->pixels[i] = malloc((size_t)w * h * 4 * layers)) == NULL) return XR_ERROR_OUT_OF_MEMORY;
    }
    gl->BindTexture(target, (GLuint_)previous);
    const GLenum_ e = gl->GetError();
    if (e != GL_NO_ERROR_) { say("OpenGL: creating the textures failed (0x%x)", e); return XR_ERROR_RUNTIME_FAILURE; }
    return XR_SUCCESS;
}

void gl_swapchain_fini(struct swapchain *sc)
{
    struct gl *gl = sc->session->gl;
    for (uint32_t i = 0; i < 4; i++) {
        // The textures, if the application's context is still current (OpenXR asks that it be).
        if (gl != NULL && sc->gl_texture[i] != 0 && gl->GetString(GL_VERSION_) != NULL) gl->DeleteTextures(1, &sc->gl_texture[i]);
        free(sc->pixels[i]);
        sc->pixels[i] = NULL;
    }
}

XrResult gl_enumerate_images(struct swapchain *sc, XrSwapchainImageBaseHeader *images)
{
    // XrSwapchainImageOpenGLKHR and XrSwapchainImageOpenGLESKHR are the same shape: type, next, a name.
    XrSwapchainImageOpenGLKHR *out = (XrSwapchainImageOpenGLKHR *)images;
    for (uint32_t i = 0; i < sc->count; i++) {
        if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR && out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR)
            return XR_ERROR_VALIDATION_FAILURE;
        out[i].image = sc->gl_texture[i];
    }
    return XR_SUCCESS;
}

XrResult gl_swapchain_read(struct swapchain *sc, uint32_t image)
{
    struct gl *gl = sc->session->gl;
    if (gl->GetString(GL_VERSION_) == NULL) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    while (gl->GetError() != GL_NO_ERROR_) {}
    if (gl->fbo == 0) gl->GenFramebuffers(1, &gl->fbo);
    GLint_ read_fbo = 0, pack_buffer = 0, align = 4, row = 0;
    gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING_, &read_fbo);
    gl->GetIntegerv(GL_PACK_ALIGNMENT_, &align);
    gl->GetIntegerv(GL_PACK_ROW_LENGTH_, &row);
    if (gl->BindBuffer != NULL) gl->GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING_, &pack_buffer);
    while (gl->GetError() != GL_NO_ERROR_) {}   // a binding this context does not have
    // The values as they are stored: with sRGB writes on, a desktop GL may convert what it reads back.
    const bool srgb = !gl->es && gl->IsEnabled != NULL && gl->IsEnabled(GL_FRAMEBUFFER_SRGB_);
    if (srgb) gl->Disable(GL_FRAMEBUFFER_SRGB_);
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER_, gl->fbo);
    if (gl->BindBuffer != NULL) gl->BindBuffer(GL_PIXEL_PACK_BUFFER_, 0);
    gl->PixelStorei(GL_PACK_ALIGNMENT_, 4);
    gl->PixelStorei(GL_PACK_ROW_LENGTH_, 0);
    if (gl->ReadBuffer != NULL) gl->ReadBuffer(GL_COLOR_ATTACHMENT0_);
    const GLsizei_ w = (GLsizei_)sc->info.width, h = (GLsizei_)sc->info.height;
    for (uint32_t layer = 0; layer < sc->info.arraySize; layer++) {
        if (sc->info.arraySize > 1) gl->FramebufferTextureLayer(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, sc->gl_texture[image], 0, (GLint_)layer);
        else gl->FramebufferTexture2D(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, sc->gl_texture[image], 0);
        gl->ReadPixels(0, 0, w, h, GL_RGBA_, GL_UNSIGNED_BYTE_, sc->pixels[image] + (size_t)layer * w * h * 4);
    }
    gl->FramebufferTexture2D(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, 0, 0);
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER_, (GLuint_)read_fbo);
    if (gl->BindBuffer != NULL) gl->BindBuffer(GL_PIXEL_PACK_BUFFER_, (GLuint_)pack_buffer);
    gl->PixelStorei(GL_PACK_ALIGNMENT_, align);
    gl->PixelStorei(GL_PACK_ROW_LENGTH_, row);
    if (srgb) gl->Enable(GL_FRAMEBUFFER_SRGB_);
    const GLenum_ e = gl->GetError();
    if (e != GL_NO_ERROR_) { say("OpenGL: reading the image back failed (0x%x)", e); return XR_ERROR_RUNTIME_FAILURE; }
    return XR_SUCCESS;
}
