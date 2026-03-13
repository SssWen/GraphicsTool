#include <cstdint>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <dlfcn.h>
#include <string>
#include <time.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "hack.h"
#include "log.h"
#include "game.h"
#include "utils.h"
#include "xdl.h"
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#include "MemoryPatch.h"

static int                  g_GlHeight, g_GlWidth;
static bool                 g_IsSetup = false;
static std::string          g_IniFileName = "";
static utils::module_info   g_TargetModule{};

// 记录绑定 ImGui GL 对象的 EGL Context
static EGLContext           g_ImGuiContext   = EGL_NO_CONTEXT;
// Context 切换后延迟一帧重建 Backend：切换帧不做任何 GL 调用，下一帧再 Shutdown+Init，
// 避免在同一帧内"删旧对象→编译 Shader→上传字体纹理→渲染"导致驱动崩溃
static bool                 g_NeedsGLReinit  = false;

// 游戏启动后延迟 10 秒再初始化 ImGui，让游戏主逻辑先稳定加载完成
static const int            IMGUI_START_DELAY_SEC = 10;
static time_t               g_HookStartTime  = 0;

HOOKAF(void, Input, void *thiz, void *ex_ab, void *ex_ac) {
    origInput(thiz, ex_ab, ex_ac);
    ImGui_ImplAndroid_HandleInputEvent((AInputEvent *)thiz);
    return;
}

void SetupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();

    // g_IniFileName 是全局 std::string，生命周期覆盖整个进程，c_str() 指针稳定
    io.IniFilename = g_IniFileName.c_str();

    // 字体必须在 Init Backends 之前添加，以便 Backend 第一次 NewFrame 时能正确构建字体纹理
    ImFontConfig font_cfg;
    font_cfg.SizePixels = 22.0f;
    io.Fonts->AddFontDefault(&font_cfg);

    // Init Backends 放在字体配置完毕之后
    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    ImGui::StyleColorsLight();
    ImGui::GetStyle().ScaleAllSizes(3.0f);
}

EGLBoolean (*old_eglSwapBuffers)(EGLDisplay dpy, EGLSurface surface);
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {

    // 记录第一次被调用的时间戳（即游戏 EGL 渲染开始的时刻）
    if (g_HookStartTime == 0)
        g_HookStartTime = time(nullptr);

    // 延迟期内直接透传，让游戏主逻辑先稳定加载，避免过早初始化 ImGui 导致资源竞争
    if (time(nullptr) - g_HookStartTime < IMGUI_START_DELAY_SEC)
        return old_eglSwapBuffers(dpy, surface);
    LOGI("************************10s结束***********************************");
    eglQuerySurface(dpy, surface, EGL_WIDTH, &g_GlWidth);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &g_GlHeight);

    // 宽高无效时直接透传，避免 ImGui 用 0 尺寸渲染时内部矩阵计算异常
    if (g_GlWidth <= 0 || g_GlHeight <= 0)
        return old_eglSwapBuffers(dpy, surface);

    EGLContext currentContext = eglGetCurrentContext();
    if (currentContext == EGL_NO_CONTEXT)
        return old_eglSwapBuffers(dpy, surface);


    if (!g_IsSetup) {
        g_ImGuiContext = currentContext;
        SetupImGui();
        g_IsSetup = true;
    } else if (currentContext != g_ImGuiContext) {
        // ── 阶段 1：Context 切换帧 ──────────────────────────────────────────
        // 本帧不做任何 GL 调用，只更新 Context 引用并标记需要重建
        // 同一帧内"删旧对象 → Init → 渲染"会在部分驱动上崩溃，必须分帧执行
        LOGI("EGL Context switched: %p -> %p, skip this frame", g_ImGuiContext, currentContext);
        g_ImGuiContext  = currentContext;
        g_NeedsGLReinit = true;
        return old_eglSwapBuffers(dpy, surface);
    }

    // ── 阶段 2：Context 切换后的第一帧 ──────────────────────────────────────
    // 旧 GL 对象在当前 Context 中不存在，glDelete* 静默失败（符合 GL 规范）
    // Init 后 NewFrame 会在新 Context 中重新编译 Shader、上传字体纹理
    if (g_NeedsGLReinit) {
        LOGI("EGL Context reinit OpenGL backend in new context: %p", currentContext);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        g_NeedsGLReinit = false;
    }

    ImGuiIO &io = ImGui::GetIO();

    // ImGui_ImplOpenGL3_RenderDrawData 内部已经完整保存/还原 GL 状态（blend/depth/cull/viewport 等）
    // 这里额外保存 depth test 和 cull face 作为双重保险，防止某些旧版 ImGui 遗漏
    GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevCullFace  = glIsEnabled(GL_CULL_FACE);

    ImGui_ImplOpenGL3_NewFrame();
    // 使用带宽高参数的重载；Init 传入的 ANativeWindow* 为 nullptr，无参版本调用时会崩溃
    ImGui_ImplAndroid_NewFrame(g_GlWidth, g_GlHeight);
    ImGui::NewFrame();

    ImGui::ShowDemoWindow();

    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // 还原 RenderDrawData 不保证还原的状态
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCullFace)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);

    return old_eglSwapBuffers(dpy, surface);
}

