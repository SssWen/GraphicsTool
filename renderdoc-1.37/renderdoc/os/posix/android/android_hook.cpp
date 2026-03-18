/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2024 Baldur Karlsson
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "common/common.h"
#include "common/threading.h"
#include "hooks/hooks.h"
#include "plthook/plthook.h"
#include "driver/gl/gl_dispatch_table_defs.h"
#include "driver/gl/egl_dispatch_table.h"

#include <android/dlext.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <link.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <set>

#if defined(RENDERDOC_HAVE_DOBBY)
#include "xdl.h"
#include "dobby.h"
#include <time.h>
#endif

// uncomment the following to print (very verbose) debugging prints for the android PLT hooking
// #define HOOK_DEBUG_PRINT(...) RDCLOG(__VA_ARGS__)

#if !defined(HOOK_DEBUG_PRINT)
#define HOOK_DEBUG_PRINT(...) \
  do                          \
  {                           \
  } while(0)
#endif

// from plthook_elf.c
#if defined __x86_64__ || defined __x86_64
#define R_JUMP_SLOT R_X86_64_JUMP_SLOT
#define Elf_Rel ElfW(Rela)
#define ELF_R_TYPE ELF64_R_TYPE
#define ELF_R_SYM ELF64_R_SYM
#elif defined __i386__ || defined __i386
#define R_JUMP_SLOT R_386_JMP_SLOT
#define Elf_Rel ElfW(Rel)
#define ELF_R_TYPE ELF32_R_TYPE
#define ELF_R_SYM ELF32_R_SYM
#elif defined __arm__ || defined __arm
#define R_JUMP_SLOT R_ARM_JUMP_SLOT
#define Elf_Rel ElfW(Rel)
#define ELF_R_TYPE ELF32_R_TYPE
#define ELF_R_SYM ELF32_R_SYM
#elif defined __aarch64__ || defined __aarch64 /* ARM64 */
#define R_JUMP_SLOT R_AARCH64_JUMP_SLOT
#define Elf_Rel ElfW(Rela)
#define ELF_R_TYPE ELF64_R_TYPE
#define ELF_R_SYM ELF64_R_SYM
#else
#error unsupported OS
#endif

class HookingInfo
{
public:
  void AddFunctionHook(const FunctionHook &hook)
  {
    SCOPED_LOCK(lock);
    funchooks.push_back(hook);

    // add to map to speed-up lookup in GetFunctionHook
    funchook_map[hook.function] = hook;
  }

  void AddLibHook(const rdcstr &name)
  {
    SCOPED_LOCK(lock);
    if(!libhooks.contains(name))
      libhooks.push_back(name);
  }

  void AddHookCallback(const rdcstr &name, FunctionLoadCallback callback)
  {
    SCOPED_LOCK(lock);
    hookcallbacks[name].push_back(callback);
  }

  rdcarray<FunctionHook> GetFunctionHooks()
  {
    SCOPED_LOCK(lock);
    return funchooks;
  }

  void ClearHooks()
  {
    SCOPED_LOCK(lock);
    libhooks.clear();
    funchooks.clear();
    funchook_map.clear();
  }

  rdcarray<rdcstr> GetLibHooks()
  {
    SCOPED_LOCK(lock);
    return libhooks;
  }

  std::map<rdcstr, rdcarray<FunctionLoadCallback>> GetHookCallbacks()
  {
    SCOPED_LOCK(lock);
    return hookcallbacks;
  }

  FunctionHook GetFunctionHook(const rdcstr &name)
  {
    SCOPED_LOCK(lock);
    return funchook_map[name];
  }

  bool IsLibHook(const rdcstr &path)
  {
    SCOPED_LOCK(lock);
    for(const rdcstr &filename : libhooks)
    {
      if(path.contains(filename))
      {
        HOOK_DEBUG_PRINT("Intercepting and returning ourselves for %s (matches %s)", path.c_str(),
                         filename.c_str());
        return true;
      }
    }

    return false;
  }

  bool IsLibHook(void *handle)
  {
    SCOPED_LOCK(lock);
    for(const rdcstr &lib : libhooks)
    {
      void *libHandle = dlopen(lib.c_str(), RTLD_NOLOAD);
      HOOK_DEBUG_PRINT("%s is %p", lib.c_str(), libHandle);
      if(libHandle == handle)
        return true;
    }

    return false;
  }

  bool IsHooked(void *handle)
  {
    SCOPED_LOCK(lock);
    bool ret = hooked_handle_already.find(handle) != hooked_handle_already.end();
    return ret;
  }

  bool IsHooked(const rdcstr &soname)
  {
    SCOPED_LOCK(lock);
    if(hooked_soname_already.find(soname) != hooked_soname_already.end())
      return true;

    // above will be absolute path, allow substring matches
    for(const rdcstr &fn : hooked_soname_already)
      if(soname.contains(fn))
        return true;

    return false;
  }

  void SetHooked(void *handle)
  {
    SCOPED_LOCK(lock);
    hooked_handle_already.insert(handle);
  }

  void SetHooked(const rdcstr &soname)
  {
    SCOPED_LOCK(lock);
    hooked_soname_already.insert(soname);
  }

private:
  std::set<rdcstr> hooked_soname_already;
  std::set<void *> hooked_handle_already;

  rdcarray<FunctionHook> funchooks;
  std::map<rdcstr, FunctionHook> funchook_map;
  rdcarray<rdcstr> libhooks;

  std::map<rdcstr, rdcarray<FunctionLoadCallback>> hookcallbacks;

  Threading::CriticalSection lock;
};

HookingInfo &GetHookInfo()
{
  static HookingInfo hookinfo;
  return hookinfo;
}

#if defined(RENDERDOC_HAVE_DOBBY)
static Threading::CriticalSection g_DobbyHookLock;
static std::set<void *> g_DobbyHookedTargets;
static std::map<rdcstr, void *> g_DobbyOrigByName;
static rdcstr g_SelfLibraryPath;
static rdcstr g_SelfLibraryBase;

// 判断给定函数名是否出现在 RenderDoc 的 GL 支持列表中。
static bool IsGLSupportedByRenderDoc(const char *name)
{
  if(name == NULL || name[0] == 0)
    return false;

  bool matched = false;

#define CHECK_GL_SUPPORTED(func, aliasName)                 \
  if(!matched && strcmp(name, STRINGIZE(aliasName)) == 0)   \
    matched = true;

  // 利用 gl_dispatch_table_defs.h 中的 ForEachSupported 宏，自动跟随 RenderDoc 的
  // GL 支持集合，不需要手工维护一张“支持函数表”。
  ForEachSupported(CHECK_GL_SUPPORTED);

#undef CHECK_GL_SUPPORTED

  return matched;
}

static rdcstr Basename(const char *path)
{
  if(path == NULL || path[0] == 0)
    return "";

  const char *slash = strrchr(path, '/');
  if(slash)
    return slash + 1;

  return path;
}

static bool IsSelfModulePath(const char *path)
{
  if(path == NULL || path[0] == 0)
    return false;

  if(strstr(path, RENDERDOC_ANDROID_LIBRARY) != NULL)
    return true;

  if(g_SelfLibraryPath.empty())
  {
    FileIO::GetLibraryFilename(g_SelfLibraryPath);
    g_SelfLibraryBase = Basename(g_SelfLibraryPath.c_str());
  }

  if(!g_SelfLibraryPath.empty() && strstr(path, g_SelfLibraryPath.c_str()) != NULL)
    return true;

  if(!g_SelfLibraryBase.empty() && strstr(path, g_SelfLibraryBase.c_str()) != NULL)
    return true;

  return false;
}

