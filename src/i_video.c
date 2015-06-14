/*
========================================================================

                               DOOM RETRO
         The classic, refined DOOM source port. For Windows PC.

========================================================================

  Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
  Copyright (C) 2013-2015 Brad Harding.

  DOOM RETRO is a fork of CHOCOLATE DOOM by Simon Howard.
  For a complete list of credits, see the accompanying AUTHORS file.

  This file is part of DOOM RETRO.

  DOOM RETRO is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation, either version 3 of the License, or (at your
  option) any later version.

  DOOM RETRO is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with DOOM RETRO. If not, see <http://www.gnu.org/licenses/>.

  DOOM is a registered trademark of id Software LLC, a ZeniMax Media
  company, in the US and/or other countries and is used without
  permission. All other trademarks are the property of their respective
  holders. DOOM RETRO is in no way affiliated with nor endorsed by
  id Software LLC.

========================================================================
*/

#include "c_console.h"
#include "d_deh.h"
#include "d_main.h"
#include "doomstat.h"
#include "hu_stuff.h"
#include "i_gamepad.h"
#include "i_system.h"
#include "i_tinttab.h"
#include "i_video.h"
#include "m_config.h"
#include "m_menu.h"
#include "m_misc.h"
#include "SDL.h"
#include "s_sound.h"
#include "v_video.h"
#include "version.h"
#include "w_wad.h"
#include "z_zone.h"

#if defined(WIN32)
#include "SDL_syswm.h"
#endif

#if !defined(_MSC_VER)
#define __forceinline inline __attribute__((always_inline))
#endif

// Window position:
char                    *windowposition = WINDOWPOSITION_DEFAULT;

SDL_Surface             *screen = NULL;

SDL_Surface             *screenbuffer = NULL;

// palette
SDL_Color               palette[256];
static boolean          palette_to_set;

// Bit mask of mouse button state
static unsigned int     mouse_button_state = 0;

boolean                 novert = NOVERT_DEFAULT;

static int              buttons[MAX_MOUSE_BUTTONS + 1] = { 0, 1, 4, 2, 8, 16, 32, 64, 128 };

// Fullscreen width and height
int                     screenwidth;
int                     screenheight;
char                    *screenresolution = SCREENRESOLUTION_DEFAULT;

// Window width and height
int                     windowwidth;
int                     windowheight;
char                    *windowsize = WINDOWSIZE_DEFAULT;

int                     displaywidth;
int                     displayheight;
int                     displaycenterx;
int                     displaycentery;

// Run in full screen mode?
boolean                 fullscreen = FULLSCREEN_DEFAULT;

boolean                 widescreen = WIDESCREEN_DEFAULT;
boolean                 returntowidescreen = false;

boolean                 widescreenresize = false;

boolean                 hud = HUD_DEFAULT;

boolean                 capfps = CAPFPS_DEFAULT;

// Flag indicating whether the screen is currently visible:
// when the screen isn't visible, don't render the screen
boolean                 screenvisible;

boolean                 window_focused;

// Empty mouse cursor
static SDL_Cursor       *cursors[2];

// Window resize state.
static boolean          need_resize = false;
static unsigned int     resize_h;

int                     desktopwidth;
int                     desktopheight;

char                    *videodriver = VIDEODRIVER_DEFAULT;
char                    envstring[255];

static int              width;
static int              height;
static int              stepx;
static int              stepy;
static int              startx;
static int              starty;

static int              blitheight = SCREENHEIGHT << FRACBITS;

static int              pitch;
byte                    *pixels;

byte                    *rows[SCREENHEIGHT];

boolean                 keys[UCHAR_MAX];

byte                    gammatable[GAMMALEVELS][256];

float                   gammalevels[GAMMALEVELS] =
{
    // Darker
    0.50f, 0.55f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f,

    // No gamma correction
    1.0f,

    // Lighter
    1.05f, 1.10f, 1.15f, 1.20f, 1.25f, 1.30f, 1.35f, 1.40f, 1.45f, 1.50f,
    1.55f, 1.60f, 1.65f, 1.70f, 1.75f, 1.80f, 1.85f, 1.90f, 1.95f, 2.0f
};

// Gamma correction level to use
int                     gammaindex;
float                   gammalevel = GAMMALEVEL_DEFAULT;

