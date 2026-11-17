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
#include <vector>

#include <log/log.h>
#include <cutils/properties.h>

#include "../hooks.h"
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
 * glShaderSource() is wrapped for drivers that support external textures in
 * GLSL ES 3.00 shaders but only advertise GL_OES_EGL_image_external, not
 * GL_OES_EGL_image_external_essl3 (e.g. Adreno 3xx). Shaders that enable the
 * essl3 extension fail to compile there, so its name is replaced with the base
 * extension (padded with spaces to keep every string length unchanged). The
 * rewrite only happens when the driver lacks essl3 but has the base extension.
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

static bool needsExternalEssl3Rewrite() {
    // 0 = not checked yet, 1 = rewrite, 2 = pass through. One driver per process.
    static std::atomic<int> state{0};
    int s = state.load(std::memory_order_relaxed);
    if (s == 0) {
        const char* ext = reinterpret_cast<const char*>(__glGetString(GL_EXTENSIONS));
        if (!ext) return false; // no current context; retry next time
        s = (!hasGlExtension(ext, "GL_OES_EGL_image_external_essl3") &&
             hasGlExtension(ext, "GL_OES_EGL_image_external")) ? 1 : 2;
        if (s == 1) {
            ALOGI("glShaderSource: driver lacks GL_OES_EGL_image_external_essl3, "
                  "rewriting it to GL_OES_EGL_image_external");
        }
        state.store(s, std::memory_order_relaxed);
    }
    return s == 1;
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                    const GLint *length) {
    static constexpr char kEssl3[] = "GL_OES_EGL_image_external_essl3";
    static constexpr size_t kEssl3Len = sizeof(kEssl3) - 1;
    static constexpr size_t kBaseLen = sizeof("GL_OES_EGL_image_external") - 1;

    if (count <= 0 || string == nullptr) {
        return __glShaderSource(shader, count, string, length);
    }

    auto pieceLen = [&](GLsizei i) -> size_t {
        return (length && length[i] >= 0) ? static_cast<size_t>(length[i])
                                          : (string[i] ? strlen(string[i]) : 0);
    };

    bool found = false;
    for (GLsizei i = 0; i < count && !found; i++) {
        if (string[i]) {
            found = std::string_view(string[i], pieceLen(i)).find(kEssl3) !=
                    std::string_view::npos;
        }
    }
    if (!found || !needsExternalEssl3Rewrite()) {
        return __glShaderSource(shader, count, string, length);
    }

    std::vector<std::string> copies(count);
    std::vector<const GLchar*> ptrs(count);
    for (GLsizei i = 0; i < count; i++) {
        if (!string[i]) {
            ptrs[i] = nullptr;
            continue;
        }
        std::string& s = copies[i];
        s.assign(string[i], pieceLen(i));
        for (size_t pos = 0; (pos = s.find(kEssl3, pos)) != std::string::npos; pos += kEssl3Len) {
            s.replace(pos + kBaseLen, kEssl3Len - kBaseLen, kEssl3Len - kBaseLen, ' ');
        }
        ptrs[i] = s.c_str();
    }
    // Lengths are unchanged, so the caller's length array stays valid.
    __glShaderSource(shader, count, ptrs.data(), length);
}
