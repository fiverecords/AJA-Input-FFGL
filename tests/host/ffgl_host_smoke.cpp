// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Offline smoke test: a minimal FFGL host that loads AJAInput.so
// through plugMain, like Resolume, on a surfaceless EGL context (Mesa).
// There is no AJA card in CI, so every input runs the synthetic
// generator; what this exercises is the shared capture: clips of one
// input share one capture, capture settings are shared, switching inputs
// re-attaches, the last clip of an input closes its capture, and a GUI
// thread polling the text parameters during all of that is safe (run it
// under ASan / TSan for the latter). Also the plugin's name, ID and
// parameters in their published order, every input, an unknown Input
// value, the No Signal option (Hold Last Frame copies every frame), and
// Resize (the picture keeps its aspect in a clip of another shape).
//
// Linux only; built with -DAJAINPUT_BUILD_HOST_SMOKE=ON.
//   ffgl_host_smoke path/to/AJAInput.so
#include <GL/glew.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

typedef union { uint32_t UIntValue; void* PointerValue; } FFMixed;
typedef FFMixed (*PlugMain)(uint32_t, FFMixed, void*);
typedef void (*SetLogCb)(void (*)(const char*));
struct Viewport { GLuint x, y, width, height; };
struct ProcessGL { uint32_t numInputTextures; void** inputTextures; GLuint HostFBO; };
struct SetParam { uint32_t ParameterNumber; FFMixed NewParameterValue; };
struct ElementName { uint32_t ParameterNumber; uint32_t ElementNumber; };
struct PluginInfo { uint32_t APIMajorVersion, APIMinorVersion; char PluginUniqueID[4];
                    char PluginName[16]; uint32_t PluginType; };

// Log lines may come from capture / producer threads too.
static std::mutex g_logMutex;
static std::vector<std::string> g_log;
static std::thread::id g_mainThread;
static int g_closedOffMainThread = 0;
static void LogCb(const char* s)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_log.push_back(s);
    // A capture must be destroyed on the render thread (GL objects).
    if (std::strstr(s, "capture closed") && std::this_thread::get_id() != g_mainThread)
        ++g_closedOffMainThread;
    std::printf("  log: %s\n", s);
}

static PlugMain pm;
static FFMixed U(uint32_t v) { FFMixed m; m.PointerValue = nullptr; m.UIntValue = v; return m; }
static FFMixed P(void* p) { FFMixed m; m.PointerValue = p; return m; }

static void SetF(void* inst, uint32_t idx, float v)
{
    SetParam sp; sp.ParameterNumber = idx; std::memcpy(&sp.NewParameterValue.UIntValue, &v, 4);
    pm(8, P(&sp), inst);
}
static float GetF(void* inst, uint32_t idx)
{
    FFMixed r = pm(9, U(idx), inst); float f; std::memcpy(&f, &r.UIntValue, 4); return f;
}
static std::string GetT(void* inst, uint32_t idx)
{
    FFMixed r = pm(9, U(idx), inst);
    return r.PointerValue ? std::string((const char*)r.PointerValue) : std::string("(null)");
}

static GLuint g_fbo = 0, g_tex = 0;
static void Process(void* inst)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, 320, 180);
    ProcessGL p{0, nullptr, g_fbo};
    pm(17, P(&p), inst);
}
// Resize: draw into a clip of another shape, the host's viewport
// (x, y, w, h) inside the 320x180 framebuffer.
static void ProcessIn(void* inst, int x, int y, int w, int h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(x, y, w, h);
    ProcessGL p{0, nullptr, g_fbo};
    pm(17, P(&p), inst);
}
// Paint the whole framebuffer with a marker alpha no draw of the plugin
// produces (the picture is opaque, its bars fully transparent).
static const int kMarkerAlpha = 77;
static void FillMarker()
{
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.2f, 0.4f, 0.6f, kMarkerAlpha / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}
static int AlphaAt(int x, int y)
{
    unsigned char px[4] = {0,0,0,0};
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return px[3];
}
static unsigned RgbAt(int x, int y)
{
    unsigned char px[4] = {0,0,0,0};
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return (px[0] << 16) | (px[1] << 8) | px[2];
}
static unsigned CenterPixel()
{
    unsigned char px[4] = {0,0,0,0};
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glReadPixels(40, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return (px[0] << 16) | (px[1] << 8) | px[2];
}
static int Count(const char* needle)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    int n = 0; for (auto& l : g_log) if (l.find(needle) != std::string::npos) ++n; return n;
}
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); ++fails; } else std::printf("ok: %s\n", msg); } while (0)