SDL_Rect                src_rect = { 0, 0, 0, 0 };

SDL_Rect                dest_rect = { 0, 0, 0, 0 };

boolean                 vid_showfps = false;
int                     fps = 0;

// Mouse acceleration
//
// This emulates some of the behavior of DOS mouse drivers by increasing
// the speed when the mouse is moved fast.
//
// The mouse input values are input directly to the game, but when
// the values exceed the value of mouse_threshold, they are multiplied
// by mouse_acceleration to increase the speed.
float                   mouse_acceleration = MOUSEACCELERATION_DEFAULT;
int                     mouse_threshold = MOUSETHRESHOLD_DEFAULT;

int                     capslock;
boolean                 alwaysrun = ALWAYSRUN_DEFAULT;

void SetWindowPositionVars(void);

boolean MouseShouldBeGrabbed(void)
{
    // if the window doesn't have focus, never grab it
    if (!window_focused)
        return false;

    // always grab the mouse when full screen (dont want to
    // see the mouse pointer)
    if (fullscreen)
        return true;

    // when menu is active or game is paused, release the mouse
    if (menuactive || paused)
        return false;

    // only grab mouse when playing levels
    return (gamestate == GS_LEVEL);
}

// Update the value of window_focused when we get a focus event
//
// We try to make ourselves be well-behaved: the grab on the mouse
// is removed if we lose focus (such as a popup window appearing),
// and we dont move the mouse around if we aren't focused either.
static void UpdateFocus(void)
{
    Uint8               state = SDL_GetAppState();

    // Should the screen be grabbed?
    screenvisible = (state & SDL_APPACTIVE);

    // We should have input (keyboard) focus and be visible
    // (not minimized)
    window_focused = ((state & SDL_APPINPUTFOCUS) && screenvisible);

    if (!window_focused && !menuactive && gamestate == GS_LEVEL && !paused && !consoleactive)
    {
        sendpause = true;
        blurred = false;
    }
}

// Show or hide the mouse cursor. We have to use different techniques
// depending on the OS.
static void SetShowCursor(boolean show)
{
    // On Windows, using SDL_ShowCursor() adds lag to the mouse input,
    // so work around this by setting an invisible cursor instead. On
    // other systems, it isn't possible to change the cursor, so this
    // hack has to be Windows-only. (Thanks to entryway for this)
#if defined(WIN32)
    SDL_SetCursor(cursors[show]);
#else
    SDL_ShowCursor(show);
#endif

    // When the cursor is hidden, grab the input.
    SDL_WM_GrabInput(!show);
}

