// X11 input blocking against the layer's real blocker: notifications capture only
// their hitbox, while an interactive menu owns pointer and keyboard until close.
// A held key must be released into the game on entry and restored on exit.
//
// It synthesises real input on the developer's display, so it only runs with
// VKHOOK_X11_INPUT_TEST=1 set; otherwise it reports SKIP (exit 77).
//
// usage: vkhook-x11-input-handoff-tests <path to libvkhook.so>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "input/mouse_input.hh"

typedef void (*InitFn)(bool);
typedef void (*SetWindowFn)(unsigned long);
typedef void (*SetRectsFn)(const void*, int);
typedef void (*SetBoolFn)(bool);
typedef void (*SetPosFn)(float, float);
typedef VkHook::MouseState (*GetMouseFn)();

struct InputRect { float x, y, width, height; };

static void pump(Display* d, const char* tag, int* sawPress, int* sawRelease, int* sawGrabFocus)
{
    while (XPending(d) > 0)
    {
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type == KeyPress)   { (*sawPress)++;   printf("    [%s] game: KeyPress keycode=%u\n", tag, ev.xkey.keycode); }
        if (ev.type == KeyRelease) { (*sawRelease)++; printf("    [%s] game: KeyRelease keycode=%u\n", tag, ev.xkey.keycode); }
        if ((ev.type == FocusIn || ev.type == FocusOut) && ev.xfocus.mode != 0)
        {
            (*sawGrabFocus)++;
            printf("    [%s] game: %s mode=%d (grab notification Wine reacts to)\n", tag,
                   ev.type == FocusIn ? "FocusIn" : "FocusOut", ev.xfocus.mode);
        }
    }
    fflush(stdout);
}