// 判断给定函数名是否出现在 RenderDoc 的 EGL 支持列表中。
static bool IsEGLSupportedByRenderDoc(const char *name)
{
  if(name == NULL || name[0] == 0)
    return false;

  bool matched = false;

  // 已有 hook 实现的 EGL 函数（会有 *_renderdoc_hooked 包装）
#define CHECK_EGL_HOOKED(func, isext, replayrequired)                    \
  if(!matched && strcmp(name, "egl" STRINGIZE(func)) == 0)               \
    matched = true;

  EGL_HOOKED_SYMBOLS(CHECK_EGL_HOOKED)

#undef CHECK_EGL_HOOKED

//   // 非 hook 但在 dispatch 表中的 EGL 函数，根据需要也可视为“supported”
// #define CHECK_EGL_NONHOOKED(func, isext, replayrequired)                 \
//   if(!matched && strcmp(name, "egl" STRINGIZE(func)) == 0)               \
//     matched = true;

//   EGL_NONHOOKED_SYMBOLS(CHECK_EGL_NONHOOKED)

// #undef CHECK_EGL_NONHOOKED

  return matched;
}

static bool IsEGLCoreHook(const FunctionHook &hook)
{
  // 先收缩为最小 EGL 核心集合，验证稳定性：
  // - 上下文创建/切换：eglGetDisplay / eglCreateContext / eglMakeCurrent
  // - 交换缓冲：eglSwapBuffers
  // - 获取函数指针：eglGetProcAddress
  const char *name = hook.function.c_str();
  if(name == NULL || name[0] == 0)
    return false;

  // 先确认是 RenderDoc 已支持的 EGL 符号，避免对未知/内部入口做 inline hook。  
  if(!IsEGLSupportedByRenderDoc(name))
    return false;
  

  if(strcmp(name, "eglGetDisplay") == 0)
    return true;
  if(strcmp(name, "eglCreateContext") == 0)
    return true;
  if(strcmp(name, "eglMakeCurrent") == 0)
    return true;
  if(strcmp(name, "eglSwapBuffers") == 0)
    return true;
  if(strcmp(name, "eglGetProcAddress") == 0)
    return true;
  
  // Phase 1: surface / window lifecycle
  if(strcmp(name, "eglCreateWindowSurface") == 0)          return true;
  if(strcmp(name, "eglDestroySurface") == 0)               return true;
  if(strcmp(name, "eglCreatePlatformWindowSurface") == 0)  return true;
  if(strcmp(name, "eglSwapBuffersWithDamageEXT") == 0)     return true;
  if(strcmp(name, "eglSwapBuffersWithDamageKHR") == 0)     return true;
  if(strcmp(name, "eglPostSubBufferNV") == 0)              return true;

  // Phase 2: display / config / query
  if(strcmp(name, "eglInitialize") == 0)        return true;
  if(strcmp(name, "eglTerminate") == 0)         return true;
  if(strcmp(name, "eglBindAPI") == 0)           return true;
  if(strcmp(name, "eglGetConfigs") == 0)        return true;
  if(strcmp(name, "eglChooseConfig") == 0)      return true;
  if(strcmp(name, "eglGetConfigAttrib") == 0)   return true;
  if(strcmp(name, "eglQuerySurface") == 0)      return true;
  if(strcmp(name, "eglQueryContext") == 0)      return true;
  if(strcmp(name, "eglQueryString") == 0)       return true;

  return false;
}