int translatekey[] =
{
    0, 0, 0, 0, 0, 0, 0, 0, KEY_BACKSPACE, KEY_TAB, 0, 0, 0, KEY_ENTER, 0, 0,
    0, 0, 0, KEY_PAUSE, 0, 0, 0, 0, 0, 0, 0, KEY_ESCAPE, 0, 0, 0, 0, ' ', '!',
    '\"', '#', '$', '%', '&', '\'', '(', ')', '*', '+', ',', KEY_MINUS, '.',
    '/', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<',
    KEY_EQUALS, '>', '?', '@', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i',
    'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
    'y', 'z', '[', '\\', ']', '^', '_', '`', 'a', 'b', 'c', 'd', 'e', 'f', 'g',
    'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', 0, 0, 0, 0, KEY_DEL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEYP_0, KEYP_1, KEYP_2,
    KEYP_3, KEYP_4, KEYP_5, KEYP_6, KEYP_7, KEYP_8, KEYP_9, KEYP_PERIOD,
    KEYP_DIVIDE, KEYP_MULTIPLY, KEYP_MINUS, KEYP_PLUS, KEYP_ENTER, KEYP_EQUALS,
    KEY_UPARROW, KEY_DOWNARROW, KEY_RIGHTARROW, KEY_LEFTARROW, KEY_INS,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_F1, KEY_F2, KEY_F3, KEY_F4,
    KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, 0, 0, 0,
    0, 0, 0, KEY_NUMLOCK, KEY_CAPSLOCK, KEY_SCRLCK, KEY_RSHIFT, KEY_RSHIFT,
    KEY_RCTRL, KEY_RCTRL, KEY_RALT, KEY_RALT, KEY_RALT, KEY_RALT, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

int TranslateKey2(int key)
{
    switch (key)
    {
        case KEY_LEFTARROW:    return SDLK_LEFT;
        case KEY_RIGHTARROW:   return SDLK_RIGHT;
        case KEY_DOWNARROW:    return SDLK_DOWN;
        case KEY_UPARROW:      return SDLK_UP;
        case KEY_ESCAPE:       return SDLK_ESCAPE;
        case KEY_ENTER:        return SDLK_RETURN;
        case KEY_TAB:          return SDLK_TAB;
        case KEY_F1:           return SDLK_F1;
        case KEY_F2:           return SDLK_F2;
        case KEY_F3:           return SDLK_F3;
        case KEY_F4:           return SDLK_F4;
        case KEY_F5:           return SDLK_F5;
        case KEY_F6:           return SDLK_F6;
        case KEY_F7:           return SDLK_F7;
        case KEY_F8:           return SDLK_F8;
        case KEY_F9:           return SDLK_F9;
        case KEY_F10:          return SDLK_F10;
        case KEY_F11:          return SDLK_F11;
        case KEY_F12:          return SDLK_F12;
        case KEY_BACKSPACE:    return SDLK_BACKSPACE;
        case KEY_DEL:          return SDLK_DELETE;
        case KEY_PAUSE:        return SDLK_PAUSE;
        case KEY_EQUALS:       return SDLK_EQUALS;
        case KEY_MINUS:        return SDLK_MINUS;
        case KEY_RSHIFT:       return SDLK_RSHIFT;
        case KEY_RCTRL:        return SDLK_RCTRL;
        case KEY_RALT:         return SDLK_RALT;
        case KEY_CAPSLOCK:     return SDLK_CAPSLOCK;
        case KEY_SCRLCK:       return SDLK_SCROLLOCK;
        case KEYP_0:           return SDLK_KP0;
        case KEYP_1:           return SDLK_KP1;
        case KEYP_3:           return SDLK_KP3;
        case KEYP_5:           return SDLK_KP5;
        case KEYP_7:           return SDLK_KP7;
        case KEYP_9:           return SDLK_KP9;
        case KEYP_PERIOD:      return SDLK_KP_PERIOD;
        case KEYP_MULTIPLY:    return SDLK_KP_MULTIPLY;
        case KEYP_DIVIDE:      return SDLK_KP_DIVIDE;
        case KEY_INS:          return SDLK_INSERT;
        case KEY_NUMLOCK:      return SDLK_NUMLOCK;
        default:               return key;
    }
}

boolean keystate(int key)
{
    Uint8       *keystate = SDL_GetKeyState(NULL);

    return keystate[TranslateKey2(key)];
}

void I_SaveWindowPosition(void)
{
#if defined(WIN32)
    SDL_SysWMinfo       info;

    SDL_VERSION(&info.version);

    if (SDL_GetWMInfo(&info))
    {
        HWND    hwnd = info.window;
        RECT    r;

        GetWindowRect(hwnd, &r);
        sprintf(windowposition, "%i,%i", r.left + 8, r.top + 30);
        M_SaveDefaults();
    }
#endif
}

void RepositionWindow(int amount)
{
#if defined(WIN32)
    SDL_SysWMinfo       info;

    SDL_VERSION(&info.version);

    if (SDL_GetWMInfo(&info))
    {
        HWND    hwnd = info.window;
        RECT    r;

        GetWindowRect(hwnd, &r);
        SetWindowPos(hwnd, NULL, r.left + amount, r.top, 0, 0, SWP_NOSIZE);
    }
#endif
}

static void FreeSurfaces(void)
{
    SDL_FreeSurface(screenbuffer);
}

void I_ShutdownGraphics(void)
{
    SetShowCursor(true);
    FreeSurfaces();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void I_ShutdownKeyboard(void)
{
#if defined(WIN32)
    if ((GetKeyState(VK_CAPITAL) & 0x0001) && !capslock)
    {
        keybd_event(VK_CAPITAL, 0x45, KEYEVENTF_EXTENDEDKEY, (uintptr_t)0);
        keybd_event(VK_CAPITAL, 0x45, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, (uintptr_t)0);
    }
#endif
}

static int AccelerateMouse(int val)
{
    if (val < 0)
        return -AccelerateMouse(-val);

    if (val > mouse_threshold)
        return (int)((val - mouse_threshold) * mouse_acceleration + mouse_threshold);
    else
        return val;
}

// Warp the mouse back to the middle of the screen
static void CenterMouse(void)
{
    // Warp to the screen center
    SDL_WarpMouse(displaycenterx, displaycentery);

    // Clear any relative movement caused by warping
    SDL_PumpEvents();
    SDL_GetRelativeMouseState(NULL, NULL);
}

boolean altdown = false;
boolean waspaused = false;

boolean noinput = true;

void I_GetEvent(void)
{
    event_t     event;
    SDL_Event   SDLEvent;
    SDL_Event   *Event = &SDLEvent;

    while (SDL_PollEvent(Event))
    {
        switch (Event->type)
        {
            case SDL_KEYDOWN:
                if (noinput)
                    return;

                event.type = ev_keydown;

                event.data1 = translatekey[Event->key.keysym.sym];
                event.data2 = tolower(Event->key.keysym.unicode);

                altdown = (Event->key.keysym.mod & KMOD_ALT);

                if (event.data1)
                {
                    if (altdown && event.data1 == KEY_TAB)
                        event.data1 = event.data2 = 0;

                    if (!isdigit(event.data2))
                        idclev = idmus = false;

                    if (idbehold && keys[event.data2])
                    {
                        HU_clearMessages();
                        idbehold = false;
                    }

                    D_PostEvent(&event);
                }
                break;

            case SDL_KEYUP:
                event.type = ev_keyup;

                event.data1 = translatekey[Event->key.keysym.sym];

                altdown = (Event->key.keysym.mod & KMOD_ALT);
                keydown = 0;

                if (event.data1)
                    D_PostEvent(&event);
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (mousesensitivity || menuactive)
                {
                    idclev = false;
                    idmus = false;
                    if (idbehold)
                    {
                        HU_clearMessages();
                        idbehold = false;
                    }
                    event.type = ev_mouse;
                    mouse_button_state |= buttons[Event->button.button];
                    event.data1 = mouse_button_state;
                    event.data2 = 0;
                    event.data3 = 0;
                    D_PostEvent(&event);
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (mousesensitivity || menuactive)
                {
                    keydown = 0;
                    event.type = ev_mouse;
                    mouse_button_state &= ~buttons[Event->button.button];
                    event.data1 = mouse_button_state;
                    event.data2 = 0;
                    event.data3 = 0;
                    D_PostEvent(&event);
                }
                break;

            case SDL_JOYBUTTONUP:
                keydown = 0;
                break;

            case SDL_QUIT:
                if (!quitting && !splashscreen)
                {
                    keydown = 0;
                    if (paused)
                    {
                        paused = false;
                        waspaused = true;
                    }
                    S_StartSound(NULL, sfx_swtchn);
                    M_QuitDOOM(0);
                }
                break;

            case SDL_ACTIVEEVENT:
                // need to update our focus state
                UpdateFocus();
                break;

            case SDL_VIDEOEXPOSE:
                palette_to_set = true;
                break;

            case SDL_VIDEORESIZE:
                if (!fullscreen && !widescreenresize)
                {
                    need_resize = true;
                    resize_h = Event->resize.h;
                }
                widescreenresize = false;
                break;

#if defined(WIN32)
            case SDL_SYSWMEVENT:
                if (!fullscreen)
                {
                    if (Event->syswm.msg->msg == WM_MOVE)
                    {
                        I_SaveWindowPosition();
                        SetWindowPositionVars();
                    }
                }
                break;
#endif

            default:
                break;
        }
    }
}

static void I_ReadMouse(void)
{
    int         x, y;
    event_t     ev;

    SDL_GetRelativeMouseState(&x, &y);

    ev.type = ev_mouse;
    ev.data1 = mouse_button_state;
    ev.data2 = AccelerateMouse(x);
    ev.data3 = (novert ? 0 : -AccelerateMouse(y));

    D_PostEvent(&ev);

    if (MouseShouldBeGrabbed())
        CenterMouse();
}

//
// I_StartTic
//
void I_StartTic(void)
{
    I_GetEvent();
    if (mousesensitivity)
        I_ReadMouse();
    gamepadfunc();
}

boolean currently_grabbed = false;

static void UpdateGrab(void)
{
    boolean     grab = MouseShouldBeGrabbed();

    if (grab && !currently_grabbed)
    {
        SetShowCursor(false);
        CenterMouse();
    }
    else if (!grab && currently_grabbed)
    {
        SDL_WarpMouse(displaywidth - 10 * displaywidth / SCREENWIDTH, displayheight - 16);
        SDL_PumpEvents();
        SDL_GetRelativeMouseState(NULL, NULL);
        SetShowCursor(true);
    }

    currently_grabbed = grab;
}

static __forceinline void StretchBlit(void)
{
    fixed_t     i = 0;
    fixed_t     y = starty;

    do
    {
        byte    *dest = pixels + i;
        byte    *src = *(rows + (y >> FRACBITS));
        fixed_t x = startx;

        do
            *dest++ = *(src + (x >> FRACBITS));
        while ((x += stepx) < (SCREENWIDTH << FRACBITS));

        i += pitch;
    } while ((y += stepy) < blitheight);
}

//
// I_FinishUpdate
//
void I_FinishUpdate(void)
{
    if (need_resize)
    {
        ApplyWindowResize(resize_h);
        need_resize = false;
        palette_to_set = true;
    }

    UpdateGrab();

    if (!screenvisible)
        return;

    if (palette_to_set)
    {
        SDL_SetColors(screenbuffer, palette, 0, 256);
        palette_to_set = false;
    }

    StretchBlit();

#if defined(WIN32)
    SDL_FillRect(screen, NULL, 0);
#endif

    SDL_LowerBlit(screenbuffer, &src_rect, screen, &dest_rect);
    SDL_Flip(screen);

    if (vid_showfps)
    {
        static int  frames = -1;
        static int  starttime = 0;
        static int  currenttime;

        ++frames;
        currenttime = SDL_GetTicks();
        if (currenttime - starttime >= 1000)
        {
            fps = frames;
            frames = 0;
            starttime = currenttime;
        }
    }
}

//
// I_ReadScreen
//
void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

//
// I_SetPalette
//
void I_SetPalette(byte *doompalette)
{
    int i;

    for (i = 0; i < 256; ++i)
    {
        palette[i].r = gammatable[gammaindex][*doompalette++];
        palette[i].g = gammatable[gammaindex][*doompalette++];
        palette[i].b = gammatable[gammaindex][*doompalette++];
    }

    palette_to_set = true;
}

static void CreateCursors(void)
{
    static Uint8 empty_cursor_data = 0;

    // Save the default cursor so it can be recalled later
    cursors[1] = SDL_GetCursor();

    // Create an empty cursor
    cursors[0] = SDL_CreateCursor(&empty_cursor_data, &empty_cursor_data, 1, 1, 0, 0);
}

void SetWindowPositionVars(void)
{
    int         x, y;

    if (sscanf(windowposition, "%10i,%10i", &x, &y) == 2)
    {
        char    buf[64];

        if (x < 0)
            x = 0;
        else if (x > desktopwidth)
            x = desktopwidth - 16;
        if (y < 0)
            y = 0;
        else if (y > desktopheight)
            y = desktopheight - 16;
        sprintf(buf, "SDL_VIDEO_WINDOW_POS=%i,%i", x, y);
        putenv(buf);
    }
    else
        putenv("SDL_VIDEO_CENTERED=1");
}

static void GetDesktopDimensions(void)
{
    SDL_VideoInfo       *videoinfo = (SDL_VideoInfo *)SDL_GetVideoInfo();

    desktopwidth = videoinfo->current_w;
    desktopheight = videoinfo->current_h;
}

static void SetupScreenRects(void)
{
    int w = screenbuffer->w;
    int h = screenbuffer->h;
    int dy;

    dest_rect.x = (screen->w - w) / 2;
    dest_rect.y = (widescreen ? 0 : (screen->h - h) / 2);
    dy = dest_rect.y + h - screen->clip_rect.y - screen->clip_rect.h;
    if (dy > 0)
        h -= dy;

    src_rect.w = dest_rect.w = w;
    src_rect.h = dest_rect.h = h;
}

static char *aspectratio(int width, int height)
{
    int hcf = gcd(width, height);

    width /= hcf;
    height /= hcf;

    if (width == 8 && height == 5)
        return "16:10";
    else
    {
        static char     ratio[10];

        M_snprintf(ratio, sizeof(ratio), "%i:%i", width, height);
        return ratio;
    }
}

static void SetVideoMode(boolean output)
{
    int width;
    int height;

    if (fullscreen)
    {
        width = screenwidth;
        height = screenheight;
        if (!width || !height)
        {
            width = desktopwidth;
            height = desktopheight;
            screenwidth = 0;
            screenheight = 0;
            M_SaveDefaults();
        }

        screen = SDL_SetVideoMode(width, height, 0,
            SDL_HWSURFACE | SDL_HWPALETTE | SDL_DOUBLEBUF | SDL_FULLSCREEN);

        if (!screen)
            I_Error("SetVideoMode, line %i: %s\n", __LINE__ - 5, SDL_GetError());

        height = screen->h;
        width = height * 4 / 3;
        width += (width & 1);

        if (width > screen->w)
        {
            width = screen->w;
            height = width * 3 / 4;
            height += (height & 1);
        }
    }
    else
    {
        if (windowheight > desktopheight)
        {
            windowheight = desktopheight;
            windowwidth = windowheight * 4 / 3;
            M_SaveDefaults();
        }
        height = MAX(ORIGINALWIDTH * 3 / 4, windowheight);
        width = height * 4 / 3;

        if (width > windowwidth)
        {
            width = windowwidth;
            height = width * 3 / 4;
        }

        SetWindowPositionVars();

        screen = SDL_SetVideoMode(windowwidth, windowheight, 0,
            SDL_HWSURFACE | SDL_HWPALETTE | SDL_DOUBLEBUF | SDL_RESIZABLE);

        if (!screen)
            I_Error("SetVideoMode, line %i: %s\n", __LINE__ - 5, SDL_GetError());

        widescreen = false;
    }

    displaycenterx = screen->w / 2;
    displaycentery = screen->h / 2;

    screenbuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 8, 0, 0, 0, 0);

    if (!screenbuffer)
        I_Error("SetVideoMode, line %i: %s\n", __LINE__ - 3, SDL_GetError());

    SetupScreenRects();

    pitch = screenbuffer->pitch;
    pixels = (byte *)screenbuffer->pixels;

    stepx = (SCREENWIDTH << FRACBITS) / width;
    stepy = (SCREENHEIGHT << FRACBITS) / height;

    startx = stepx - 1;
    starty = stepy - 1;
}

void ToggleWidescreen(boolean toggle)
{
    if (fullscreen && (double)screenwidth / screenheight < (double)16 / 10)
    {
        widescreen = returntowidescreen = false;
        return;
    }

    height = screen->h;

    if (toggle)
    {
        widescreen = true;

        if (returntowidescreen && screensize == 8)
        {
            screensize = 7;
            R_SetViewSize(screensize);
        }

        height += (int)((double)height * SBARHEIGHT / (SCREENHEIGHT - SBARHEIGHT) + 1.5);
        blitheight = (SCREENHEIGHT - SBARHEIGHT) << FRACBITS;
    }
    else
    {
        widescreen = false;

        blitheight = SCREENHEIGHT << FRACBITS;
    }

    returntowidescreen = false;

    width = height * 4 / 3;

    if (fullscreen)
    {
        width += (width & 1);

        if ((double)width / screen->w >= 0.99)
            width = screen->w;
    }

    if (!fullscreen)
    {
        int     diff = (screen->w - width) / 2;

        widescreenresize = true;

        screen = SDL_SetVideoMode(width, screen->h, 0,
            SDL_HWSURFACE | SDL_HWPALETTE | SDL_DOUBLEBUF | SDL_RESIZABLE);

        if (!screenbuffer)
            I_Error("ToggleWidescreen, line %i: %s\n", __LINE__ - 3, SDL_GetError());

        RepositionWindow(diff);
        windowwidth = screen->w;
        windowheight = screen->h;
    }

    SDL_FreeSurface(screenbuffer);
    screenbuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 8, 0, 0, 0, 0);

    if (!screenbuffer)
        I_Error("ToggleWidescreen, line %i: %s\n", __LINE__ - 3, SDL_GetError());

    SetupScreenRects();

    pitch = screenbuffer->pitch;
    pixels = (byte *)screenbuffer->pixels;

    stepx = (SCREENWIDTH << FRACBITS) / width;
    stepy = (SCREENHEIGHT << FRACBITS) / height;

    startx = stepx - 1;
    starty = stepy - 1;

    palette_to_set = true;
}

void I_RestartGraphics(void)
{
    FreeSurfaces();
    SetVideoMode(false);
    if (widescreen)
        ToggleWidescreen(true);
}

#if defined(WIN32)
void I_InitWindows32(void);
#endif

void ToggleFullscreen(void)
{
    fullscreen = !fullscreen;
    M_SaveDefaults();
    if (fullscreen)
    {
        width = screenwidth;
        height = screenheight;
        if (!width || !height)
        {
            width = desktopwidth;
            height = desktopheight;
            screenwidth = 0;
            screenheight = 0;
            M_SaveDefaults();
        }

        screen = SDL_SetVideoMode(width, height, 0,
            SDL_HWSURFACE | SDL_HWPALETTE | SDL_DOUBLEBUF | SDL_FULLSCREEN);

        if (!screen)
            I_Error("ToggleFullscreen, line %i: %s\n", __LINE__ - 5, SDL_GetError());

        if (widescreen)
        {
            if (gamestate != GS_LEVEL)
                returntowidescreen = true;
            else
            {
                ToggleWidescreen(true);
                if (widescreen)
                    screensize = 7;
                R_SetViewSize(screensize);
                M_SaveDefaults();
                if (widescreen)
                    return;
            }
        }

        height = screen->h;
        width = height * 4 / 3;
        width += (width & 1);

        if (width > screen->w)
        {
            width = screen->w;
            height = width * 3 / 4;
            height += (height & 1);
        }
    }
    else
    {
        if (windowheight > desktopheight)
        {
            windowheight = desktopheight;
            windowwidth = windowheight * 4 / 3;
            M_SaveDefaults();
        }

        height = MAX(ORIGINALWIDTH * 3 / 4, windowheight);
        width = height * 4 / 3;

        if (width > windowwidth)
        {
            width = windowwidth;
            height = width * 3 / 4;
        }

        SetWindowPositionVars();

        screen = SDL_SetVideoMode(width, height, 0,
            SDL_HWSURFACE | SDL_HWPALETTE | SDL_DOUBLEBUF | SDL_RESIZABLE);

        if (!screen)
            I_Error("ToggleFullscreen, line %i: %s\n", __LINE__ - 5, SDL_GetError());

        if (widescreen)
        {
            if (gamestate != GS_LEVEL)
                returntowidescreen = true;
            else
            {
                ToggleWidescreen(true);
                if (widescreen)
                    screensize = 7;
                R_SetViewSize(screensize);
                M_SaveDefaults();
                return;
            }
        }
    }

    SDL_FreeSurface(screenbuffer);
    screenbuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 8, 0, 0, 0, 0);

    if (!screenbuffer)
        I_Error("ToggleFullscreen, line %i: %s\n", __LINE__ - 3, SDL_GetError());

    SetupScreenRects();

    pitch = screenbuffer->pitch;
    pixels = (byte *)screenbuffer->pixels;

    stepx = (SCREENWIDTH << FRACBITS) / width;
    stepy = (SCREENHEIGHT << FRACBITS) / height;

    startx = stepx - 1;
    starty = stepy - 1;
}

