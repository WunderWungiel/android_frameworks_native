/*
 ** Copyright 2007, The Android Open Source Project
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#include <atomic>
#include <string>
#include <string_view>

#include <log/log.h>
#include <cutils/properties.h>

#include "../hooks.h"
#include "shader_rewrite.h"
#include "../egl_impl.h"

using namespace android;

// ----------------------------------------------------------------------------
// Actual GL entry-points
// ----------------------------------------------------------------------------

#undef API_ENTRY
#undef CALL_GL_API
#undef CALL_GL_API_INTERNAL_CALL
#undef CALL_GL_API_INTERNAL_SET_RETURN_VALUE
#undef CALL_GL_API_INTERNAL_DO_RETURN
#undef CALL_GL_API_RETURN

#if USE_SLOW_BINDING

    #define API_ENTRY(_api) _api

    #define CALL_GL_API_INTERNAL_CALL(_api, ...)                         \
        gl_hooks_t::gl_t const * const _c = &getGlThreadSpecific()->gl;  \
        if (_c) return _c->_api(__VA_ARGS__);

    #define CALL_GL_API_INTERNAL_SET_RETURN_VALUE return 0;

    // This stays blank, since void functions will implicitly return, and
    // all of the other functions will return 0 based on the previous macro.
    #define CALL_GL_API_INTERNAL_DO_RETURN

#elif defined(__arm__)

    #define GET_TLS(reg) "mrc p15, 0, " #reg ", c13, c0, 3 \n"

    #define API_ENTRY(_api) __attribute__((naked,noinline)) _api

    #define CALL_GL_API_INTERNAL_CALL(_api, ...)                 \
        asm volatile(                                            \
            GET_TLS(r12)                                         \
            "ldr   r12, [r12, %[tls]] \n"                        \
            "cmp   r12, #0            \n"                        \
            "ldrne pc,  [r12, %[api]] \n"                        \
            :                                                    \
            : [tls] "J"(TLS_SLOT_OPENGL_API*4),                  \
              [api] "J"(__builtin_offsetof(gl_hooks_t, gl._api)) \
            : "r0", "r1", "r2", "r3", "r12"                      \
        );

    #define CALL_GL_API_INTERNAL_SET_RETURN_VALUE \
        asm volatile(                             \
            "mov r0, #0 \n"                       \
            :                                     \
            :                                     \
            : "r0"                                \
        );


    #define CALL_GL_API_INTERNAL_DO_RETURN \
        asm volatile(                      \
            "bx lr \n"                     \
            :                              \
            :                              \
            : "r0"                         \
        );

#elif defined(__aarch64__)

    #define API_ENTRY(_api) __attribute__((naked,noinline)) _api

    #define CALL_GL_API_INTERNAL_CALL(_api, ...)                    \
        asm volatile(                                               \
            "mrs x16, tpidr_el0\n"                                  \
            "ldr x16, [x16, %[tls]]\n"                              \
            "cbz x16, 1f\n"                                         \
            "ldr x16, [x16, %[api]]\n"                              \
            "br  x16\n"                                             \
            "1:\n"                                                  \
            :                                                       \
            : [tls] "i" (TLS_SLOT_OPENGL_API * sizeof(void*)),      \
              [api] "i" (__builtin_offsetof(gl_hooks_t, gl._api))   \
            : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x16" \
        );

    #define CALL_GL_API_INTERNAL_SET_RETURN_VALUE \
        asm volatile(                             \
            "mov w0, wzr \n"                      \
            :                                     \
            :                                     \
            : "w0"                                \
        );

    #define CALL_GL_API_INTERNAL_DO_RETURN \
        asm volatile(                      \
            "ret \n"                       \
            :                              \
            :                              \
            :                              \
        );

#elif defined(__i386__)

    #define API_ENTRY(_api) __attribute__((naked,noinline)) _api

    #define CALL_GL_API_INTERNAL_CALL(_api, ...)                    \
        __asm__ volatile(                                           \
            "mov %%gs:0, %%eax\n"                                   \
            "mov %P[tls](%%eax), %%eax\n"                           \
            "test %%eax, %%eax\n"                                   \
            "je 1f\n"                                               \
            "jmp *%P[api](%%eax)\n"                                 \
            "1:\n"                                                  \
            :                                                       \
            : [tls] "i" (TLS_SLOT_OPENGL_API*sizeof(void*)),        \
              [api] "i" (__builtin_offsetof(gl_hooks_t, gl._api))   \
            : "cc", "%eax"                                          \
            );

    #define CALL_GL_API_INTERNAL_SET_RETURN_VALUE \
        __asm__ volatile(                         \
            "xor %%eax, %%eax\n"                  \
            :                                     \
            :                                     \
            : "%eax"                              \
        );

    #define CALL_GL_API_INTERNAL_DO_RETURN \
        __asm__ volatile(                  \
            "ret\n"                        \
            :                              \
            :                              \
            :                              \
        );

#elif defined(__x86_64__)

    #define API_ENTRY(_api) __attribute__((naked,noinline)) _api

    #define CALL_GL_API_INTERNAL_CALL(_api, ...)                    \
        __asm__ volatile(                                           \
            "mov %%fs:0, %%rax\n"                                   \
            "mov %P[tls](%%rax), %%rax\n"                           \
            "test %%rax, %%rax\n"                                   \
            "je 1f\n"                                               \
            "jmp *%P[api](%%rax)\n"                                 \
            "1:\n"                                                  \
            :                                                       \
            : [tls] "i" (TLS_SLOT_OPENGL_API*sizeof(void*)),        \
              [api] "i" (__builtin_offsetof(gl_hooks_t, gl._api))   \
            : "cc", "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9",   \
              "%xmm0", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "%xmm5", \
              "%xmm6", "%xmm7"                                      \
        );

    #define CALL_GL_API_INTERNAL_SET_RETURN_VALUE \
        __asm__ volatile(                         \
            "xor %%eax, %%eax\n"                  \
            :                                     \
            :                                     \
            : "%eax"                              \
        );

    #define CALL_GL_API_INTERNAL_DO_RETURN \
        __asm__ volatile(                  \
            "retq\n"                       \
            :                              \
            :                              \
            :                              \
        );

#elif defined(__riscv)

    #define API_ENTRY(_api) __attribute__((naked,noinline)) _api

    #define CALL_GL_API_INTERNAL_CALL(_api, ...)                 \
        asm volatile(                                            \
            "mv t0, tp\n"                                        \
            "li t1, %[tls]\n"                                    \
            "add t0, t0, t1\n"                                   \
            "ld t0, 0(t0)\n"                                     \
            "beqz t0, 1f\n"                                      \
            "li t1, %[api]\n"                                    \
            "add t0, t0, t1\n"                                   \
            "ld t0, 0(t0)\n"                                     \
            "jalr x0, t0\n"                                      \
            "1:\n"                                               \
            :                                                    \
            : [tls] "i"(TLS_SLOT_OPENGL_API*sizeof(void *)),     \
              [api] "i"(__builtin_offsetof(gl_hooks_t, gl._api)) \
            : "t0", "t1", "t2", "a0", "a1", "a2", "a3", "a4",    \
              "a5", "t6", "t3", "t4", "t5", "t6"                 \
        );

    #define CALL_GL_API_INTERNAL_SET_RETURN_VALUE \
        asm volatile(                             \
            "li a0, 0\n"                          \
            :                                     \
            :                                     \
            : "a0"                                \
        );

    #define CALL_GL_API_INTERNAL_DO_RETURN \
        asm volatile(                      \
            "ret\n"                        \
            :                              \
            :                              \
            :                              \
        );

#endif

#define CALL_GL_API(_api, ...) \
    CALL_GL_API_INTERNAL_CALL(_api, __VA_ARGS__) \
    CALL_GL_API_INTERNAL_DO_RETURN

#define CALL_GL_API_RETURN(_api, ...) \
    CALL_GL_API_INTERNAL_CALL(_api, __VA_ARGS__) \
    CALL_GL_API_INTERNAL_SET_RETURN_VALUE \
    CALL_GL_API_INTERNAL_DO_RETURN

extern "C" {
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "gl2_api.in"
#include "gl2ext_api.in"
#pragma GCC diagnostic warning "-Wunused-parameter"
}

#undef API_ENTRY
#undef CALL_GL_API
#undef CALL_GL_API_INTERNAL_CALL
#undef CALL_GL_API_INTERNAL_SET_RETURN_VALUE
#undef CALL_GL_API_INTERNAL_DO_RETURN
#undef CALL_GL_API_RETURN

/*
 * glGetString() and glGetStringi() are special because we expose some
 * extensions in the wrapper. Also, wrapping glGetXXX() is required because
 * the value returned for GL_NUM_EXTENSIONS may have been altered by the
 * injection of the additional extensions.
 */