// 选择性地对一小撮核心 GL 函数启用 Dobby inline hook。
// 函数名必须先在 ForEachSupported 列表中出现（RenderDoc 已有 hook 实现），
// 再根据这里的白名单决定是否交给 Dobby 处理。
static bool IsGLCoreHook(const FunctionHook &hook)
{
  const char *name = hook.function.c_str();
  if(name == NULL || name[0] == 0)
    return false;

  // 先确保是 RenderDoc 支持的 GL 函数，避免对未知/未包装的入口做 inline hook。
  if(!IsGLSupportedByRenderDoc(name))
    return false;

  // ============================================================
  // 四条链最小闭环白名单（Texture / Program / FBO / Buffer）
  // ============================================================

  // ----------------------------
  // Draw（事件入口）
  // ----------------------------
  // clear 事件（很多引擎每帧开头都会调用，影响帧边界与背景）
  if(strcmp(name, "glClear") == 0)
    return true;
  if(strcmp(name, "glClearColor") == 0)
    return true;
  if(strcmp(name, "glClearDepthf") == 0)
    return true;
  if(strcmp(name, "glClearDepth") == 0)
    return true;
  if(strcmp(name, "glClearStencil") == 0)
    return true;
  if(strcmp(name, "glColorMask") == 0)
    return true;
  if(strcmp(name, "glDepthMask") == 0)
    return true;
  if(strcmp(name, "glViewport") == 0)
    return true;
  if(strcmp(name, "glScissor") == 0)
    return true;

  // depth bias
  if(strcmp(name, "glPolygonOffset") == 0)
    return true;

  // stencil state（不少引擎会用 Separate 变体）
  if(strcmp(name, "glStencilMask") == 0)
    return true;
  if(strcmp(name, "glStencilOpSeparate") == 0)
    return true;
  if(strcmp(name, "glStencilFuncSeparate") == 0)
    return true;

  // fixed-function state（blend/depth/stencil enable 等）
  // 这类状态如果缺失，可能导致事件的输出与资源绑定推断不一致（尤其是 FX pass）
  if(strcmp(name, "glEnable") == 0)
    return true;
  if(strcmp(name, "glDisable") == 0)
    return true;
  if(strcmp(name, "glBlendFuncSeparate") == 0)
    return true;
  if(strcmp(name, "glBlendEquationSeparate") == 0)
    return true;
  if(strcmp(name, "glDepthFunc") == 0)
    return true;

  if(strcmp(name, "glDrawElements") == 0)
    return true;
  if(strcmp(name, "glDrawArrays") == 0)
    return true;
  if(strcmp(name, "glDrawElementsBaseVertex") == 0)
    return true;
  if(strcmp(name, "glDrawElementsInstanced") == 0)
    return true;
  if(strcmp(name, "glDrawArraysInstanced") == 0)
    return true;
  // instancing 变体（GLES3.1+ 常见）
  if(strcmp(name, "glDrawElementsInstancedBaseVertex") == 0)
    return true;
  if(strcmp(name, "glDrawElementsInstancedBaseInstance") == 0)
    return true;
  if(strcmp(name, "glDrawArraysInstancedBaseInstance") == 0)
    return true;
  if(strcmp(name, "glDrawElementsInstancedBaseVertexBaseInstance") == 0)
    return true;

  // ----------------------------
  // Texture lifecycle（闭环）
  // ----------------------------
  if(strcmp(name, "glGenTextures") == 0)
    return true;
  if(strcmp(name, "glDeleteTextures") == 0)
    return true;
  if(strcmp(name, "glActiveTexture") == 0)
    return true;
  if(strcmp(name, "glBindTexture") == 0)
    return true;

  // 像素存储状态会影响 tex upload（row alignment/stride），缺失会导致部分贴图解析异常
  if(strcmp(name, "glPixelStorei") == 0)
    return true;
  if(strcmp(name, "glPixelStoref") == 0)
    return true;

  if(strcmp(name, "glTexImage2D") == 0)
    return true;
  if(strcmp(name, "glTexSubImage2D") == 0)
    return true;
  if(strcmp(name, "glTexStorage2D") == 0)
    return true;
  if(strcmp(name, "glCompressedTexImage2D") == 0)
    return true;
  if(strcmp(name, "glCompressedTexSubImage2D") == 0)
    return true;
  if(strcmp(name, "glGenerateMipmap") == 0)
    return true;

  // 3D / array 纹理（Unity/GLES3 常见：volume/texture array）
  if(strcmp(name, "glTexImage3D") == 0)
    return true;
  if(strcmp(name, "glTexSubImage3D") == 0)
    return true;
  if(strcmp(name, "glTexStorage3D") == 0)
    return true;
  if(strcmp(name, "glCompressedTexImage3D") == 0)
    return true;
  if(strcmp(name, "glCompressedTexSubImage3D") == 0)
    return true;

  // 多重采样纹理/附件（MSAA RT 经常走这条）
  if(strcmp(name, "glTexImage2DMultisample") == 0)
    return true;
  if(strcmp(name, "glTexImage3DMultisample") == 0)
    return true;
  if(strcmp(name, "glTexStorage2DMultisample") == 0)
    return true;
  if(strcmp(name, "glTexStorage3DMultisample") == 0)
    return true;
  if(strcmp(name, "glTexStorage3DMultisampleOES") == 0)
    return true;

  // Copy/Blit 到纹理（部分后处理/截图路径会用）
  if(strcmp(name, "glCopyTexImage2D") == 0)
    return true;
  if(strcmp(name, "glCopyTexSubImage2D") == 0)
    return true;
  if(strcmp(name, "glCopyTexSubImage3D") == 0)
    return true;
  if(strcmp(name, "glCopyTexSubImage3DOES") == 0)
    return true;
  if(strcmp(name, "glCopyImageSubData") == 0)
    return true;
  if(strcmp(name, "glCopyImageSubDataEXT") == 0)
    return true;
  if(strcmp(name, "glCopyImageSubDataOES") == 0)
    return true;

  // buffer texture / view（部分驱动/引擎会用来做资源别名）
  if(strcmp(name, "glTexBuffer") == 0)
    return true;
  if(strcmp(name, "glTexBufferRange") == 0)
    return true;
  if(strcmp(name, "glTextureView") == 0)
    return true;
  if(strcmp(name, "glTextureViewOES") == 0)
    return true;
  if(strcmp(name, "glTextureViewEXT") == 0)
    return true;

  if(strcmp(name, "glTexParameteri") == 0)
    return true;
  if(strcmp(name, "glTexParameterf") == 0)
    return true;
  if(strcmp(name, "glTexParameteriv") == 0)
    return true;
  if(strcmp(name, "glTexParameterfv") == 0)
    return true;
  if(strcmp(name, "glTexParameterIiv") == 0)
    return true;
  if(strcmp(name, "glTexParameterIuiv") == 0)
    return true;
  if(strcmp(name, "glTexParameterIivEXT") == 0)
    return true;
  if(strcmp(name, "glTexParameterIuivEXT") == 0)
    return true;
  if(strcmp(name, "glTexParameterIivOES") == 0)
    return true;
  if(strcmp(name, "glTexParameterIuivOES") == 0)
    return true;

  // Sampler objects（Unity 有时会用 sampler 分离状态）
  if(strcmp(name, "glGenSamplers") == 0)
    return true;
  if(strcmp(name, "glDeleteSamplers") == 0)
    return true;
  if(strcmp(name, "glBindSampler") == 0)
    return true;
  if(strcmp(name, "glSamplerParameteri") == 0)
    return true;
  if(strcmp(name, "glSamplerParameterf") == 0)
    return true;
  if(strcmp(name, "glSamplerParameteriv") == 0)
    return true;
  if(strcmp(name, "glSamplerParameterfv") == 0)
    return true;

  // Android 外部纹理 / EGLImage
  if(strcmp(name, "glEGLImageTargetTexture2DOES") == 0)
    return true;
  if(strcmp(name, "glEGLImageTargetRenderbufferStorageOES") == 0)
    return true;

  // ----------------------------
  // Program / Shader（传统 GLSL）
  // ----------------------------
  if(strcmp(name, "glCreateShader") == 0)
    return true;
  if(strcmp(name, "glShaderSource") == 0)
    return true;
  if(strcmp(name, "glCompileShader") == 0)
    return true;
  if(strcmp(name, "glDeleteShader") == 0)
    return true;

  if(strcmp(name, "glCreateProgram") == 0)
    return true;
  if(strcmp(name, "glAttachShader") == 0)
    return true;
  if(strcmp(name, "glLinkProgram") == 0)
    return true;
  if(strcmp(name, "glUseProgram") == 0)
    return true;
  if(strcmp(name, "glDeleteProgram") == 0)
    return true;

  // Uniform updates（材质参数/采样器绑定/矩阵等，缺失会导致贴图或 shader 状态不完整）
  if(strcmp(name, "glGetUniformLocation") == 0)
    return true;

  // sampler 常用
  if(strcmp(name, "glUniform1i") == 0)
    return true;
  if(strcmp(name, "glUniform1iv") == 0)
    return true;

  // 标量/向量（常见）
  if(strcmp(name, "glUniform1f") == 0)
    return true;
  if(strcmp(name, "glUniform2f") == 0)
    return true;
  if(strcmp(name, "glUniform3f") == 0)
    return true;
  if(strcmp(name, "glUniform4f") == 0)
    return true;
  if(strcmp(name, "glUniform1fv") == 0)
    return true;
  if(strcmp(name, "glUniform2fv") == 0)
    return true;
  if(strcmp(name, "glUniform3fv") == 0)
    return true;
  if(strcmp(name, "glUniform4fv") == 0)
    return true;

  // integer uniforms
  if(strcmp(name, "glUniform1ui") == 0)
    return true;
  if(strcmp(name, "glUniform1uiv") == 0)
    return true;
  if(strcmp(name, "glUniform2i") == 0)
    return true;
  if(strcmp(name, "glUniform3i") == 0)
    return true;
  if(strcmp(name, "glUniform4i") == 0)
    return true;
  if(strcmp(name, "glUniform2iv") == 0)
    return true;
  if(strcmp(name, "glUniform3iv") == 0)
    return true;
  if(strcmp(name, "glUniform4iv") == 0)
    return true;
  if(strcmp(name, "glUniform2ui") == 0)
    return true;
  if(strcmp(name, "glUniform3ui") == 0)
    return true;
  if(strcmp(name, "glUniform4ui") == 0)
    return true;
  if(strcmp(name, "glUniform2uiv") == 0)
    return true;
  if(strcmp(name, "glUniform3uiv") == 0)
    return true;
  if(strcmp(name, "glUniform4uiv") == 0)
    return true;

  // 矩阵（transform/骨骼/UV 变换）
  if(strcmp(name, "glUniformMatrix2fv") == 0)
    return true;
  if(strcmp(name, "glUniformMatrix3fv") == 0)
    return true;
  if(strcmp(name, "glUniformMatrix4fv") == 0)
    return true;
  if(strcmp(name, "glUniformMatrix2x3fv") == 0)
    return true;
  if(strcmp(name, "glUniformMatrix3x2fv") == 0)
    return true;
  if(strcmp(name, "glUniformMatrix2x4fv") == 0)
    return true;
  if(strcmp(name, "glUniformMatrix4x2fv") == 0)
    return true;
  if(strcmp(name, "glUniformMatrix3x4fv") == 0)
    return true;
  if(strcmp(name, "glUniformMatrix4x3fv") == 0)
    return true;

  // Program binary / pipeline（Unity 可能走）
  if(strcmp(name, "glProgramBinary") == 0)
    return true;
  if(strcmp(name, "glGetProgramBinary") == 0)
    return true;
  if(strcmp(name, "glBindProgramPipeline") == 0)
    return true;
  if(strcmp(name, "glUseProgramStages") == 0)
    return true;
  if(strcmp(name, "glCreateShaderProgramv") == 0)
    return true;
  if(strcmp(name, "glCreateShaderProgramvEXT") == 0)
    return true;
  if(strcmp(name, "glProgramParameteri") == 0)
    return true;

  // ----------------------------
  // FBO / RBO（闭环）
  // ----------------------------
  if(strcmp(name, "glGenFramebuffers") == 0)
    return true;
  if(strcmp(name, "glDeleteFramebuffers") == 0)
    return true;
  if(strcmp(name, "glBindFramebuffer") == 0)
    return true;
  // tile-based GPU 常用 discard/invalidations（影响附件生命周期与脏资源判定）
  if(strcmp(name, "glInvalidateFramebuffer") == 0)
    return true;
  if(strcmp(name, "glInvalidateSubFramebuffer") == 0)
    return true;
  if(strcmp(name, "glFramebufferTexture2D") == 0)
    return true;
  if(strcmp(name, "glFramebufferRenderbuffer") == 0)
    return true;
  if(strcmp(name, "glCheckFramebufferStatus") == 0)
    return true;

  if(strcmp(name, "glGenRenderbuffers") == 0)
    return true;
  if(strcmp(name, "glDeleteRenderbuffers") == 0)
    return true;
  if(strcmp(name, "glBindRenderbuffer") == 0)
    return true;
  if(strcmp(name, "glRenderbufferStorage") == 0)
    return true;
  if(strcmp(name, "glRenderbufferStorageMultisample") == 0)
    return true;

  // ----------------------------
  // Buffer + Vertex input（闭环）
  // ----------------------------
  if(strcmp(name, "glGenBuffers") == 0)
    return true;
  if(strcmp(name, "glDeleteBuffers") == 0)
    return true;
  if(strcmp(name, "glBindBuffer") == 0)
    return true;
  if(strcmp(name, "glBufferData") == 0)
    return true;
  if(strcmp(name, "glBufferSubData") == 0)
    return true;

  // 顶点输入
  if(strcmp(name, "glVertexAttribPointer") == 0)
    return true;
  if(strcmp(name, "glEnableVertexAttribArray") == 0)
    return true;
  if(strcmp(name, "glDisableVertexAttribArray") == 0)
    return true;
  // instancing attribute divisor（核心）
  if(strcmp(name, "glVertexAttribDivisor") == 0)
    return true;
  if(strcmp(name, "glVertexAttribDivisorEXT") == 0)
    return true;
  if(strcmp(name, "glVertexAttribDivisorANGLE") == 0)
    return true;

  // VAO（GLES3 常用）
  if(strcmp(name, "glGenVertexArrays") == 0)
    return true;
  if(strcmp(name, "glBindVertexArray") == 0)
    return true;
  if(strcmp(name, "glDeleteVertexArrays") == 0)
    return true;

  // 映射（常见更新路径）
  if(strcmp(name, "glMapBufferRange") == 0)
    return true;
  if(strcmp(name, "glFlushMappedBufferRange") == 0)
    return true;
  if(strcmp(name, "glUnmapBuffer") == 0)
    return true;

  // UBO/SSBO 绑定（让 shader 资源更完整）
  if(strcmp(name, "glBindBufferBase") == 0)
    return true;
  if(strcmp(name, "glBindBufferRange") == 0)
    return true;
  if(strcmp(name, "glUniformBlockBinding") == 0)
    return true;

  return false;
}