void hook_egl() {
    void *egl_handle = xdl_open("libEGL.so", XDL_DEFAULT);
    void *eglSwapBuffers = xdl_sym(egl_handle, "eglSwapBuffers", nullptr);
    if (eglSwapBuffers != nullptr) {
        LOGI("***********************************************************");
        LOGI("* hook eglSwapBuffers 成功 !!!!!!!!!!!!!!!!!!!!!!!!!!!!!! *");
        LOGI("***********************************************************");
        utils::hook((void*)eglSwapBuffers, (func_t)hook_eglSwapBuffers, (func_t*)&old_eglSwapBuffers);        
    }
    xdl_close(egl_handle);
}

void hook_gles() {
    // TODO: 在此添加 OpenGL ES 函数 Hook
}

typedef EGLDisplay (*eglGetDisplay_t)(EGLNativeDisplayType display_id);
eglGetDisplay_t old_eglGetDisplay = nullptr;

EGLDisplay my_eglGetDisplay(EGLNativeDisplayType display_id) {
    static bool inited = false;
    if (!inited) {
        LOGI("***********************************************************");
        LOGI("* Unity 正在初始化 EGL，现在开始 Hook!!!!!!!!!!!!!!!!!!!!!! *");
        LOGI("***********************************************************");
        inited = true;
        hook_egl();
        hook_gles();
        
    }
    return old_eglGetDisplay(display_id);
}

void prehook_eglgetdisplay() {
    void *h = xdl_open("libEGL.so", XDL_DEFAULT);
    void *sym = xdl_sym(h, "eglGetDisplay", nullptr);
    if (sym != nullptr) {
        LOGI("***********************************************************");
        LOGI("* 第一次 Hook eglGetDisplay 成功 !!!!!!!!!!!!!!!!!!!!!!!!!!!!!! *");
        LOGI("***********************************************************");
        utils::hook(sym, (func_t)my_eglGetDisplay, (func_t*)&old_eglGetDisplay);        
    }
    xdl_close(h);
}

void hack_start(const char *_game_data_dir) {
    LOGI("hack start | %s", _game_data_dir);
    do {
        sleep(1);
        g_TargetModule = utils::find_module(TargetLibName);
    } while (g_TargetModule.size <= 0);
    LOGI("%s: %p - %p", TargetLibName, g_TargetModule.start_address, g_TargetModule.end_address);

    // TODO: hooking/patching here
}

void hack_prepare(const char *_game_data_dir) {
    LOGI("hack thread: %d", gettid());
    int api_level = utils::get_android_api_level();
    LOGI("api level: %d", api_level);
    g_IniFileName = std::string(_game_data_dir) + "/files/imgui.ini";

    hack_start(_game_data_dir);
}