void ApplyWindowResize(int resize_h)
{
    windowheight = height = MAX(SCREENWIDTH * 3 / 4, MIN(resize_h, desktopheight));
    windowwidth = windowheight * 4 / 3;

    if (widescreen)
        height += (int)((double)height * SBARHEIGHT / (SCREENHEIGHT - SBARHEIGHT) + 1.5);

    screen = SDL_SetVideoMode(windowwidth, windowheight, 0,
        SDL_HWSURFACE | SDL_HWPALETTE | SDL_DOUBLEBUF | SDL_RESIZABLE);

    if (!screen)
        I_Error("ApplyWindowResize, line %i: %s\n", __LINE__ - 5, SDL_GetError());

    SDL_FreeSurface(screenbuffer);
    screenbuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, windowwidth, height, 8, 0, 0, 0, 0);

    if (!screenbuffer)
        I_Error("ApplyWindowResize, line %i: %s\n", __LINE__ - 3, SDL_GetError());

    SetupScreenRects();

    pitch = screenbuffer->pitch;
    pixels = (byte *)screenbuffer->pixels;

    stepx = (SCREENWIDTH << FRACBITS) / windowwidth;
    stepy = (SCREENHEIGHT << FRACBITS) / height;

    startx = stepx - 1;
    starty = stepy - 1;

    M_SaveDefaults();

    displaywidth = windowwidth;
    displayheight = windowheight;
    displaycenterx = displaywidth / 2;
    displaycentery = displayheight / 2;
}