extern "C" {
    const GLubyte * __glGetString(GLenum name);
    const GLubyte * __glGetStringi(GLenum name, GLuint index);
    void __glGetBooleanv(GLenum pname, GLboolean * data);
    void __glGetFloatv(GLenum pname, GLfloat * data);
    void __glGetIntegerv(GLenum pname, GLint * data);
    void __glGetInteger64v(GLenum pname, GLint64 * data);
}

const GLubyte * glGetString(GLenum name) {
    egl_connection_t* const cnx = egl_get_connection();
    return cnx->platform.glGetString(name);
}

const GLubyte * glGetStringi(GLenum name, GLuint index) {
    egl_connection_t* const cnx = egl_get_connection();
    return cnx->platform.glGetStringi(name, index);
}

void glGetBooleanv(GLenum pname, GLboolean * data) {
    egl_connection_t* const cnx = egl_get_connection();
    return cnx->platform.glGetBooleanv(pname, data);
}

void glGetFloatv(GLenum pname, GLfloat * data) {
    egl_connection_t* const cnx = egl_get_connection();
    return cnx->platform.glGetFloatv(pname, data);
}

void glGetIntegerv(GLenum pname, GLint * data) {
    egl_connection_t* const cnx = egl_get_connection();
    return cnx->platform.glGetIntegerv(pname, data);
}