// 仅对少量关键符号打印日志，避免遍历数千 hook 时刷爆 logcat。
static bool ShouldLogDobbySymbol(const char *name)
{
  if(name == NULL || name[0] == 0)
    return false;

  // EGL 关键入口
  if(strcmp(name, "eglGetProcAddress") == 0)
    return true;
  if(strcmp(name, "eglSwapBuffers") == 0)
    return true;
  if(strcmp(name, "eglMakeCurrent") == 0)
    return true;

  // 传统 shader/program 路径
  if(strcmp(name, "glCreateShader") == 0)
    return true;
  if(strcmp(name, "glShaderSource") == 0)
    return true;
  if(strcmp(name, "glCompileShader") == 0)
    return true;
  if(strcmp(name, "glCreateProgram") == 0)
    return true;
  if(strcmp(name, "glAttachShader") == 0)
    return true;
  if(strcmp(name, "glLinkProgram") == 0)
    return true;
  if(strcmp(name, "glUseProgram") == 0)
    return true;

  // program binary / pipeline 路径（Unity 可能走这条）
  if(strcmp(name, "glProgramBinary") == 0)
    return true;
  if(strcmp(name, "glGetProgramBinary") == 0)
    return true;
  if(strcmp(name, "glBindProgramPipeline") == 0)
    return true;
  if(strcmp(name, "glUseProgramStages") == 0)
    return true;
  if(strcmp(name, "glCreateShaderProgramv") == 0)
    return true;
  if(strcmp(name, "glCreateShaderProgramvEXT") == 0)
    return true;

  // 纹理上传/存储（纹理资源缺失排查）
  if(strcmp(name, "glBindTexture") == 0)
    return true;
  if(strcmp(name, "glTexImage2D") == 0)
    return true;
  if(strcmp(name, "glTexSubImage2D") == 0)
    return true;
  if(strcmp(name, "glTexStorage2D") == 0)
    return true;
  if(strcmp(name, "glCompressedTexImage2D") == 0)
    return true;
  if(strcmp(name, "glGenerateMipmap") == 0)
    return true;

  // EGLImage / external texture（Android 常见资源路径）
  if(strcmp(name, "glEGLImageTargetTexture2DOES") == 0)
    return true;
  if(strcmp(name, "glEGLImageTargetRenderbufferStorageOES") == 0)
    return true;

  // draw（确认 draw 是否进入 renderdoc wrapper）
  if(strcmp(name, "glDrawElements") == 0)
    return true;
  if(strcmp(name, "glDrawArrays") == 0)
    return true;

  return false;
}

