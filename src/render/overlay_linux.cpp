#include "overlay_linux.h"

#include <GL/gl.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <spdlog/spdlog.h>

/* ---------------------------------------------------------------------------
 * Window identity spoofing.
 *
 * EAC on Linux enumerates all X11 windows via XQueryTree and reads:
 *   WM_NAME / _NET_WM_NAME  — window title
 *   WM_CLASS               — app instance + class (res_name, res_class)
 *   _NET_WM_PID            — links window back to a specific process
 *   WM_CLIENT_MACHINE      — hostname
 *
 * Strategy:
 *   1. Query actually-running Steam windows and copy a real title verbatim.
 *      This is indistinguishable from Steam itself — no fixed pool to blacklist.
 *   2. If Steam isn't running, generate a random hex fragment that looks like
 *      a system identifier (e.g. "a3f8c1d2") — cannot be blacklisted by name.
 * ---------------------------------------------------------------------------*/

static std::string GenerateRandomHexTitle()
{
    /* Read 4 random bytes from /dev/urandom for the hex fragment */
    uint32_t rnd = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        (void)read(fd, &rnd, sizeof(rnd));
        close(fd);
    } else {
        rnd = (uint32_t)(uintptr_t)&rnd ^ (uint32_t)time(nullptr);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%08x", rnd);
    return std::string(buf);
}

static std::string StealSteamWindowTitle(Display* display)
{
    Window root = DefaultRootWindow(display);
    Window parent{}, *children = nullptr;
    unsigned int nchildren = 0;
    if (!XQueryTree(display, root, &root, &parent, &children, &nchildren))
        return {};

    Atom net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    Atom utf8_string  = XInternAtom(display, "UTF8_STRING",  False);
    Atom wm_class_atom = XInternAtom(display, "WM_CLASS",   False);

    std::string result;
    for (unsigned int i = 0; i < nchildren && result.empty(); ++i) {
        /* Check WM_CLASS for "Steam" */
        Atom actual_type{};
        int actual_format{};
        unsigned long nitems{}, bytes_after{};
        unsigned char* prop = nullptr;

        if (XGetWindowProperty(display, children[i], wm_class_atom,
                               0, 256, False, AnyPropertyType,
                               &actual_type, &actual_format,
                               &nitems, &bytes_after, &prop) != Success || !prop) {
            continue;
        }
        /* WM_CLASS is two null-terminated strings: instance\0class\0 */
        std::string cls(reinterpret_cast<char*>(prop));
        XFree(prop);
        prop = nullptr;

        if (cls != "steam" && cls != "Steam")
            continue;

        /* Found a Steam window — grab its _NET_WM_NAME */
        if (XGetWindowProperty(display, children[i], net_wm_name,
                               0, 1024, False, utf8_string,
                               &actual_type, &actual_format,
                               &nitems, &bytes_after, &prop) == Success && prop) {
            std::string title(reinterpret_cast<char*>(prop), nitems);
            XFree(prop);
            if (!title.empty())
                result = title;
        }
    }
    if (children)
        XFree(children);
    return result;
}

static std::string PickWindowTitle(Display* display)
{
    /* Prefer an exact live Steam window title */
    std::string live = StealSteamWindowTitle(display);
    if (!live.empty())
        return live;
    /* Fallback: random hex fragment — cannot be blacklisted */
    return GenerateRandomHexTitle();
}

static void SpoofWindowIdentity(Display* display, Window window, const std::string& title)
{
    /* 1. Title: WM_NAME (legacy) + _NET_WM_NAME (EWMH) */
    XStoreName(display, window, title.c_str());
    Atom net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    Atom utf8_string  = XInternAtom(display, "UTF8_STRING",  False);
    XChangeProperty(display, window, net_wm_name, utf8_string, 8,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(title.c_str()),
                    static_cast<int>(title.size()));

    /* 2. WM_CLASS: res_name + res_class — both "Steam" */
    XClassHint hint{};
    hint.res_name  = const_cast<char*>("steam");
    hint.res_class = const_cast<char*>("Steam");
    XSetClassHint(display, window, &hint);

    /* 3. _NET_WM_PID: delete — eliminates the direct process link */
    Atom net_wm_pid = XInternAtom(display, "_NET_WM_PID", False);
    XDeleteProperty(display, window, net_wm_pid);

    /* 4. WM_CLIENT_MACHINE: replace real hostname with "localhost" */
    const char* machine = "localhost";
    XChangeProperty(display, window, XA_WM_CLIENT_MACHINE, XA_STRING, 8,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(machine),
                    static_cast<int>(strlen(machine)));

    XFlush(display);
}

