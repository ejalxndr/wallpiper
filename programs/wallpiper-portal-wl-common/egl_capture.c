/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ethan Alexander
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "egl_capture.h"
#include "state_internal.h"

#include <wallpiper/vk_format.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <gbm.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

typedef EGLImageKHR (*PfnEglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum,
                                            EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*PfnEglDestroyImageKHR)(EGLDisplay, EGLImageKHR);
typedef void (*PfnGlEGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);

typedef struct {
  bool attempted;
  bool ok;

  int drm_fd;
  struct gbm_device *gbm;
  EGLDisplay display;
  EGLContext context;

  PfnEglCreateImageKHR eglCreateImageKHR;
  PfnEglDestroyImageKHR eglDestroyImageKHR;
  PfnGlEGLImageTargetTexture2DOES glEGLImageTargetTexture2DOES;

  GLuint blitProgram;
  GLint blitTexLoc;
} wp_wl_egl_capture_ctx_t;

static wp_wl_egl_capture_ctx_t g_ctx;

static bool open_render_node(int *out_fd) {
  for (int minor = 128; minor < 136; minor++) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
      *out_fd = fd;
      return true;
    }
  }
  return false;
}

static GLuint compile_shader(GLenum type, const char *src) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, NULL);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    printf("[capture] shader compile failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static bool ensure_blit_program(wp_wl_egl_capture_ctx_t *ctx) {
  if (ctx->blitProgram) {
    return true;
  }

  static const char *vs_src = "attribute vec2 pos;\n"
                              "varying vec2 uv;\n"
                              "void main() {\n"
                              "  uv = pos * 0.5 + 0.5;\n"
                              "  gl_Position = vec4(pos, 0.0, 1.0);\n"
                              "}\n";
  static const char *fs_src = "precision mediump float;\n"
                              "uniform sampler2D tex;\n"
                              "varying vec2 uv;\n"
                              "void main() {\n"
                              "  gl_FragColor = texture2D(tex, uv);\n"
                              "}\n";

  GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
  if (!vs || !fs) {
    if (vs)
      glDeleteShader(vs);
    if (fs)
      glDeleteShader(fs);
    return false;
  }

  GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glBindAttribLocation(program, 0, "pos");
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[512];
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    printf("[capture] blit program link failed: %s\n", log);
    glDeleteProgram(program);
    return false;
  }

  ctx->blitProgram = program;
  ctx->blitTexLoc = glGetUniformLocation(program, "tex");
  return true;
}

static bool ensure_egl_ctx(wp_wl_egl_capture_ctx_t *ctx, char *err,
                           size_t err_len) {
  if (ctx->attempted) {
    if (!ctx->ok) {
      snprintf(err, err_len, "%s",
               "headless EGL/GBM capture context failed "
               "to initialize earlier, see log");
    }
    return ctx->ok;
  }
  ctx->attempted = true;

  if (!open_render_node(&ctx->drm_fd)) {
    snprintf(err, err_len, "%s", "no /dev/dri/renderD* node found");
    return false;
  }

  ctx->gbm = gbm_create_device(ctx->drm_fd);
  if (!ctx->gbm) {
    snprintf(err, err_len, "%s", "gbm_create_device failed");
    return false;
  }

  PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
          "eglGetPlatformDisplayEXT");
  if (eglGetPlatformDisplayEXT) {
    ctx->display =
        eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, ctx->gbm, NULL);
  } else {
    ctx->display = eglGetDisplay((EGLNativeDisplayType)ctx->gbm);
  }
  if (ctx->display == EGL_NO_DISPLAY) {
    snprintf(err, err_len, "%s",
             "eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR) "
             "failed");
    return false;
  }

  EGLint major = 0, minor = 0;
  if (!eglInitialize(ctx->display, &major, &minor)) {
    snprintf(err, err_len, "%s", "eglInitialize failed");
    return false;
  }

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    snprintf(err, err_len, "%s", "eglBindAPI(EGL_OPENGL_ES_API) failed");
    return false;
  }

  const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE,
      EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,
      8,
      EGL_GREEN_SIZE,
      8,
      EGL_BLUE_SIZE,
      8,
      EGL_ALPHA_SIZE,
      8,
      EGL_NONE,
  };
  EGLConfig config;
  EGLint num_configs = 0;
  if (!eglChooseConfig(ctx->display, config_attribs, &config, 1,
                       &num_configs) ||
      num_configs == 0) {
    snprintf(err, err_len, "%s", "eglChooseConfig found no usable config");
    return false;
  }

  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  ctx->context =
      eglCreateContext(ctx->display, config, EGL_NO_CONTEXT, context_attribs);
  if (ctx->context == EGL_NO_CONTEXT) {
    snprintf(err, err_len, "%s", "eglCreateContext failed");
    return false;
  }

  if (!eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                      ctx->context)) {
    snprintf(err, err_len, "%s",
             "eglMakeCurrent (surfaceless) failed -- is "
             "EGL_KHR_surfaceless_context supported?");
    return false;
  }

  ctx->eglCreateImageKHR =
      (PfnEglCreateImageKHR)eglGetProcAddress("eglCreateImageKHR");
  ctx->eglDestroyImageKHR =
      (PfnEglDestroyImageKHR)eglGetProcAddress("eglDestroyImageKHR");
  ctx->glEGLImageTargetTexture2DOES =
      (PfnGlEGLImageTargetTexture2DOES)eglGetProcAddress(
          "glEGLImageTargetTexture2DOES");
  if (!ctx->eglCreateImageKHR || !ctx->eglDestroyImageKHR ||
      !ctx->glEGLImageTargetTexture2DOES) {
    snprintf(err, err_len, "%s",
             "EGL_KHR_image_base / GL_OES_EGL_image not available");
    return false;
  }

  if (!ensure_blit_program(ctx)) {
    snprintf(err, err_len, "%s", "failed to compile capture blit shader");
    return false;
  }

  ctx->ok = true;
  return true;
}