static bool DobbyHookOneSymbol(void *handle, const FunctionHook &hook, const char *owner)
{
  const char *name = hook.function.c_str();

  // RDCLOG("DobbyHook candidate: %s (owner=%s)", name, owner ? owner : "(null)");
  // 仅对白名单中的 EGL/GL 函数做 Dobby inline hook，避免一次性 hook 全 GL/GLES 导致黑屏/崩溃。
  if(!IsEGLCoreHook(hook) && !IsGLCoreHook(hook))
  {
    RDCLOG("Skip non-core EGL/GL symbol: %s", name);
    return false;
  }

  if(handle == NULL || hook.hook == NULL || hook.function.empty())
    return false;

  void *target = xdl_sym(handle, name, NULL);
  if(target == NULL)
  {
    RDCLOG("DobbyHook skip: %s not found in %s", name, owner ? owner : "(null)");
    return false;
  }

  {
    SCOPED_LOCK(g_DobbyHookLock);
    if(g_DobbyHookedTargets.find(target) != g_DobbyHookedTargets.end())
    {
      if(hook.orig && *hook.orig == NULL)
      {
        auto it = g_DobbyOrigByName.find(hook.function);
        if(it != g_DobbyOrigByName.end())
          *hook.orig = it->second;
      }
      return true;
    }
  }

  Dl_info info = {};
  if(dladdr(target, &info) == 0 || info.dli_fname == NULL)
  {
    RDCWARN("Skip Dobby hook for %s (owner=%s): dladdr failed for target=%p", name,
            owner ? owner : "(null)", target);
    return false;
  }

  // 关键保护：禁止对 renderdoc 自身导出符号做 inline hook。
  // 若命中 self-export（例如 eglSwapBuffers wrapper），会形成递归并在渲染线程栈爆崩溃。
  if(IsSelfModulePath(info.dli_fname))
  {
    RDCWARN("Skip self-export symbol hook: %s owner=%s target=%p module=%s", name,
            owner ? owner : "(null)", target, info.dli_fname);
    return false;
  }

  void *trampoline = NULL;
  int rc = DobbyHook(target, hook.hook, (dobby_dummy_func_t *)&trampoline);
  if(rc != 0)
  {
    RDCERR("DobbyHook failed: %s in %s target=%p hook=%p rc=%d", name, owner, target, hook.hook, rc);
    return false;
  }

  {
    SCOPED_LOCK(g_DobbyHookLock);
    g_DobbyHookedTargets.insert(target);
    if(trampoline && g_DobbyOrigByName.find(hook.function) == g_DobbyOrigByName.end())
      g_DobbyOrigByName[hook.function] = trampoline;
  }

  if(hook.orig && *hook.orig == NULL)
    *hook.orig = trampoline;

  RDCLOG("DobbyHook success: %s in %s target=%p hook=%p tramp=%p", name, owner, target, hook.hook,
         trampoline);
  return true;
}

static void ApplyDobbyHooksForHandle(void *handle, const char *owner)
{
  if(handle == NULL)
    return;

  rdcarray<FunctionHook> hooks = GetHookInfo().GetFunctionHooks();
  for(const FunctionHook &hook : hooks)
    DobbyHookOneSymbol(handle, hook, owner);
}

static void ApplyDobbyHooksForPath(const char *path)
{
  if(path == NULL || path[0] == 0)
    return;

  // 使用 TRY_FORCE_LOAD 获取真实系统库句柄，避免仅命中重定向句柄。
  void *handle = xdl_open(path, XDL_TRY_FORCE_LOAD);
  if(handle == NULL)
  {
    HOOK_DEBUG_PRINT("xdl_open failed for %s", path);
    return;
  }

  ApplyDobbyHooksForHandle(handle, path);
  xdl_close(handle);
}

static void ApplyDobbyHooksForRegisteredLibraries()
{
  rdcarray<rdcstr> libs = GetHookInfo().GetLibHooks();
  for(const rdcstr &lib : libs)
    ApplyDobbyHooksForPath(lib.c_str());
}
#endif

void *intercept_dlopen(const char *filename, int flag)
{
  if(filename)
  {
#if defined(RENDERDOC_HAVE_DOBBY)
    // Dobby 路径下，主劫持依赖 inline hook，不再把 libEGL/libGLES 的 dlopen 重定向到 ourselves。
    // 否则 dlsym(handle, "egl*") 容易优先命中 renderdoc 导出 wrapper，增加 self-hook 递归风险。
    if(strstr(filename, RENDERDOC_ANDROID_LIBRARY))
    {
      HOOK_DEBUG_PRINT("Intercepting self dlopen for %s", filename);
      return dlopen(RENDERDOC_ANDROID_LIBRARY, flag);
    }
#else
    // if this is a library we're hooking, or a request for our own library in any form, return our
    // own library.
    // We need to intercept requests for our own library, because the android loader makes the
    // completely ridiculous decision to load multiple copies of the same library into a process if
    // it's dlopen'd with different paths. This obviously breaks with our hook install.
    if(strstr(filename, RENDERDOC_ANDROID_LIBRARY) || GetHookInfo().IsLibHook(rdcstr(filename)))
    {
      HOOK_DEBUG_PRINT("Intercepting dlopen for %s", filename);
      RDCLOG("WEN: dlopen %s  ", rdcstr(RENDERDOC_ANDROID_LIBRARY).c_str());
      return dlopen(RENDERDOC_ANDROID_LIBRARY, flag);
    }
#endif
  }

  return NULL;
}