void I_InitGammaTables(void)
{
    int i;
    int j;

    for (i = 0; i < GAMMALEVELS; i++)
        if (gammalevels[i] == 1.0)
            for (j = 0; j < 256; j++)
                gammatable[i][j] = j;
        else
            for (j = 0; j < 256; j++)
                gammatable[i][j] = (byte)(pow((j + 1) / 256.0, 1.0 / gammalevels[i]) * 255.0);
}

boolean I_ValidScreenMode(int width, int height)
{
    return SDL_VideoModeOK(width, height, 32, SDL_FULLSCREEN);
}

void I_InitKeyboard(void)
{
#if defined(WIN32)
    capslock = (GetKeyState(VK_CAPITAL) & 0x0001);

    if ((alwaysrun && !capslock) || (!alwaysrun && capslock))
    {
        keybd_event(VK_CAPITAL, 0x45, KEYEVENTF_EXTENDEDKEY, (uintptr_t)0);
        keybd_event(VK_CAPITAL, 0x45, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, (uintptr_t)0);
    }
#endif
}

void I_InitGraphics(void)
{
    int         i = 0;
    SDL_Event   dummy;
    byte        *doompal = W_CacheLumpName("PLAYPAL", PU_CACHE);

    putenv("SDL_DISABLE_LOCK_KEYS=1");

    while (i < UCHAR_MAX)
        keys[i++] = true;
    keys['v'] = keys['V'] = false;
    keys['s'] = keys['S'] = false;
    keys['i'] = keys['I'] = false;
    keys['r'] = keys['R'] = false;
    keys['a'] = keys['A'] = false;
    keys['l'] = keys['L'] = false;

    I_InitTintTables(doompal);

    I_InitGammaTables();

    if (videodriver != NULL && strlen(videodriver) > 0)
    {
        M_snprintf(envstring, sizeof(envstring), "SDL_VIDEODRIVER=%s", videodriver);
        putenv(envstring);
    }

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
    {
#if defined(WIN32)
        if (!strcasecmp(videodriver, "directx"))
            M_StringCopy(videodriver, "windib", 7);
        else
            M_StringCopy(videodriver, "directx", 8);
        M_snprintf(envstring, sizeof(envstring), "SDL_VIDEODRIVER=%s", videodriver);
        putenv(envstring);
        M_SaveDefaults();

        if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
#endif
            I_Error("I_InitGraphics, line %i: %s\n", __LINE__ - 2, SDL_GetError());
    }

    CreateCursors();
    SDL_SetCursor(cursors[0]);

    if (fullscreen && (screenwidth || screenheight))
        if (!I_ValidScreenMode(screenwidth, screenheight))
        {
            screenwidth = 0;
            screenheight = 0;
        }

    GetDesktopDimensions();

    SetVideoMode(true);

#if defined(WIN32)
    I_InitWindows32();
#endif

    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);

    SDL_WM_SetCaption(PACKAGE_NAME, NULL);

    I_SetPalette(doompal);

    SDL_SetColors(screenbuffer, palette, 0, 256);

    if (!fullscreen)
        currently_grabbed = true;
    UpdateFocus();
    UpdateGrab();

    screens[0] = Z_Malloc(SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);
    memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);

    for (i = 0; i < SCREENHEIGHT; i++)
        rows[i] = *screens + i * SCREENWIDTH;

    I_FinishUpdate();

    SDL_EnableUNICODE(1);
    SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);

    while (SDL_PollEvent(&dummy));

    if (fullscreen)
        CenterMouse();
}