namespace {
void RequestWindowState(Display* display, Window window, Atom first, Atom second = None) {
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;
    event.xclient.data.l[1] = static_cast<long>(first);
    event.xclient.data.l[2] = static_cast<long>(second);
    event.xclient.data.l[3] = 1;
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
}
}

Overlay::~Overlay() {
    Shutdown();
}

bool Overlay::Initialize(void*) {
    if (!glfwInit()) {
        spdlog::error("GLFW initialization failed");
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    m_width = mode ? mode->width : 1920;
    m_height = mode ? mode->height : 1080;

    /* Title is set via SpoofWindowIdentity after creation — use empty placeholder */
    m_window = glfwCreateWindow(m_width, m_height, "",
                                nullptr, nullptr);
    if (!m_window) {
        spdlog::error("Failed to create Linux overlay window");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(0);

    Display* display = glfwGetX11Display();
    Window window = glfwGetX11Window(m_window);

    /* Pick title now that we have the Display — prefer a live Steam title */
    m_windowTitle = PickWindowTitle(display);

    /* Spoof all identity properties before the window is visible */
    SpoofWindowIdentity(display, window, m_windowTitle);

    RequestWindowState(display, window,
        XInternAtom(display, "_NET_WM_STATE_ABOVE", False),
        XInternAtom(display, "_NET_WM_STATE_STICKY", False));
    RequestWindowState(display, window,
        XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False),
        XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False));

    XEvent desktopEvent{};
    desktopEvent.xclient.type = ClientMessage;
    desktopEvent.xclient.window = window;
    desktopEvent.xclient.message_type = XInternAtom(display, "_NET_WM_DESKTOP", False);
    desktopEvent.xclient.format = 32;
    desktopEvent.xclient.data.l[0] = -1;
    desktopEvent.xclient.data.l[1] = 1;
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &desktopEvent);
    XRaiseWindow(display, window);
    XFlush(display);
    SetInputRegion(false);

    spdlog::info("Linux overlay initialized ({}x{})", m_width, m_height);
    return true;
}

void Overlay::BeginFrame() {
    glfwPollEvents();
    const double now = glfwGetTime();
    if (now - m_lastRaiseTime >= 1.0) {
        Display* display = glfwGetX11Display();
        Window window = glfwGetX11Window(m_window);
        XRaiseWindow(display, window);
        /* Re-apply identity spoof in case a window manager reset our properties */
        SpoofWindowIdentity(display, window, m_windowTitle);
        XFlush(display);
        m_lastRaiseTime = now;
    }
    glViewport(0, 0, m_width, m_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Overlay::EndFrame() {
    glfwSwapBuffers(m_window);
}

void Overlay::SetClickThrough(bool clickThrough) {
    if (m_clickThrough == clickThrough)
        return;
    m_clickThrough = clickThrough;
    SetInputRegion(!clickThrough);
}

void Overlay::SetInputRegion(bool enabled) {
    if (!m_window)
        return;

    Display* display = glfwGetX11Display();
    Window window = glfwGetX11Window(m_window);
    if (!enabled) {
        XShapeCombineRectangles(display, window, ShapeInput, 0, 0,
                                nullptr, 0, ShapeSet, YXBanded);
    } else {
        XRectangle rect{0, 0, static_cast<unsigned short>(m_width),
                        static_cast<unsigned short>(m_height)};
        XShapeCombineRectangles(display, window, ShapeInput, 0, 0,
                                &rect, 1, ShapeSet, YXBanded);
    }
    XFlush(display);
}

void Overlay::Shutdown() {
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        glfwTerminate();
    }
}