// we need this on both paths since interceptor-lib is unable to hook dlopen in libvulkan.so
static int dl_iterate_callback(struct dl_phdr_info *info, size_t size, void *data)
{
  if(info->dlpi_name == NULL)
  {
    HOOK_DEBUG_PRINT("Skipping NULL entry!");
    return 0;
  }
  rdcstr soname = info->dlpi_name;

  if(GetHookInfo().IsHooked(soname))
    return 0;

  HOOK_DEBUG_PRINT("Hooking %s", soname.c_str());
  GetHookInfo().SetHooked(soname);

  for(int ph = 0; ph < info->dlpi_phnum; ph++)
  {
    if(info->dlpi_phdr[ph].p_type != PT_DYNAMIC)
      continue;

    ElfW(Dyn) *dynamic = (ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[ph].p_vaddr);

    ElfW(Sym) *dynsym = NULL;
    const char *strtab = NULL;
    size_t strtabcount = 0;
    Elf_Rel *pltbase = NULL;
    ElfW(Sword) pltcount = 0;

    while(dynamic->d_tag != DT_NULL)
    {
      if(dynamic->d_tag == DT_SYMTAB)
        dynsym = (ElfW(Sym) *)(info->dlpi_addr + dynamic->d_un.d_ptr);
      else if(dynamic->d_tag == DT_STRTAB)
        strtab = (const char *)(info->dlpi_addr + dynamic->d_un.d_ptr);
      else if(dynamic->d_tag == DT_STRSZ)
        strtabcount = dynamic->d_un.d_val;
      else if(dynamic->d_tag == DT_JMPREL)
        pltbase = (Elf_Rel *)(info->dlpi_addr + dynamic->d_un.d_ptr);
      else if(dynamic->d_tag == DT_PLTRELSZ)
        pltcount = dynamic->d_un.d_val / sizeof(Elf_Rel);

      /*
      if(dynamic->d_tag == DT_NEEDED)
        HOOK_DEBUG_PRINT("NEEDED [%i, %s]", dynamic->d_un.d_val, strtab + dynamic->d_un.d_val);
        */

      dynamic++;
    }

    if(!dynsym || !strtab || !pltbase || pltcount == 0 || strtabcount == 0)
    {
      RDCWARN("Missing required section to hook %s", info->dlpi_name);
      continue;
    }

    void **relro_base = NULL;
    void **relro_end = NULL;
    bool relro_failed = false;

    FILE *f = FileIO::fopen(info->dlpi_name, FileIO::ReadText);

    // read the file on disk to get the .relro section
    if(f)
    {
      ElfW(Ehdr) ehdr;
      size_t read = FileIO::fread(&ehdr, sizeof(ehdr), 1, f);

      if(read == 1 && ehdr.e_ident[0] == ELFMAG0 && ehdr.e_ident[1] == 'E' &&
         ehdr.e_ident[2] == 'L' && ehdr.e_ident[3] == 'F')
      {
        FileIO::fseek64(f, ehdr.e_phoff, SEEK_SET);
        for(ElfW(Half) idx = 0; idx < ehdr.e_phnum; idx++)
        {
          ElfW(Phdr) phdr;
          read = FileIO::fread(&phdr, sizeof(phdr), 1, f);
          if(read != 1)
          {
            RDCWARN("Failed reading section");
            break;
          }

          if(phdr.p_type == PT_GNU_RELRO)
          {
            relro_base = (void **)(info->dlpi_addr + phdr.p_vaddr);
            relro_end = (void **)(info->dlpi_addr + phdr.p_vaddr + phdr.p_memsz);
          }
        }
      }
      else
      {
        RDCWARN("Didn't get valid ELF header");
      }

      FileIO::fclose(f);
    }
    else
    {
      RDCWARN("Couldn't open '%s' to look for relro!", info->dlpi_name);
      relro_failed = true;
    }

    if(relro_base)
      HOOK_DEBUG_PRINT("Got relro %p -> %p", relro_base, relro_end);
    HOOK_DEBUG_PRINT("Got %i PLT entries", pltcount);

    int pagesize = sysconf(_SC_PAGE_SIZE);

    for(ElfW(Sword) i = 0; i < pltcount; i++)
    {
      Elf_Rel *plt = pltbase + i;
      if(ELF_R_TYPE(plt->r_info) != R_JUMP_SLOT)
      {
        HOOK_DEBUG_PRINT("[%i]: Mismatched type %i vs %i", i, ELF_R_TYPE(plt->r_info), R_JUMP_SLOT);
        continue;
      }

      size_t idx = ELF_R_SYM(plt->r_info);
      size_t name = dynsym[idx].st_name;
      if(name + 1 > strtabcount)
      {
        HOOK_DEBUG_PRINT("[%i] name out of boundstoo big section header string table index: %zu", i,
                         name);
        continue;
      }

      const char *importname = strtab + name;
      void **import = (void **)(info->dlpi_addr + plt->r_offset);

      HOOK_DEBUG_PRINT("[%i] %s at %p (ptr to %p)", i, importname, import, *import);

      const FunctionHook repl = GetHookInfo().GetFunctionHook(importname);
      if(repl.hook)
      {
        HOOK_DEBUG_PRINT("replacing %s!", importname);

        uintptr_t pagebase = 0;

        if(relro_failed || (relro_base <= import && import <= relro_end))
        {
          if(relro_failed)
            HOOK_DEBUG_PRINT("Couldn't get relro sections - mapping read/write");
          else
            HOOK_DEBUG_PRINT("In relro range - %p <= %p <= %p", relro_base, import, relro_end);
          pagebase = uintptr_t(import) & ~(pagesize - 1);

          int ret = mprotect((void *)pagebase, pagesize, PROT_READ | PROT_WRITE);
          if(ret != 0)
          {
            RDCERR("Couldn't read/write the page: %d %d", ret, errno);
            return 0;
          }

          HOOK_DEBUG_PRINT("Marked page read/write");
        }
        else
        {
          HOOK_DEBUG_PRINT("Not in relro! - %p vs %p vs %p", relro_base, import, relro_end);
        }

        // note we don't save the orig function here, since we want to apply our library priorities
        // and we don't know what order these headers will be iterated in. See EndHookRegistration
        // for where we iterate and fetch all the function pointers we want.
        *import = repl.hook;

        if(pagebase)
        {
          if(relro_failed)
          {
            HOOK_DEBUG_PRINT(
                "Couldn't find relro sections - being conservative and leaving read-write");
          }
          else
          {
            HOOK_DEBUG_PRINT("Moving back to read-only");
            mprotect((void *)pagebase, pagesize, PROT_READ);
          }
        }

        HOOK_DEBUG_PRINT("[%i*] %s at %p (ptr to %p)", i, importname, import, *import);
      }
    }
  }

  return 0;
}

// android has a special dlopen that passes the caller address in.
typedef void *(*pfn__loader_dlopen)(const char *filename, int flags, const void *caller_addr);

typedef void *(*pfnandroid_dlopen_ext)(const char *__filename, int __flags,
                                       const android_dlextinfo *__info);

pfnandroid_dlopen_ext real_android_dlopen_ext = NULL;

pfn__loader_dlopen loader_dlopen = NULL;
uint64_t suppressTLS = 0;

void process_dlopen(const char *filename, int flag)
{
#if defined(RENDERDOC_HAVE_DOBBY)
  (void)flag;
  if(filename == NULL)
    return;

  // Dobby 模式下，EGL/GL 类库一旦加载就立刻应用 inline hook，确保从创建
  // EGLDisplay/EGLContext/EGLSurface 开始就被 RenderDoc 追踪，避免“晚接管”
  // 导致内部状态缺失。
  ApplyDobbyHooksForPath(filename);
  return;
#else
  if(filename && !GetHookInfo().IsHooked(rdcstr(filename)))
  {
    HOOK_DEBUG_PRINT("iterating after %s", filename);
    dl_iterate_phdr(dl_iterate_callback, NULL);
    GetHookInfo().SetHooked(filename);
  }
  else
  {
    HOOK_DEBUG_PRINT("Ignoring");
  }
#endif
}

extern "C" __attribute__((visibility("default"))) void *hooked_loader_dlopen(
    const char *filename, int flag, const void *caller_addr)
{
  HOOK_DEBUG_PRINT("hooked_loader_dlopen for %s | %d", filename, flag);

  void *ret = intercept_dlopen(filename, flag);
  if(ret)
    return ret;

  if(loader_dlopen == NULL)
  {
    RDCERR("loader_dlopen trampoline is NULL");
    return NULL;
  }

  ret = loader_dlopen(filename, flag, caller_addr);

  if(filename && ret)
    process_dlopen(filename, flag);

  return ret;
}

extern "C" __attribute__((visibility("default"))) void *hooked_dlopen(const char *filename, int flag)
{
  // get caller address immediately.
  const void *caller_addr = __builtin_return_address(0);
  RDCLOG("WEN: 本地hooked_dlopen");// 替换系统原本的'dlopen',当系统调用dlopen时候，实际执行的是这个钩子函数
  RDCLOG("WEN: hooked_dlopen for %s | %d", filename, flag);// 替换系统原本的'dlopen',当系统调用dlopen时候，实际执行的是这个钩子函数
  HOOK_DEBUG_PRINT("hooked_dlopen for %s | %d", filename, flag);

  // 1. 检查是否需要拦截（如替换为 RenderDoc 自身库）
  void *ret = intercept_dlopen(filename, flag);

  // if we intercepted, return immediately
  if(ret)
    return ret;
  // 2. 调用原始 dlopen 加载目标库
  ret = loader_dlopen(filename, flag, caller_addr);
  HOOK_DEBUG_PRINT("Got %p", ret);
  // 3. 处理新加载的库（如修改 PLT/GOT 表）
  if(filename && ret)
    process_dlopen(filename, flag);

  return ret;
}

extern "C" __attribute__((visibility("default"))) void *hooked_android_dlopen_ext(
    const char *__filename, int __flags, const android_dlextinfo *__info)
{
  RDCLOG("WEN: hooked_android_dlopen_ext for %s | %d", __filename, __flags);
  HOOK_DEBUG_PRINT("hooked_android_dlopen_ext for %s | %d", __filename, __flags);

  void *ret = intercept_dlopen(__filename, __flags);

  // if we intercepted, return immediately
  if(ret)
    return ret;

  // otherwise return the 'real' result.
  if(real_android_dlopen_ext != NULL)
    ret = real_android_dlopen_ext(__filename, __flags, __info);
  else
    ret = android_dlopen_ext(__filename, __flags, __info);
  HOOK_DEBUG_PRINT("Got %p", ret);

  if(__filename && ret)
    process_dlopen(__filename, __flags);

  return ret;
}

