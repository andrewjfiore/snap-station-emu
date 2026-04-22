/* jstest.c — dynamic SDL2 joystick probe. No link-time SDL dependency;
 * LoadLibrary("SDL2.dll") at runtime, resolve the ~10 functions we need,
 * and dump every joystick event so we can see the ground-truth axis/button
 * numbers for whatever controller the user has plugged in.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#define SDL_INIT_JOYSTICK   0x00000200u
#define SDL_JOYAXISMOTION   0x600
#define SDL_JOYBALLMOTION   0x601
#define SDL_JOYHATMOTION    0x602
#define SDL_JOYBUTTONDOWN   0x603
#define SDL_JOYBUTTONUP     0x604
#define SDL_JOYDEVICEADDED  0x605
#define SDL_JOYDEVICEREMOVED 0x606
#define SDL_QUIT_EVENT      0x100

typedef struct SDL_Joystick SDL_Joystick;

/* 56-byte SDL_Event union is large; we just need the tag + a few payloads. */
typedef struct {
    uint32_t type;
    uint32_t timestamp;
    int32_t  which;
    uint8_t  button;   /* button for button events, axis for axis events */
    uint8_t  state;
    uint8_t  pad1, pad2;
    int16_t  value;
    uint16_t pad3;
} JoyPayload;

typedef union {
    uint32_t   type;
    JoyPayload jp;
    uint8_t    padding[64];
} SDL_Event;

static int  (*pSDL_Init)(uint32_t);
static void (*pSDL_Quit)(void);
static int  (*pSDL_NumJoysticks)(void);
static SDL_Joystick *(*pSDL_JoystickOpen)(int);
static const char *(*pSDL_JoystickName)(SDL_Joystick *);
static int  (*pSDL_JoystickNumAxes)(SDL_Joystick *);
static int  (*pSDL_JoystickNumButtons)(SDL_Joystick *);
static int  (*pSDL_JoystickNumHats)(SDL_Joystick *);
static int  (*pSDL_PollEvent)(SDL_Event *);
static void (*pSDL_JoystickUpdate)(void);
static const char *(*pSDL_GetError)(void);

static int resolve(HMODULE dll)
{
#define R(name) do { p##name = (void*)GetProcAddress(dll, #name); \
    if (!p##name) { fprintf(stderr, "missing %s\n", #name); return 0; } } while (0)
    R(SDL_Init);
    R(SDL_Quit);
    R(SDL_NumJoysticks);
    R(SDL_JoystickOpen);
    R(SDL_JoystickName);
    R(SDL_JoystickNumAxes);
    R(SDL_JoystickNumButtons);
    R(SDL_JoystickNumHats);
    R(SDL_PollEvent);
    R(SDL_JoystickUpdate);
    R(SDL_GetError);
#undef R
    return 1;
}

int main(void)
{
    HMODULE dll = LoadLibraryA("SDL2.dll");
    if (!dll) {
        fprintf(stderr, "LoadLibrary(SDL2.dll) failed: err=%lu\n", GetLastError());
        return 1;
    }
    if (!resolve(dll)) return 2;

    if (pSDL_Init(SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", pSDL_GetError());
        return 3;
    }

    int n = pSDL_NumJoysticks();
    printf("SDL_NumJoysticks = %d\n", n);
    if (n <= 0) {
        fprintf(stderr, "no joysticks\n");
        return 4;
    }
    SDL_Joystick *js = pSDL_JoystickOpen(0);
    if (!js) {
        fprintf(stderr, "SDL_JoystickOpen: %s\n", pSDL_GetError());
        return 5;
    }
    printf("  name    : %s\n", pSDL_JoystickName(js));
    printf("  axes    : %d\n", pSDL_JoystickNumAxes(js));
    printf("  buttons : %d\n", pSDL_JoystickNumButtons(js));
    printf("  hats    : %d\n", pSDL_JoystickNumHats(js));
    printf("\nPress buttons / move sticks. 20-second window. Ctrl+C to exit early.\n\n");
    fflush(stdout);

    time_t start = time(NULL);
    SDL_Event e;
    while (time(NULL) - start < 20) {
        pSDL_JoystickUpdate();
        while (pSDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_JOYBUTTONDOWN:
                printf("  BUTTON %2u  DOWN\n", e.jp.button); fflush(stdout); break;
            case SDL_JOYBUTTONUP:
                printf("  BUTTON %2u  up\n",   e.jp.button); fflush(stdout); break;
            case SDL_JOYAXISMOTION:
                if (e.jp.value > 8000 || e.jp.value < -8000) {
                    printf("  AXIS   %2u  = %6d\n", e.jp.button, e.jp.value);
                    fflush(stdout);
                }
                break;
            case SDL_JOYHATMOTION:
                printf("  HAT    %2u  = 0x%02x\n", e.jp.button, e.jp.value & 0xff);
                fflush(stdout); break;
            }
        }
        Sleep(10);
    }

    pSDL_Quit();
    return 0;
}