bool wp_wl_egl_render_node(uint32_t *out_major, uint32_t *out_minor) {
  char err[128];
  if (!ensure_egl_ctx(&g_ctx, err, sizeof(err))) {
    return false;
  }
  struct stat st;
  if (fstat(g_ctx.drm_fd, &st) != 0) {
    return false;
  }
  *out_major = major(st.st_rdev);
  *out_minor = minor(st.st_rdev);
  return true;
}

bool wp_wl_egl_capture_readback(wp_wl_state_t *state, uint32_t channel,
                                uint8_t **out_pixels, int *out_width,
                                int *out_height, char *err, size_t err_len) {
  wp_wl_output_t *out = wp_wl_find_output_for_channel(state, channel);
  if (!out || !out->has_current_source ||
      out->current_source.kind != WP_WL_SOURCE_SLOT) {
    snprintf(err, err_len, "no active wallpaper frame on channel %u", channel);
    return false;
  }

  uint32_t local_idx = out->current_source.slot % WP_WL_CAPTURE_SLOT_COUNT;
  wp_wl_slot_t *slot = &out->slots[local_idx];
  if (!slot->in_use || slot->slot != out->current_source.slot) {
    snprintf(err, err_len, "no active wallpaper frame on channel %u", channel);
    return false;
  }

  if (!ensure_egl_ctx(&g_ctx, err, err_len)) {
    return false;
  }

  const int width = (int)slot->width;
  const int height = (int)slot->height;

  const uint32_t fourcc = wp_drm_fourcc_from_vk_format(slot->format, NULL);
  const EGLint modifier_lo = (EGLint)(slot->modifier & 0xffffffffu);
  const EGLint modifier_hi = (EGLint)(slot->modifier >> 32);
  const EGLint image_attribs[] = {
      EGL_WIDTH,
      width,
      EGL_HEIGHT,
      height,
      EGL_LINUX_DRM_FOURCC_EXT,
      (EGLint)fourcc,
      EGL_DMA_BUF_PLANE0_FD_EXT,
      slot->fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
      0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT,
      (EGLint)slot->stride,
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
      modifier_lo,
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
      modifier_hi,
      EGL_NONE,
  };

  EGLImageKHR image =
      g_ctx.eglCreateImageKHR(g_ctx.display, EGL_NO_CONTEXT,
                              EGL_LINUX_DMA_BUF_EXT, NULL, image_attribs);
  if (image == EGL_NO_IMAGE_KHR) {
    snprintf(err, err_len, "eglCreateImageKHR failed (0x%x)", eglGetError());
    return false;
  }

  GLuint src_tex = 0;
  glGenTextures(1, &src_tex);
  glBindTexture(GL_TEXTURE_2D, src_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  g_ctx.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

  GLuint dst_tex = 0;
  glGenTextures(1, &dst_tex);
  glBindTexture(GL_TEXTURE_2D, dst_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         dst_tex, 0);

  bool ok = false;
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    snprintf(err, err_len, "%s", "capture FBO incomplete");
  } else {
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(g_ctx.blitProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(g_ctx.blitTexLoc, 0);

    const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    uint8_t *pixels = malloc((size_t)width * (size_t)height * 4);
    if (!pixels) {
      snprintf(err, err_len, "%s", "out of memory");
    } else {
      glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
      *out_pixels = pixels;
      *out_width = width;
      *out_height = height;
      ok = true;
    }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &dst_tex);
  glDeleteTextures(1, &src_tex);
  g_ctx.eglDestroyImageKHR(g_ctx.display, image);

  return ok;
}