bool hooks_suppressed();

extern "C" __attribute__((visibility("default"))) void *hooked_dlsym(void *handle, const char *symbol)
{
  if(handle == NULL || symbol == NULL || hooks_suppressed())
    return dlsym(handle, symbol);

  const FunctionHook repl = GetHookInfo().GetFunctionHook(symbol);

  if(repl.hook == NULL)
    return dlsym(handle, symbol);

  if(!GetHookInfo().IsHooked(handle))
  {
    dl_iterate_phdr(dl_iterate_callback, NULL);
    GetHookInfo().SetHooked(handle);
  }

  HOOK_DEBUG_PRINT("Got dlsym for %s which we want in %p...", symbol, handle);

  if(GetHookInfo().IsLibHook(handle))
  {
    HOOK_DEBUG_PRINT("identified dlsym(%s) we want to interpose! returning %p", symbol, repl.hook);
    return repl.hook;
  }

  void *ret = dlsym(handle, symbol);
  Dl_info info = {};
  dladdr(ret, &info);
  HOOK_DEBUG_PRINT("real ret is %p in %s", ret, info.dli_fname);
  return ret;
}

static void InstallHooksCommon()
{
  suppressTLS = Threading::AllocateTLSSlot();

  // blacklist hooking certain system libraries or ourselves
  GetHookInfo().SetHooked(RENDERDOC_ANDROID_LIBRARY);
  GetHookInfo().SetHooked("libc.so");
  GetHookInfo().SetHooked("libvndksupport.so");

  real_android_dlopen_ext = &android_dlopen_ext;

  loader_dlopen = (pfn__loader_dlopen)dlsym(RTLD_NEXT, "__loader_dlopen");

#if defined(RENDERDOC_HAVE_DOBBY)
  // 为了验证 EGL/GL Dobby inline hook 的稳定性，暂时不对 linker/bionic 的
  // __loader_dlopen / android_dlopen_ext 入口做 inline hook，避免在 early init
  // 阶段 patch 到不安全区域导致 SIGILL。此时 Dobby 只会在 EndHookRegistration/
  // Refresh 阶段对已加载的 libEGL/libGLES 做一次批量 hook。
  if(loader_dlopen)
  {
    RDCLOG("Dobby loader hooks temporarily disabled (have __loader_dlopen=%p)", loader_dlopen);
  }
  else
  {
    RDCWARN("Couldn't find __loader_dlopen for Dobby hook (loader hooks disabled)");
  }
#else
  if(loader_dlopen)
  {
    RDCLOG("钩子注册： 将 dlopen 的符号指向 android_hook的 hooked_dlopen。");
    LibraryHooks::RegisterFunctionHook("", FunctionHook("dlopen", NULL, (void *)&hooked_dlopen));    
  }
  else
  {
    RDCWARN("Couldn't find __loader_dlopen, falling back to slow path for dlopen hooking");
    LibraryHooks::RegisterFunctionHook("", FunctionHook("dlsym", NULL, (void *)&hooked_dlsym));
  }

  LibraryHooks::RegisterFunctionHook(
      "", FunctionHook("android_dlopen_ext", NULL, (void *)&hooked_android_dlopen_ext));
#endif
}

#if defined(RENDERDOC_HAVE_INTERCEPTOR_LIB)

void intercept_error(void *, const char *error_msg)
{
  RDCERR("intercept_error: %s", error_msg);
}

#include "interceptor-lib/include/interceptor.h"

void PatchHookedFunctions()
{
  RDCLOG("Applying hooks with interceptor-lib");

// see below - Huawei workaround
#if defined(__LP64__)
  LibraryHooks::RegisterLibraryHook("/system/lib64/libhwgl.so", NULL);
#else
  LibraryHooks::RegisterLibraryHook("/system/lib/libhwgl.so", NULL);
#endif

  rdcarray<rdcstr> libs = GetHookInfo().GetLibHooks();
  rdcarray<FunctionHook> funchooks = GetHookInfo().GetFunctionHooks();

  // we just leak this
  void *intercept = InitializeInterceptor();

  std::set<rdcstr> fallbacklibs;
  std::set<FunctionHook> fallbackhooks;

  for(const rdcstr &lib : libs)
  {
    void *handle = dlopen(lib.c_str(), RTLD_NOW);

    bool huawei = lib.contains("libhwgl.so");

    if(!handle)
    {
      HOOK_DEBUG_PRINT("Didn't get handle for %s", lib.c_str());
      continue;
    }

    HOOK_DEBUG_PRINT("Hooking %s = %p", lib.c_str(), handle);

    std::set<void *> foundfunctions;

    for(const FunctionHook &hook : funchooks)
    {
      void *oldfunc = dlsym(handle, hook.function.c_str());

      // UNTESTED workaround taken directly from GAPID, in installer.cpp. Quoted comment:
      /*
            // Huawei implements all functions in this library with prefix,
            // all GL functions in libGLES*.so are just trampolines to his.
            // However, we do not support trampoline interception for now,
            // so try to intercept the internal implementation instead.
      */
      if(huawei && oldfunc == NULL)
        oldfunc = dlsym(handle, ("hw_" + hook.function).c_str());

      if(GetHookInfo().IsHooked(oldfunc))
        continue;

      if(!oldfunc)
      {
        HOOK_DEBUG_PRINT("%s didn't have %s", lib.c_str(), hook.function.c_str());
        continue;
      }

      HOOK_DEBUG_PRINT("Hooking %s::%s = %p with %p", lib.c_str(), hook.function.c_str(), oldfunc,
                       hook.hook);

      void *trampoline = NULL;

      bool success = InterceptFunction(intercept, oldfunc, hook.hook, &trampoline, &intercept_error);

      if(!hook.orig)
        RDCWARN("No original pointer for hook of '%s' - trampoline will be lost!",
                hook.function.c_str());

      if(hook.orig && *hook.orig == NULL)
        *hook.orig = trampoline;

      if(success)
      {
        HOOK_DEBUG_PRINT("Hooked successfully, trampoline is %p", trampoline);
      }
      else
      {
        RDCERR("Failed to hook %s::%s!", lib.c_str(), hook.function.c_str());
        fallbacklibs.insert(lib);
        fallbackhooks.insert(hook);
      }

      GetHookInfo().SetHooked(oldfunc);
    }
  }

  // we still need to hook android_dlopen_ext with interceptor-lib so that we can intercept the
  // vulkan loader's attempts to load our library and prevent it from loading a second copy (!!)
  // into the process.
  // Unfortunately, interceptor-lib can't hook this function so we need to set up the PLT hooking.
  // This is just a minimal setup to intercept that one function.
  GetHookInfo().ClearHooks();

  for(const rdcstr &l : fallbacklibs)
  {
    RDCLOG("Falling back to PLT hooking for %s", l.c_str());
    GetHookInfo().AddLibHook(l);
  }

  for(const FunctionHook &hook : fallbackhooks)
  {
    RDCLOG("Falling back to PLT hooking for %s", hook.function.c_str());
    GetHookInfo().AddFunctionHook(hook);
  }
}

#else

void PatchHookedFunctions()
{
#if defined(RENDERDOC_HAVE_DOBBY)
  // Android + Dobby 模式：在 EndHookRegistration 阶段立刻对已注册的
  // libEGL/libGLES 等库执行 inline hook，让 EGL 从进程启动阶段就处于
  // 被拦截状态，只保留 so 注入层面的延迟策略。
  RDCLOG("Applying hooks with Dobby inline hooks (immediate EGL/GL install)");
  ApplyDobbyHooksForRegisteredLibraries();
#else
  RDCLOG("Applying hooks with PLT hooks");
#endif
}