void glGetInteger64v(GLenum pname, GLint64 * data) {
    egl_connection_t* const cnx = egl_get_connection();
    return cnx->platform.glGetInteger64v(pname, data);
}

/*
 * glShaderSource() is wrapped to work around shader compiler bugs of old drivers:
 *
 * - Drivers that support external textures in GLSL ES 3.00 shaders but only
 *   advertise GL_OES_EGL_image_external, not GL_OES_EGL_image_external_essl3
 *   (e.g. Adreno 3xx): shaders that enable the essl3 extension fail to compile,
 *   so its name is replaced with the base extension (padded with spaces).
 *
 * - Adreno 3xx: glLinkProgram() crashes (SIGSEGV in libsc-a3xx.so) on loops
 *   whose breaks sit in nested if/else branches, e.g. the
 *   `do { if (a) { if (b) { ...; break; } else { ...; break; } } ... } while(false);`
 *   blocks SPIRV-Cross emits for structured selections (Flutter/Impeller
 *   shaders). When every break of such a block is in tail position the wrapper
 *   is redundant and is removed (see shader_rewrite.cpp).
 *
 * Each rewrite only happens when the current driver needs it.
 */

extern "C" {
    void __glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                          const GLint *length);
}

static bool hasGlExtension(const char* extensions, const char* name) {
    if (!extensions) return false;
    const size_t len = strlen(name);
    for (const char* p = extensions; (p = strstr(p, name)) != nullptr; p += len) {
        if ((p == extensions || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
            return true;
        }
    }
    return false;
}

enum : int {
    kQuirkUnknown = -1,
    kQuirkExternalEssl3 = 1 << 0,
    kQuirkFlattenDoWhile = 1 << 1,
};

static int shaderSourceQuirks() {
    // One driver per process, so the result is computed once.
    static std::atomic<int> state{kQuirkUnknown};
    int q = state.load(std::memory_order_relaxed);
    if (q == kQuirkUnknown) {
        const char* ext = reinterpret_cast<const char*>(__glGetString(GL_EXTENSIONS));
        const char* renderer = reinterpret_cast<const char*>(__glGetString(GL_RENDERER));
        if (!ext || !renderer) return 0; // no current context; retry next time
        q = 0;
        if (!hasGlExtension(ext, "GL_OES_EGL_image_external_essl3") &&
            hasGlExtension(ext, "GL_OES_EGL_image_external")) {
            q |= kQuirkExternalEssl3;
            ALOGI("glShaderSource: driver lacks GL_OES_EGL_image_external_essl3, "
                  "rewriting it to GL_OES_EGL_image_external");
        }
        if (strncmp(renderer, "Adreno (TM) 3", strlen("Adreno (TM) 3")) == 0) {
            q |= kQuirkFlattenDoWhile;
            ALOGI("glShaderSource: %s, flattening do-while(false) blocks", renderer);
        }
        state.store(q, std::memory_order_relaxed);
    }
    return q;
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                    const GLint *length) {
    static constexpr char kEssl3[] = "GL_OES_EGL_image_external_essl3";
    static constexpr size_t kEssl3Len = sizeof(kEssl3) - 1;
    static constexpr size_t kBaseLen = sizeof("GL_OES_EGL_image_external") - 1;

    if (count <= 0 || string == nullptr) {
        return __glShaderSource(shader, count, string, length);
    }

    auto piece = [&](GLsizei i) -> std::string_view {
        if (!string[i]) return {};
        return std::string_view(string[i], (length && length[i] >= 0)
                                                   ? static_cast<size_t>(length[i])
                                                   : strlen(string[i]));
    };

    bool hasEssl3 = false, hasFalse = false;
    for (GLsizei i = 0; i < count; i++) {
        const std::string_view p = piece(i);
        hasEssl3 = hasEssl3 || p.find(kEssl3) != std::string_view::npos;
        hasFalse = hasFalse || p.find("false") != std::string_view::npos;
    }
    if (!hasEssl3 && !hasFalse) {
        return __glShaderSource(shader, count, string, length);
    }
    const int quirks = shaderSourceQuirks();
    const bool essl3 = hasEssl3 && (quirks & kQuirkExternalEssl3);
    const bool flatten = hasFalse && (quirks & kQuirkFlattenDoWhile);
    if (!essl3 && !flatten) {
        return __glShaderSource(shader, count, string, length);
    }

    // The driver concatenates the pieces anyway; join them so that rewrites can
    // see whole statements.
    std::string src;
    for (GLsizei i = 0; i < count; i++) {
        src.append(piece(i));
    }
    if (essl3) {
        for (size_t pos = 0; (pos = src.find(kEssl3, pos)) != std::string::npos;
             pos += kEssl3Len) {
            src.replace(pos + kBaseLen, kEssl3Len - kBaseLen, kEssl3Len - kBaseLen, ' ');
        }
    }
    if (flatten) {
        android::gles::flattenTrivialDoWhile(src);
    }
    const GLchar* ptr = src.c_str();
    const GLint len = static_cast<GLint>(src.size());
    __glShaderSource(shader, 1, &ptr, &len);
}