static void Frames(std::vector<void*> insts, int n)
{
    for (int i = 0; i < n; ++i) { for (void* in : insts) Process(in); usleep(16000); }
}

int main(int argc, char** argv)
{
    g_mainThread = std::this_thread::get_id();
    auto getDpy = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = getDpy(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, 0);
    eglInitialize(d, 0, 0); eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 1,
                   EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE};
    EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c);
    glewExperimental = GL_TRUE; GLenum ge = glewInit();
    std::printf("glew=%d GL=%s\n", ge, glGetString(GL_VERSION));

    glGenTextures(1, &g_tex); glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 320, 180, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &g_fbo); glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_tex, 0);

    void* so = dlopen(argc > 1 ? argv[1] : "./AJAInput.so", RTLD_NOW);
    if (!so) { std::printf("dlopen: %s\n", dlerror()); return 1; }
    pm = (PlugMain)dlsym(so, "plugMain");
    auto setLog = (SetLogCb)dlsym(so, "SetLogCallback");
    if (setLog) setLog(LogCb);
    pm(34, U(0), nullptr);   // FF_INITIALISE_V2

    Viewport vp{0, 0, 320, 180};
    // The published parameter order (compositions store values by index).
    const uint32_t PID_Input = 0, PID_Format = 2, PID_Resize = 3, PID_NoSignal = 4,
                   PID_LL = 7, PID_Ring = 8, PID_TenBit = 9, PID_Frames = 11;
    // Input option values: SDI 1-8 = 0-7, HDMI 1-4 = 8-11, quad-link = 12, 13.
    const float IN_SDI1 = 0.0f, IN_SDI2 = 1.0f, IN_SDI3 = 2.0f, IN_HDMI = 8.0f;

    std::printf("--- plugin info and parameters\n");
    {
        const PluginInfo* info = (const PluginInfo*)pm(0, U(0), nullptr).PointerValue;  // FF_GET_INFO
        CHECK(info && std::strncmp(info->PluginName, "AJA Input", 16) == 0, "plugin is called AJA Input");
        CHECK(info && std::memcmp(info->PluginUniqueID, "AJIN", 4) == 0, "unique ID is AJIN");
        CHECK(pm(4, U(0), nullptr).UIntValue == 15u, "15 parameters");          // FF_GET_NUM_PARAMETERS
        const char* order[] = {"Input", "Device", "Format", "Resize", "No Signal", "Colorimetry",
                               "Deinterlace", "Low Latency Mode", "Ring Depth",
                               "10-bit capture (v210)", "Show Stats", "Stats / Frames",
                               "Stats / Drops", "Stats / Recoveries", "Stats / Idle wakeups"};
        bool orderOk = true;
        for (uint32_t i = 0; i < 15; ++i)
        {
            const char* n = (const char*)pm(5, U(i), nullptr).PointerValue;   // FF_GET_PARAMETER_NAME
            if (!n || std::strcmp(n, order[i]) != 0)
            {
                std::printf("parameter %u is \"%s\", expected \"%s\"\n", i, n ? n : "(null)", order[i]);
                orderOk = false;
            }
        }
        CHECK(orderOk, "parameters in the published order");
        CHECK(pm(31, U(PID_NoSignal), nullptr).UIntValue == 3u, "No Signal has 3 choices");
        CHECK(pm(31, U(PID_Resize), nullptr).UIntValue == 4u, "Resize has 4 choices");
        const char* resizeNames[] = {"Fit", "Fill", "Stretch", "Original"};
        bool namesOk = true;
        for (uint32_t i = 0; i < 4; ++i)
        {
            ElementName rn{PID_Resize, i};
            const char* e = (const char*)pm(35, P(&rn), nullptr).PointerValue;
            namesOk = namesOk && e && std::strcmp(e, resizeNames[i]) == 0;
        }
        CHECK(namesOk, "Resize choices: Fit, Fill, Stretch, Original");
        // No AJA card here: the Input menu offers SDI 1, SDI 2 and HDMI.
        CHECK(pm(31, U(PID_Input), nullptr).UIntValue == 3u, "Input menu without a card: 3 entries");
        ElementName en{PID_Input, 2};
        const char* el = (const char*)pm(35, P(&en), nullptr).PointerValue;   // FF_GET_PARAMETER_ELEMENT_NAME
        CHECK(el && std::strcmp(el, "HDMI") == 0, "third entry is HDMI");
        float hdmiValue = -1.0f;
        {
            ElementName q{PID_Input, 2};
            FFMixed r = pm(36, P(&q), nullptr);      // FF_GET_PARAMETER_ELEMENT_VALUE
            std::memcpy(&hdmiValue, &r.UIntValue, sizeof(float));
        }
        CHECK(hdmiValue == IN_HDMI, "HDMI's option value is 8");
    }

    std::printf("--- two clips on SDI1\n");
    void* A = pm(18, P(&vp), nullptr).PointerValue;   // FF_INSTANTIATE_GL (calls InitGL)
    void* B = pm(18, P(&vp), nullptr).PointerValue;
    CHECK(A && B, "two instances created");
    // Lazy attach: the host applies saved values after InitGL, so nothing
    // is captured until a clip is rendered.
    CHECK(Count("capture (new") == 0, "no capture before a clip is shown");
    CHECK(GetT(A, PID_Format).find("idle (starts when shown)") != std::string::npos, "unshown clip says it is idle");
    Frames({A, B}, 20);
    CHECK(Count("capture (new") == 1 && Count("capture (shared") == 1, "second clip shares the first clip's capture");
    std::string fa = GetT(A, PID_Format), fb = GetT(B, PID_Format);
    std::printf("A: %s\nB: %s\n", fa.c_str(), fb.c_str());
    CHECK(fa.find("2 clips") != std::string::npos && fb.find("2 clips") != std::string::npos, "readout says 2 clips");
    const unsigned pxA = CenterPixel();
    Process(B); const unsigned pxB = CenterPixel();
    std::printf("pixel A=%06X B=%06X\n", pxA, pxB);
    CHECK(pxA != 0x0F0F0F && pxA != 0x000000 && pxA == pxB, "both clips draw the same (non-background) picture");
    CHECK(GetT(A, PID_Frames) != "0", "frames counted");
    CHECK(Count("synthetic producer started for SDI1") == 1, "one producer for the shared input");

    std::printf("--- shared settings\n");
    SetF(A, PID_LL, 1.0f); SetF(A, PID_Ring, 3.0f);
    // A clip that has not rendered since (e.g. sitting in the deck)
    // reports the input's value too.
    CHECK(GetF(B, PID_LL) == 1.0f && GetF(B, PID_Ring) == 3.0f, "idle B reports the shared values at once");
    Frames({A, B}, 3);
    CHECK(GetF(B, PID_LL) == 1.0f && GetF(B, PID_Ring) == 3.0f, "B shows A's Low Latency / Ring Depth");

    std::printf("--- B switches to SDI2\n");
    SetF(B, PID_Input, IN_SDI2);
    Frames({A, B}, 10);
    fa = GetT(A, PID_Format); fb = GetT(B, PID_Format);
    std::printf("A: %s\nB: %s\n", fa.c_str(), fb.c_str());
    CHECK(fa.find("clips") == std::string::npos && fb.find("clips") == std::string::npos, "one clip each now");
    CHECK(Count("synthetic producer started for SDI2") == 1, "SDI2 capture created");
    CHECK(GetF(B, PID_LL) == 1.0f, "B kept its settings when starting the SDI2 capture");

    std::printf("--- third clip joins SDI2 with different settings\n");
    const int sdi1Attaches = Count("clip attached to the SDI1");
    void* C = pm(18, P(&vp), nullptr).PointerValue;
    SetF(C, PID_Input, IN_SDI2);   // saved values arrive after InitGL
    Frames({A, B, C}, 5);
    CHECK(Count("clip attached to the SDI1") == sdi1Attaches, "C went straight to SDI2 (never attached to the default SDI1)");
    std::printf("C: %s ring=%.0f\n", GetT(C, PID_Format).c_str(), GetF(C, PID_Ring));
    CHECK(GetT(C, PID_Format).find("2 clips") != std::string::npos, "C shares SDI2 with B");
    CHECK(GetF(C, PID_Ring) == 3.0f, "C adopted the SDI2 capture's Ring Depth");
    CHECK(Count("SDI1 capture closed") == 0, "SDI1 capture still alive for A");

    std::printf("--- a clip loaded into the deck and never shown\n");
    {
        const int created = Count("capture (new");
        void* D = pm(18, P(&vp), nullptr).PointerValue;
        SetF(D, PID_Input, IN_HDMI);
        SetF(D, PID_LL, 0.0f);
        SetF(D, PID_Ring, 5.0f);
        CHECK(GetF(A, PID_LL) == 1.0f && GetF(A, PID_Ring) == 3.0f, "deck clip's saved values do not touch SDI1's capture");
        CHECK(GetF(D, PID_LL) == 0.0f && GetF(D, PID_Ring) == 5.0f, "deck clip reports its own values");
        CHECK(GetT(D, PID_Format).find("HDMI idle") != std::string::npos, "deck clip readout: HDMI idle");
        CHECK(GetT(A, PID_Format).find("clips") == std::string::npos, "deck clip is not counted on SDI1");
        pm(19, U(0), D);
        CHECK(Count("capture (new") == created, "deck clip never created a capture");
    }

    std::printf("--- inputs of other AJA cards (synthetic here)\n");
    {
        void* E = pm(18, P(&vp), nullptr).PointerValue;
        SetF(E, PID_NoSignal, 2.0f);          // Hold Last Frame: copies every frame
        CHECK(GetF(E, PID_NoSignal) == 2.0f, "No Signal value round-trips");
        const struct { float value; const char* label; } inputs[] = {
            {4.0f, "SDI5"}, {7.0f, "SDI8"}, {9.0f, "HDMI2"}, {11.0f, "HDMI4"},
            {12.0f, "SDI1-4"}, {13.0f, "SDI5-8"} };
        for (const auto& in : inputs)
        {
            SetF(E, PID_Input, in.value);
            Frames({A, B, C, E}, 4);
            const std::string started = std::string("synthetic producer started for ") + in.label;
            CHECK(Count(started.c_str()) >= 1, started.c_str());
            const unsigned px = CenterPixel();
            CHECK(px != 0x000000, "the input shows its test pattern");
        }
        // A value no input has (a later version's composition): SDI 1.
        const int sdi1 = Count("clip attached to the SDI1");
        SetF(E, PID_Input, 20.0f);
        Frames({A, B, C, E}, 3);
        CHECK(Count("clip attached to the SDI1") == sdi1 + 1, "unknown Input value falls back to SDI 1");
        CHECK(GetT(E, PID_Format).find("2 clips") != std::string::npos, "and shares SDI1 with A");
        SetF(E, PID_NoSignal, 1.0f);
        Frames({A, B, C, E}, 2);
        pm(19, U(0), E);
        CHECK(glGetError() == GL_NO_ERROR, "no GL error after Hold Last Frame copies");
    }

    std::printf("--- Resize: the 16:9 picture in a 4:3 clip\n");
    {
        void* F = pm(18, P(&vp), nullptr).PointerValue;
        CHECK(GetF(F, PID_Resize) == 0.0f, "Resize defaults to Fit");
        // The clip is the 240x180 viewport; the framebuffer's last 80
        // columns are outside it and must keep the marker.
        auto run = [&](float mode) {
            SetF(F, PID_Resize, mode);
            for (int i = 0; i < 4; ++i) { FillMarker(); ProcessIn(F, 0, 0, 240, 180); usleep(16000); }
        };
        run(0.0f);   // Fit: 240x135 at y = 22
        std::printf("Fit: bar a=%d picture a=%d rgb=%06X outside a=%d\n",
                    AlphaAt(120, 5), AlphaAt(120, 90), RgbAt(120, 90), AlphaAt(300, 90));
        CHECK(AlphaAt(120, 5) == 0 && AlphaAt(120, 175) == 0, "Fit: transparent bars above and below");
        CHECK(AlphaAt(120, 90) == 255 && RgbAt(120, 90) != 0x000000, "Fit: the picture in the middle");
        CHECK(AlphaAt(120, 21) == 0 && AlphaAt(120, 22) == 255 &&
              AlphaAt(120, 156) == 255 && AlphaAt(120, 157) == 0, "Fit: the picture is lines 22-156 (135 lines)");
        CHECK(AlphaAt(300, 90) == kMarkerAlpha, "Fit: nothing drawn outside the clip");
        GLint now[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, now);
        CHECK(now[0] == 0 && now[1] == 0 && now[2] == 240 && now[3] == 180, "host viewport restored");
        CHECK(!glIsEnabled(GL_SCISSOR_TEST), "host scissor test (off) restored");
        GLfloat cc[4] = {0, 0, 0, 0};
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        CHECK(cc[3] > 0.29f && cc[3] < 0.31f, "host clear colour restored");
        run(1.0f);   // Fill: 320x180 at x = -40, cut to the clip
        CHECK(AlphaAt(120, 5) == 255 && AlphaAt(2, 90) == 255 && AlphaAt(238, 90) == 255, "Fill: no bars");
        CHECK(AlphaAt(300, 90) == kMarkerAlpha, "Fill: the overflow is cut at the clip's edge");
        run(2.0f);   // Stretch: the full-clip draw
        CHECK(AlphaAt(120, 5) == 255 && AlphaAt(300, 90) == kMarkerAlpha, "Stretch: the whole clip");
        run(3.0f);   // Original: 1280x720 centred, cut to the clip
        CHECK(AlphaAt(120, 5) == 255 && AlphaAt(300, 90) == kMarkerAlpha, "Original: centred and cut to the clip");
        // A scissor box the host set is kept: nothing is drawn outside it,
        // and it is still set afterwards.
        SetF(F, PID_Resize, 0.0f);
        FillMarker();
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 200, 180);
        ProcessIn(F, 0, 0, 240, 180);
        GLint box[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_SCISSOR_BOX, box);
        CHECK(glIsEnabled(GL_SCISSOR_TEST) && box[0] == 0 && box[2] == 200 && box[3] == 180,
              "host scissor test and box restored");
        glDisable(GL_SCISSOR_TEST);
        CHECK(AlphaAt(220, 90) == kMarkerAlpha && AlphaAt(220, 5) == kMarkerAlpha, "the host's scissor box is respected");
        CHECK(AlphaAt(120, 90) == 255 && AlphaAt(120, 5) == 0, "and the picture drawn inside it");
        // No Signal Black covers the picture's area only. The first frame
        // of a new input usually has no picture yet; if it already has one
        // there is nothing to check.
        SetF(F, PID_NoSignal, 0.0f);
        SetF(F, PID_Input, IN_SDI3);   // a new capture
        FillMarker();
        ProcessIn(F, 0, 0, 240, 180);
        if (RgbAt(120, 90) == 0x000000 && AlphaAt(120, 90) == 255)
            CHECK(AlphaAt(120, 5) == 0 && AlphaAt(300, 90) == kMarkerAlpha,
                  "No Signal Black: the picture's area is black, its bars transparent");
        else
            std::printf("skip: the new input's first frame already had a picture\n");
        Frames({A, B, C}, 1);
        for (int i = 0; i < 3; ++i) { ProcessIn(F, 0, 0, 240, 180); usleep(16000); }
        pm(19, U(0), F);
        CHECK(glGetError() == GL_NO_ERROR, "no GL error with Resize");
    }

    std::printf("--- 10-bit toggle on a synthetic input (no-op kind)\n");
    SetF(B, PID_TenBit, 1.0f);
    Frames({A, B, C}, 3);
    CHECK(GetF(C, PID_TenBit) == 1.0f, "C sees B's 10-bit setting");

    std::printf("--- stress: input switching and settings while a GUI thread polls\n");
    {
        std::atomic<bool> run{true};
        std::atomic<long> polls{0};
        std::thread gui([&]{
            while (run.load()) {
                for (void* in : {A, B, C}) {
                    for (uint32_t idx : {PID_Format, 11u, 12u, 13u, 14u}) {
                        FFMixed r = pm(9, U(idx), in);
                        if (r.PointerValue) { volatile char ch = *(const char*)r.PointerValue; (void)ch; }
                    }
                    (void)GetF(in, PID_LL);
                }
                ++polls;
            }
        });
        for (int i = 0; i < 60; ++i) {
            SetF(B, PID_Input, (i & 1) ? IN_SDI1 : IN_SDI2);
            if (i % 7 == 0) SetF(C, PID_LL, (i & 2) ? 1.0f : 0.0f);
            if (i % 11 == 0) SetF(A, PID_TenBit, (i & 4) ? 1.0f : 0.0f);
            if (i % 5 == 0) SetF(C, PID_Input, (i & 8) ? IN_HDMI : IN_SDI2);
            Frames({A, B, C}, 2);
        }
        run = false; gui.join();
        std::printf("gui polls=%ld\n", polls.load());
        SetF(B, PID_Input, IN_SDI2); SetF(C, PID_Input, IN_SDI2);
        Frames({A, B, C}, 3);
        CHECK(GetT(B, PID_Format).find("2 clips") != std::string::npos, "after the storm B and C share SDI2 again");
    }

    std::printf("--- teardown\n");
    const int c1 = Count("SDI1 capture closed"), c2 = Count("SDI2 capture closed");
    pm(19, U(0), A);   // FF_DEINSTANTIATE_GL
    CHECK(Count("SDI1 capture closed") == c1 + 1, "SDI1 capture closed with its last clip");
    pm(19, U(0), B);
    CHECK(Count("SDI2 capture closed") == c2, "SDI2 capture still alive for C");
    Frames({C}, 3);
    CHECK(GetT(C, PID_Format).find("clips") == std::string::npos, "C alone on SDI2");
    pm(19, U(0), C);
    CHECK(Count("SDI2 capture closed") == c2 + 1, "SDI2 capture closed with its last clip");
    CHECK(Count("capture (new") == Count("capture closed"), "every capture created was closed");
    CHECK(g_closedOffMainThread == 0, "every capture was destroyed on the render thread");
    std::printf("glError=%x\n", glGetError());
    std::printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