#endif

bool LibraryHooks::Detect(const char *identifier)
{
  const bool symbol = (dlsym(RTLD_DEFAULT, identifier) != NULL);
  const bool env = (getenv(identifier) != NULL);

  RDCLOG("Detecting symbol %s by dlsym: %s", identifier, symbol ? "yes" : "no");
  RDCLOG("Detecting symbol %s by getenv: %s", identifier, env ? "yes" : "no");

  return symbol || env;
}

void *LibraryHooks::GetOrigFunctionPtr(const char *funcName)
{
#if defined(RENDERDOC_HAVE_DOBBY)
  if(funcName == NULL)
    return NULL;

  SCOPED_LOCK(g_DobbyHookLock);
  auto it = g_DobbyOrigByName.find(funcName);
  if(it != g_DobbyOrigByName.end())
    return it->second;
#else
  (void)funcName;
#endif

  // PLT/GOT 路径下原始符号地址天然绕过 hook；Dobby 路径下未命中则返回空。
  return NULL;
}

void LibraryHooks::RemoveHooks()
{
  RDCERR("Removing hooks is not possible on this platform");
}

void LibraryHooks::ReplayInitialise()
{
  // nothing to do
}

void LibraryHooks::BeginHookRegistration()
{
  // nothing to do
  RDCLOG("WEN: android_hook-BeginHookRegistration");
}

void LibraryHooks::RegisterFunctionHook(const char *libraryName, const FunctionHook &hook)
{
  // we don't use the library name on android
  (void)libraryName;
  HOOK_DEBUG_PRINT("Registering function hook for %s: %p", hook.function.c_str(), hook.hook);
  GetHookInfo().AddFunctionHook(hook);
}

void LibraryHooks::RegisterLibraryHook(const char *name, FunctionLoadCallback cb)
{
  GetHookInfo().AddLibHook(name);

  HOOK_DEBUG_PRINT("Registering library hook for %s %s", name, cb ? "with callback" : "");

  // open the library immediately if we can
  dlopen(name, RTLD_NOW);

  if(cb)
    GetHookInfo().AddHookCallback(name, cb);
}

void LibraryHooks::IgnoreLibrary(const char *libraryName)
{
}

void LibraryHooks::EndHookRegistration()
{
  RDCLOG("Android_Hook : EndHookRegistration");
  RDCLOG("WEN: android_hook -----------EndHookRegistration");
  // ensure we load all libraries we can immediately, so they are immediately hooked and don't get
  // loaded later.
  rdcarray<rdcstr> libs = GetHookInfo().GetLibHooks();
  for(const rdcstr &lib : libs)
  {
    void *handle = dlopen(lib.c_str(), RTLD_GLOBAL);
    HOOK_DEBUG_PRINT("%s: %p", lib.c_str(), handle);// android 相关的libs
  }

  // try to prevent the library from being unloaded, increment our dlopen refcount (might not work
  // on android, but we'll try!)
  // we use RTLD_NOLOAD to prevent a second copy being loaded if this path doesn't refer to
  // ourselves or otherwise breaks because of android's terrible library handling.
  {
    rdcstr selfLib;
    FileIO::GetLibraryFilename(selfLib);
    if(FileIO::exists(selfLib))
    {
      void *handle = dlopen(selfLib.c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
      if(handle)
        RDCLOG("Dummy-loaded %s with dlopen to prevent library unload", selfLib.c_str());
      else
        RDCLOG("Failed to dummy-loaded %s with dlopen", selfLib.c_str());
    }
    else
    {
      RDCLOG("Couldn't dummy-load %s because it doesn't exist", selfLib.c_str());
    }
  }

  if(libs.empty())
  {
    RDCLOG("No library hooks registered, not doing any hooking");
    return;
  }

  PatchHookedFunctions();

  // this already hooks dlopen (if possible) and android_dlopen_ext, which is enough
  InstallHooksCommon();

  LibraryHooks::Refresh();

  // iterate our list of libraries and look up the original pointer for any that we don't already
  // have. If we have interceptor-lib this will only be for functions that failed to generate a
  // trampoline and we're PLT hooking - without interceptor-lib this will be all functions, but it
  // will allow us to control the order/priority.
  rdcarray<rdcstr> libraryHooks = GetHookInfo().GetLibHooks();
  rdcarray<FunctionHook> functionHooks = GetHookInfo().GetFunctionHooks();

  RDCLOG("Fetching %zu original function pointers over %zu libraries", functionHooks.size(),
         libraryHooks.size());

#if !defined(RENDERDOC_HAVE_DOBBY)
  for(auto it = libraryHooks.begin(); it != libraryHooks.end(); ++it)
  {
    void *handle = dlopen(it->c_str(), RTLD_NOLOAD | RTLD_GLOBAL);

    if(handle)
    {
      for(FunctionHook &hook : functionHooks)
      {
        if(hook.orig && *hook.orig == NULL)
          *hook.orig = dlsym(handle, hook.function.c_str());
      }
    }
  }
#else
  // Dobby 路径下必须以 trampoline 为准，避免把已 inline-hook 的入口地址误当作 orig。
  RDCLOG("Skipping dlsym backfill on Dobby path; trampolines are authoritative");
#endif

  RDCLOG("Finished");

  // call the callbacks for any libraries that loaded now. If the library wasn't loaded above then
  // it can't be loaded, since we only hook system libraries.
  std::map<rdcstr, rdcarray<FunctionLoadCallback>> callbacks = GetHookInfo().GetHookCallbacks();
  for(auto it = callbacks.begin(); it != callbacks.end(); ++it)
  {
    void *handle = dlopen(it->first.c_str(), RTLD_GLOBAL);
    if(handle)
    {
      HOOK_DEBUG_PRINT("Calling callbacks for %s", it->first.c_str());
      for(FunctionLoadCallback callback : it->second)
        if(callback)
          callback(handle);
    }
  }

  RDCLOG("Called library callbacks - hook registration complete");
}

void LibraryHooks::Refresh()
{
  if(suppressTLS == 0)
  {
    RDCLOG("Not refreshing android hooks with no libraries registered");
    return;
  }

  RDCLOG("Refreshing android hooks...");
#if defined(RENDERDOC_HAVE_DOBBY)
  // Dobby 模式下，Refresh 即刻对已注册库进行一次增量扫描 + hook，
  // 覆盖运行过程中后续加载的 libEGL/libGLES 等，保持“全程有钩子”。
  ApplyDobbyHooksForRegisteredLibraries();
#else
  dl_iterate_phdr(dl_iterate_callback, NULL);
#endif
  RDCLOG("Refreshed");
}

ScopedSuppressHooking::ScopedSuppressHooking()
{
  if(suppressTLS == 0)
    return;

  uintptr_t old = (uintptr_t)Threading::GetTLSValue(suppressTLS);
  Threading::SetTLSValue(suppressTLS, (void *)(old + 1));
}

ScopedSuppressHooking::~ScopedSuppressHooking()
{
  if(suppressTLS == 0)
    return;

  uintptr_t old = (uintptr_t)Threading::GetTLSValue(suppressTLS);
  Threading::SetTLSValue(suppressTLS, (void *)(old - 1));
}

bool hooks_suppressed()
{
  if(suppressTLS == 0)
    return true;

  return (uintptr_t)Threading::GetTLSValue(suppressTLS) > 0;
}