static const char* grabStatus(Display* probe, Window w, int keyboard)
{
    int status = keyboard
        ? XGrabKeyboard(probe, w, False, GrabModeAsync, GrabModeAsync, CurrentTime)
        : XGrabPointer(probe, w, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    if (keyboard) XUngrabKeyboard(probe, CurrentTime);
    else          XUngrabPointer(probe, CurrentTime);
    XSync(probe, False);
    return status == AlreadyGrabbed ? "held by the layer" : (status == GrabSuccess ? "free" : "other");
}

int main(int argc, char** argv)
{
    if (getenv("VKHOOK_X11_INPUT_TEST") == nullptr)
    {
        printf("skipped: set VKHOOK_X11_INPUT_TEST=1 to run (it synthesises real input)\n");
        return 77;
    }
    if (XOpenDisplay(NULL) == nullptr)
    {
        printf("skipped: no X display\n");
        return 77;
    }

    const char* libPath = argc > 1 ? argv[1] : "build/src/libvkhook.so";
    void* lib = dlopen(libPath, RTLD_NOW);
    if (!lib) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    InitFn        initInputBlocker        = (InitFn) dlsym(lib, "_ZN6VkHook16initInputBlockerEb");
    SetWindowFn   setX11Window            = (SetWindowFn) dlsym(lib, "_ZN6VkHook12setX11WindowEm");
    SetRectsFn    setInputRects           = (SetRectsFn) dlsym(lib, "_ZN6VkHook13setInputRectsEPKNS_9InputRectEi");
    SetBoolFn     setInputBlocked         = (SetBoolFn) dlsym(lib, "_ZN6VkHook15setInputBlockedEb");
    SetBoolFn     setInputInteractive     = (SetBoolFn) dlsym(lib, "_ZN6VkHook19setInputInteractiveEb");
    SetBoolFn     setOverlayWantsKeyboard = (SetBoolFn) dlsym(lib, "_ZN6VkHook23setOverlayWantsKeyboardEb");
    SetPosFn      updatePointerPosition   = (SetPosFn) dlsym(lib, "_ZN6VkHook21updatePointerPositionEff");
    GetMouseFn getMouseState = (GetMouseFn) dlsym(lib, "_ZN6VkHook16getMouseStateX11Ev");
    if (!initInputBlocker || !setX11Window || !setInputRects || !setInputBlocked
        || !setInputInteractive || !setOverlayWantsKeyboard || !updatePointerPosition || !getMouseState)
    {
        printf("missing symbols: %s\n", dlerror());
        return 1;
    }

    Display* game = XOpenDisplay(NULL);
    Display* probe = XOpenDisplay(NULL);
    if (!game || !probe) { printf("no display\n"); return 1; }

    const Window root = DefaultRootWindow(game);
    const Window win = XCreateSimpleWindow(game, root, 90, 90, 380, 260, 0, 0, 0x101820);
    XSelectInput(game, win, KeyPressMask | KeyReleaseMask | FocusChangeMask | StructureNotifyMask);
    XStoreName(game, win, "vkhook input test");
    XMapWindow(game, win);
    XSync(game, False);
    usleep(300000);
    int p = 0, r = 0, f = 0;
    pump(game, "setup", &p, &r, &f);
    XSetInputFocus(game, win, RevertToParent, CurrentTime);
    XSync(game, False);
    usleep(150000);
    pump(game, "setup", &p, &r, &f);

    const KeyCode w = XKeysymToKeycode(game, XK_w);

    setX11Window((unsigned long)win);
    initInputBlocker(true);
    struct InputRect rect = {0.0f, 0.0f, 380.0f, 260.0f};
    setInputRects(&rect, 1);
    setInputBlocked(true);
    updatePointerPosition(100.0f, 100.0f);   // pointer over the overlay hitbox

    printf("\n1. pointer over the overlay: pointer grab taken, keyboard left alone\n");
    printf("    pointer grab : %s\n", grabStatus(probe, root, 0));
    printf("    keyboard grab: %s\n", grabStatus(probe, root, 1));
    int ok1 = strcmp(grabStatus(probe, root, 0), "held by the layer") == 0
           && strcmp(grabStatus(probe, root, 1), "free") == 0;

    printf("\n2. a key held across the keyboard grab is released into the game\n");
    XTestFakeKeyEvent(probe, w, True, 0);            // player presses w
    XSync(probe, False);
    usleep(150000);
    p = r = f = 0;
    pump(game, "held", &p, &r, &f);
    if (p == 0) { printf("    game never saw the press; aborting\n"); return 1; }

    updatePointerPosition(500.0f, 500.0f);           // outside every overlay hitbox
    setInputInteractive(true);                    // menu, without text-field focus
    usleep(200000);
    p = r = f = 0;
    pump(game, "grab-engaged", &p, &r, &f);
    printf("    keyboard grab: %s\n", grabStatus(probe, root, 1));
    int ok2 = r > 0 && strcmp(grabStatus(probe, root, 1), "held by the layer") == 0
           && strcmp(grabStatus(probe, root, 0), "held by the layer") == 0;

    printf("\n3. leaving a text field does not release the open menu's capture\n");
    setOverlayWantsKeyboard(true);
    setOverlayWantsKeyboard(false);
    int ok3 = strcmp(grabStatus(probe, root, 1), "held by the layer") == 0
           && strcmp(grabStatus(probe, root, 0), "held by the layer") == 0;

    printf("\n4. captured raw motion is independent of game cursor warps\n");
    XTestFakeRelativeMotionEvent(probe, -10000, -10000, 0);
    XSync(probe, False);
    usleep(50000);
    const auto origin = getMouseState();
    XTestFakeRelativeMotionEvent(probe, 35, 20, 0);
    XSync(probe, False);
    usleep(50000);
    const auto moved = getMouseState();
    XWarpPointer(game, None, win, 0, 0, 0, 0, 190, 130);
    XSync(game, False);
    usleep(50000);
    const auto warped = getMouseState();
    int okMotion = moved.x == origin.x + 35 && moved.y == origin.y + 20
                && warped.x == moved.x && warped.y == moved.y;
    printf("    origin=%d,%d moved=%d,%d after warp=%d,%d\n",
           origin.x, origin.y, moved.x, moved.y, warped.x, warped.y);

    printf("\n5. captured buttons release and wheel ticks are consumed once\n");
    XTestFakeButtonEvent(probe, 1, True, 0);
    XSync(probe, False);
    usleep(50000);
    const auto heldMouse = getMouseState();
    XTestFakeButtonEvent(probe, 1, False, 0);
    XTestFakeButtonEvent(probe, 5, True, 0);
    XTestFakeButtonEvent(probe, 5, False, 0);
    XSync(probe, False);
    usleep(50000);
    const auto releasedMouse = getMouseState();
    const auto drainedMouse = getMouseState();
    int okMouse = heldMouse.leftButton && !releasedMouse.leftButton
               && releasedMouse.scrollDelta == -1.0f && drainedMouse.scrollDelta == 0.0f;

    printf("\n6. closing the menu hands still-held keys back\n");
    setInputInteractive(false);
    setInputBlocked(false);
    usleep(200000);
    p = r = f = 0;
    pump(game, "grab-released", &p, &r, &f);
    printf("    keyboard grab: %s\n", grabStatus(probe, root, 1));
    int ok4 = p > 0 && strcmp(grabStatus(probe, root, 1), "free") == 0
           && strcmp(grabStatus(probe, root, 0), "free") == 0;

    // clean up the simulated physical key
    XTestFakeKeyEvent(probe, w, False, 0);
    XSync(probe, False);
    pump(game, "cleanup", &p, &r, &f);

    printf("\nresult: notification=%s entry=%s ownership=%s motion=%s mouse=%s exit=%s\n",
           ok1 ? "PASS" : "FAIL", ok2 ? "PASS" : "FAIL", ok3 ? "PASS" : "FAIL",
           okMotion ? "PASS" : "FAIL", okMouse ? "PASS" : "FAIL", ok4 ? "PASS" : "FAIL");
    printf("%s\n", (ok1 && ok2 && ok3 && okMotion && okMouse && ok4) ? "ALL PASS" : "FAILURES PRESENT");

    XDestroyWindow(game, win);
    XCloseDisplay(probe);
    XCloseDisplay(game);
    return (ok1 && ok2 && ok3 && okMotion && okMouse && ok4) ? 0 : 1;
}
