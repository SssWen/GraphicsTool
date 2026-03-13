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

// 游戏启动后延迟 N 秒再启动 ImGui，等待游戏主逻辑加载完毕
static const int            IMGUI_START_DELAY_SEC = 15;
static time_t               g_HookStartTime = 0;

HOOKAF(void, Input, void *thiz, void *ex_ab, void *ex_ac) {
    origInput(thiz, ex_ab, ex_ac);
    ImGui_ImplAndroid_HandleInputEvent((AInputEvent *)thiz);
}

void SetupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();

    io.IniFilename = g_IniFileName.c_str();

    // 字体必须在 Init Backends 之前配置
    ImFontConfig font_cfg;
    font_cfg.SizePixels = 22.0f;
    io.Fonts->AddFontDefault(&font_cfg);

    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    ImGui::StyleColorsLight();
    ImGui::GetStyle().ScaleAllSizes(3.0f);
}

EGLBoolean (*old_eglSwapBuffers)(EGLDisplay dpy, EGLSurface surface);
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {

    if (g_HookStartTime == 0)
        g_HookStartTime = time(nullptr);

    if (time(nullptr) - g_HookStartTime < IMGUI_START_DELAY_SEC)
        return old_eglSwapBuffers(dpy, surface);

    eglQuerySurface(dpy, surface, EGL_WIDTH, &g_GlWidth);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &g_GlHeight);

    if (g_GlWidth <= 0 || g_GlHeight <= 0)
        return old_eglSwapBuffers(dpy, surface);

    if (!g_IsSetup) {
        SetupImGui();
        g_IsSetup = true;
    }

    ImGuiIO &io = ImGui::GetIO();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_GlWidth, g_GlHeight);
    ImGui::NewFrame();

    ImGui::ShowDemoWindow();

    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return old_eglSwapBuffers(dpy, surface);
}

void hook_egl() {
    void *egl_handle = xdl_open("libEGL.so", XDL_DEFAULT);
    void *eglSwapBuffers = xdl_sym(egl_handle, "eglSwapBuffers", nullptr);
    if (eglSwapBuffers != nullptr) {
        LOGI("hook eglSwapBuffers success");
        utils::hook((void*)eglSwapBuffers, (func_t)hook_eglSwapBuffers, (func_t*)&old_eglSwapBuffers);
    }
    xdl_close(egl_handle);
}

void hook_gles() {
    // TODO: 在此添加 OpenGL ES 函数 Hook
}

EGLBoolean hook_eglSwapBuffers_Backup(EGLDisplay dpy, EGLSurface surface) {

    if (g_HookStartTime == 0)
        g_HookStartTime = time(nullptr);

    if (time(nullptr) - g_HookStartTime < IMGUI_START_DELAY_SEC)
        return old_eglSwapBuffers(dpy, surface);

    eglQuerySurface(dpy, surface, EGL_WIDTH, &g_GlWidth);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &g_GlHeight);

    if (g_GlWidth <= 0 || g_GlHeight <= 0)
        return old_eglSwapBuffers(dpy, surface);

    if (!g_IsSetup) {
        SetupImGui();
        g_IsSetup = true;
    }

    ImGuiIO &io = ImGui::GetIO();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_GlWidth, g_GlHeight);
    ImGui::NewFrame();

    ImGui::ShowDemoWindow();

    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return old_eglSwapBuffers(dpy, surface);
}


typedef EGLDisplay (*eglGetDisplay_t)(EGLNativeDisplayType display_id);
eglGetDisplay_t old_eglGetDisplay = nullptr;

EGLDisplay my_eglGetDisplay(EGLNativeDisplayType display_id) {
    static bool inited = false;
    if (!inited) {
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
        LOGI("hook eglGetDisplay success");
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
    LOGI("api level: %d", utils::get_android_api_level());
    g_IniFileName = std::string(_game_data_dir) + "/files/imgui.ini";

    hack_start(_game_data_dir);
}
