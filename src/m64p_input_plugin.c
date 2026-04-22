/* m64p_input_plugin.c
 *
 * Mupen64Plus input-plugin scaffold that presents a Snap Station on
 * controller port 4. Plug this in alongside mupen64plus-input-sdl (which
 * handles ports 1-3 as normal gamepads); the core routes port-4 traffic
 * to this plugin via the same ControllerCommand / ReadController hooks.
 *
 * This file deliberately contains only the Mupen64Plus-facing boilerplate
 * and the glue that funnels JoyBus commands to joybus_snapstation.c. The
 * state machine, photo capture, and print dialog are all elsewhere and
 * platform-agnostic.
 *
 * Build notes:
 *   - Compile as a shared library named mupen64plus-input-snapstation.dll
 *     (Windows) or .so (Linux).
 *   - Drop it in the Mupen64Plus plugin directory.
 *   - Set InputPlugin=mupen64plus-input-snapstation in mupen64plus.cfg,
 *     OR run a forwarding plugin that chains SDL for ports 1-3 and this
 *     plugin for port 4. A simple example wrapper is left as an exercise
 *     - for a standalone Snap Station test you can swap the whole input
 *     plugin and use keyboard polling elsewhere if you don't need port-1
 *     control.
 *
 *   - The screenshot hook: on_capture_photo triggers
 *         CoreDoCommand(M64CMD_TAKE_NEXT_SCREENSHOT, 0, NULL);
 *     which writes a PNG to the configured screenshot directory. The
 *     on_print_ready callback then reads the most-recent 16 PNGs from
 *     that directory, passes them to ss_sheet_compose(), and hands the
 *     result to the Windows print dialog.
 */

#include "joybus_snapstation.h"
#include "smart_card.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#  include <windows.h>
#endif

/* Credits granted per press of the Minus button; user-triggered "insert
 * card" substitute while the PI DOM2 cart-bus hook is unimplemented. */
#define SS_MINUS_CREDIT_GRANT 9

/* --- Mupen64Plus plugin API types (abridged from m64p_plugin_protocol.h). */

#define M64P_PLUGIN_API_VERSION     0x020100
#define M64P_CORE_API_VERSION       0x020001
#define M64PLUGIN_INPUT             4

typedef void *m64p_dynlib_handle;
typedef int   m64p_error;

typedef struct {
    uint16_t Version;
    uint16_t Type;
    char     Name[100];
    int      Reserved1;
    int      Reserved2;
} PLUGIN_INFO;

/* BUTTONS union per mupen64plus-core m64p_plugin.h. */
typedef union {
    uint32_t Value;
    struct {
        unsigned R_DPAD       : 1;
        unsigned L_DPAD       : 1;
        unsigned D_DPAD       : 1;
        unsigned U_DPAD       : 1;
        unsigned START_BUTTON : 1;
        unsigned Z_TRIG       : 1;
        unsigned B_BUTTON     : 1;
        unsigned A_BUTTON     : 1;
        unsigned R_CBUTTON    : 1;
        unsigned L_CBUTTON    : 1;
        unsigned D_CBUTTON    : 1;
        unsigned U_CBUTTON    : 1;
        unsigned R_TRIG       : 1;
        unsigned L_TRIG       : 1;
        unsigned Reserved1    : 1;
        unsigned Reserved2    : 1;
        signed   X_AXIS       : 8;
        signed   Y_AXIS       : 8;
    };
} BUTTONS;

typedef struct {
    int     Present;         /* 1 if connected */
    int     RawData;         /* 1 if raw-data mode */
    int     Plugin;          /* 1=none, 2=mempak, 3=rumble, 4=transferpak, 5=raw */
    int     Type;            /* Extra field present in mupen64plus-core's real
                              * CONTROL struct (absent from some older header
                              * definitions). Must be declared so our stride
                              * (4 ints = 16 bytes) matches SDL's and the
                              * core's stride - otherwise Controls[i] indexing
                              * desynchronizes and our port-4 override lands
                              * in SDL's Controls[2], leaving the real port 4
                              * with RawData=0, which makes the core handle
                              * port-4 JoyBus internally instead of routing
                              * to our ControllerCommand. */
} CONTROL;

/* Real Mupen64Plus ABI: CONTROL_INFO contains a pointer to a core-owned
 * 4-entry CONTROL array. Plugin writes propagate through the pointer. */
typedef struct { CONTROL *Controls; } CONTROL_INFO;

/* We export a minimal subset of the plugin ABI. */
#ifdef _WIN32
#  define EXPORT __declspec(dllexport)
#  define CALL   __cdecl
#else
#  define EXPORT __attribute__((visibility("default")))
#  define CALL
#endif

/* --- Mupen64Plus core debugger / memory API ----------------------------
 * Resolved by GetProcAddress on the core dynlib handle passed to
 * PluginStartup. Used to observe PI register traffic each VI so we can
 * reconcile PI DOM2 smart-card accesses with smart_card.c.                */

/* PI register physical addresses (mirrored at 0x04600000+). */
#define PI_DRAM_ADDR_REG   0xa4600000u
#define PI_CART_ADDR_REG   0xa4600004u
#define PI_RD_LEN_REG      0xa4600008u
#define PI_WR_LEN_REG      0xa460000cu
#define PI_STATUS_REG      0xa4600010u

/* Smart-card backing window on the cart bus. */
#define SC_PI_BASE         0x08000000u
#define SC_PI_END          0x0801ffffu   /* 128 KB window */

typedef uint32_t (*ptr_DebugMemRead32)(uint32_t addr);
typedef void     (*ptr_DebugMemWrite32)(uint32_t addr, uint32_t value);
typedef uint8_t  (*ptr_DebugMemRead8)(uint32_t addr);
typedef void     (*ptr_DebugMemWrite8)(uint32_t addr, uint8_t value);
typedef void    *(*ptr_DebugMemGetPointer)(int mem_ptr_type);
typedef int      (*ptr_CoreDoCommand)(int cmd, int param_int, void *param_ptr);

/* M64CMD values we use (from m64p_types.h). */
#define M64CMD_SET_FRAME_CALLBACK 17

static struct {
    ptr_DebugMemRead32     Read32;
    ptr_DebugMemWrite32    Write32;
    ptr_DebugMemRead8      Read8;
    ptr_DebugMemWrite8     Write8;
    ptr_DebugMemGetPointer GetPointer;
    ptr_CoreDoCommand      DoCommand;
    int                    resolved;
} g_dbg;

static void resolve_dbg(void *core_dynlib)
{
#ifdef _WIN32
    if (g_dbg.resolved) return;
    HMODULE h = (HMODULE)core_dynlib;
    if (!h) h = GetModuleHandleA("mupen64plus.dll");
    if (!h) return;
    g_dbg.Read32       = (ptr_DebugMemRead32)    GetProcAddress(h, "DebugMemRead32");
    g_dbg.Write32      = (ptr_DebugMemWrite32)   GetProcAddress(h, "DebugMemWrite32");
    g_dbg.Read8        = (ptr_DebugMemRead8)     GetProcAddress(h, "DebugMemRead8");
    g_dbg.Write8       = (ptr_DebugMemWrite8)    GetProcAddress(h, "DebugMemWrite8");
    g_dbg.GetPointer   = (ptr_DebugMemGetPointer)GetProcAddress(h, "DebugMemGetPointer");
    g_dbg.DoCommand    = (ptr_CoreDoCommand)     GetProcAddress(h, "CoreDoCommand");
    g_dbg.resolved = 1;
    fprintf(stderr, "[snapstation-input] dbg: Read32=%p Write32=%p Read8=%p Write8=%p "
                    "GetPointer=%p DoCommand=%p\n",
            (void*)g_dbg.Read32, (void*)g_dbg.Write32,
            (void*)g_dbg.Read8, (void*)g_dbg.Write8,
            (void*)g_dbg.GetPointer, (void*)g_dbg.DoCommand);
    fflush(stderr);
#endif
}

/* --- Frame callback: poll PI regs each frame and look for DOM2 DMAs. */

/* State declared early (before on_core_frame) because the per-VI frame
 * poll needs to reference g_station (for jbs_tick) and the smart-card
 * state (for on_print_ready's credit decrement). */
static jbs_printer_t *g_station;
static sc_reader_t   *g_sc_reader;
static sc_card_t     *g_sc_card;

/* Credit-search state (used by on_core_frame's per-frame poker and by
 * GetKeys's Home/Plus/Minus narrowing logic). */
static uint32_t  g_search_lo;
static uint32_t  g_search_hi;
static int       g_search_active;
static uint8_t  *g_search_backup;  /* 8MB buffer, allocated on first use */
static uint32_t  g_search_backup_lo;
static uint32_t  g_search_backup_hi;
static int       g_search_has_backup;

/* Vision-search kick flag. Set by the Capture button (SDL 15) to tell
 * the vision loop "use this frame as the baseline - I'm now on the
 * target screen". The loop clears it once it has re-entered V_INIT. */
static volatile int g_vision_reset;

/* Memory block types for DebugMemGetPointer. */
#define M64P_DBG_PTR_RDRAM   1
#define M64P_DBG_PTR_PI_REG  2
#define M64P_DBG_PTR_SI_REG  3
#define M64P_DBG_PTR_VI_REG  4
#define M64P_DBG_PTR_RI_REG  5
#define M64P_DBG_PTR_AI_REG  6

/* PI register indices inside the block (each is a 32-bit word). */
#define PI_DRAM_ADDR_IDX   0
#define PI_CART_ADDR_IDX   1
#define PI_RD_LEN_IDX      2
#define PI_WR_LEN_IDX      3
#define PI_STATUS_IDX      4

static void on_core_frame(unsigned int frame_index)
{
    static unsigned long s_vi;
    static uint32_t s_last_cart, s_last_dram, s_last_status;
    static uint32_t *s_pi_regs;
    static uint8_t  *s_rdram;
    s_vi++;
    (void)frame_index;

    /* Tick the JoyBus state machine once per VI so the post-0x02 busy
     * stall actually decays - otherwise READ 0xC000 returns 0x08 (busy)
     * forever and the ROM spins in snapstation_signal_photo_ready's
     * poll loop on the very first photo. */
    if (g_station) jbs_tick(g_station);

    /* Resolve direct pointers once; DebugMemRead32 is stubbed in RMG's
     * core so we can't use it. DebugMemGetPointer should still return
     * the actual buffer pointers since it's not debugger-gated. */
    if (!s_pi_regs && g_dbg.GetPointer) {
        s_pi_regs = (uint32_t *)g_dbg.GetPointer(M64P_DBG_PTR_PI_REG);
        s_rdram   = (uint8_t  *)g_dbg.GetPointer(M64P_DBG_PTR_RDRAM);
        fprintf(stderr, "[snapstation-input] dbg: PI_REG ptr=%p RDRAM ptr=%p\n",
                (void*)s_pi_regs, (void*)s_rdram);
        fflush(stderr);
    }
    if (!s_pi_regs) return;

    uint32_t dram   = s_pi_regs[PI_DRAM_ADDR_IDX];
    uint32_t cart   = s_pi_regs[PI_CART_ADDR_IDX];
    uint32_t status = s_pi_regs[PI_STATUS_IDX];

    if (s_vi == 1 || (s_vi % 120) == 0) {
        uint32_t ram0 = s_rdram ? *(uint32_t*)s_rdram : 0;
        fprintf(stderr, "[snapstation-input] pi[f=%lu]: CART=0x%08x DRAM=0x%08x "
                        "STATUS=0x%08x  RDRAM[0]=0x%08x\n",
                s_vi, cart, dram, status, ram0);
        fflush(stderr);
    }

    /* DOM2 watch: log every NEW cart access inside the Snap Station smart-
     * card window (0x08000000-0x0801FFFF). The DRAM address captured here
     * is where NPHE's osEPiStartDma landed the card image - hence where
     * the in-RAM credit cache lives. Check every frame; NPHE's DOM2 DMAs
     * are transient so we want to spot them the first time they appear. */
    if (cart >= SC_PI_BASE && cart <= SC_PI_END) {
        static uint32_t s_last_dom2_cart;
        static uint32_t s_last_dom2_dram;
        if (cart != s_last_dom2_cart || dram != s_last_dom2_dram) {
            uint32_t rd_len = s_pi_regs[PI_RD_LEN_IDX];
            uint32_t wr_len = s_pi_regs[PI_WR_LEN_IDX];
            fprintf(stderr,
                "[snapstation-input] DOM2 access @ f=%lu: CART=0x%08x "
                "DRAM=0x%08x RD_LEN=0x%08x WR_LEN=0x%08x STATUS=0x%08x\n",
                s_vi, cart, dram, rd_len, wr_len, status);
            fflush(stderr);
            s_last_dom2_cart = cart;
            s_last_dom2_dram = dram;
        }
    }

    /* Credit-variable binary search. Plugin maintains a current RAM
     * range [s_search_lo, s_search_hi]. Every frame, any 4-byte-aligned
     * u32 currently equal to 0 inside that range is written to 9
     * (mupen-LE, so N64 BE reads see the value 9). A backup of the
     * range is kept so narrowing (or exiting) restores the overwritten
     * bytes. User drives the search via Home/Plus/Minus (see GetKeys).
     *
     * Gated on SNAPSTATION_CREDIT_SEARCH=1 so it's off by default. */
    if (s_rdram) {
        static int s_cs_enabled = -1;
        static int s_vs_enabled_cached = -1;
        if (s_cs_enabled < 0) {
            const char *e = getenv("SNAPSTATION_CREDIT_SEARCH");
            s_cs_enabled = (e && e[0]) ? 1 : 0;
            fprintf(stderr, "[snapstation-input] credit-search=%d\n",
                    s_cs_enabled);
        }
        if (s_vs_enabled_cached < 0) {
            const char *e = getenv("SNAPSTATION_VISION_SEARCH");
            s_vs_enabled_cached = (e && e[0]) ? 1 : 0;
        }
        if ((s_cs_enabled || s_vs_enabled_cached) && g_search_active) {
            for (uint32_t a = g_search_lo; a + 4 <= g_search_hi + 1; a += 4) {
                if (s_rdram[a] == 0 && s_rdram[a + 1] == 0
                    && s_rdram[a + 2] == 0 && s_rdram[a + 3] == 0) {
                    s_rdram[a] = 0x09;  /* u32 LE = 0x00000009 */
                    /* other 3 bytes are already 0 */
                }
            }
        }
    }

    /* (RDRAM snapshot writing moved to take_ram_snapshot() below, and is
     * now driven by Plus/Minus button edge events from GetKeys instead of
     * fixed frame numbers - much more useful for RAM diffing.) */
    (void)s_rdram;

#ifdef _WIN32
    /* ------------------------------------------------------------------
     * Autonomous vision-driven binary search for the "PRINT is enabled"
     * gate in RDRAM. Activated by SNAPSTATION_VISION_SEARCH=1.
     *
     * Loop, per state:
     *   V_INIT         - one-shot: allocate backup buf, snap baseline,
     *                    set range to full 8MB.
     *   V_BASELINE     - wait for baseline PNG to land, measure mean
     *                    HSV saturation in a PRINT-button ROI as the
     *                    "disabled" reference.
     *   V_POKE_HOLD    - every frame, re-poke all zero u32 in the
     *                    current range with 9 (so the game can't erase
     *                    our write before rendering). After N frames,
     *                    fire a screenshot.
     *   V_MEASURE      - wait for post-poke PNG, measure the same ROI.
     *                    If saturation delta > threshold, the button
     *                    lit up - bisect lower half. Otherwise, swap to
     *                    upper half. Restore bytes from backup before
     *                    every bisect step. Stop when range < 256 B.
     *
     * ROI assumes the classic Snap-PRINT layout: sidebar button near
     * the bottom-left, ~4-21% of framebuffer width and ~73-79% of
     * height. Refined after first calibration if needed.
     * ------------------------------------------------------------------ */
    if (s_rdram && g_dbg.GetPointer) {
        static int s_vs_enabled = -1;
        if (s_vs_enabled < 0) {
            const char *e = getenv("SNAPSTATION_VISION_SEARCH");
            s_vs_enabled = (e && e[0]) ? 1 : 0;
            fprintf(stderr, "[snapstation-input] vision-search=%d\n",
                    s_vs_enabled);
        }

        if (s_vs_enabled) {
            enum {
                V_INIT = 0, V_BASELINE, V_POKE_HOLD, V_MEASURE, V_DONE
            };
            /* ROI (percent of framebuffer w/h) - covers the PRINT button
             * sidebar position. */
            static const float ROI_X0 = 0.04f, ROI_Y0 = 0.73f;
            static const float ROI_X1 = 0.22f, ROI_Y1 = 0.80f;
            /* Enabled when measured_sat exceeds baseline by this delta. */
            static const float LIT_DELTA = 0.10f;
            /* Frame budgets. */
            static const int POKE_HOLD_FRAMES   = 60;   /* ~1 s */
            static const int MEASURE_TIMEOUT    = 180;  /* ~3 s */
            static const int BASELINE_SETTLE    = 60;   /* ~1 s */
            /* Minimum range width before declaring victory. */
            static const uint32_t STOP_WIDTH = 0x100;

            /* Start idle - user presses Capture on the Gallery screen
             * to kick off the search. Otherwise the baseline gets
             * sampled from the boot logo and everything downstream is
             * meaningless. */
            static int       s_phase = V_DONE;
            static int       s_frame_in_phase;
            static float     s_baseline_sat = -1.0f;
            static FILETIME  s_last_shot_ft;
            static int       s_iter;

            s_frame_in_phase++;

            /* Capture-button asked us to rebaseline. Restore any poked
             * bytes first so the game state is pristine for the user's
             * "PRINT is grayed" reference frame. */
            if (g_vision_reset) {
                g_vision_reset = 0;
                if (s_rdram && g_search_has_backup) {
                    uint32_t len = g_search_backup_hi
                                 - g_search_backup_lo + 1;
                    memcpy(s_rdram + g_search_backup_lo,
                           g_search_backup, len);
                }
                s_phase = V_INIT;
                s_frame_in_phase = 0;
                s_iter = 0;
                fprintf(stderr,
                    "[vision] reset requested; re-entering INIT\n");
                fflush(stderr);
            }

            if (s_phase == V_INIT) {
                if (!g_search_backup) g_search_backup = malloc(0x800000);
                g_search_lo = 0x00000000;
                g_search_hi = 0x007FFFFF;
                g_search_backup_lo = g_search_lo;
                g_search_backup_hi = g_search_hi;
                memcpy(g_search_backup, s_rdram + g_search_lo, 0x800000);
                g_search_has_backup = 1;
                g_search_active    = 0;  /* don't poke during baseline */
                memset(&s_last_shot_ft, 0, sizeof(s_last_shot_ft));
                fprintf(stderr,
                    "[vision] INIT -> baseline snap (no pokes)\n");
                fflush(stderr);
                m64p_ss_vision_snap();
                s_phase = V_BASELINE;
                s_frame_in_phase = 0;
            }
            else if (s_phase == V_BASELINE) {
                if (s_frame_in_phase >= BASELINE_SETTLE) {
                    float s = m64p_ss_vision_measure_roi(
                        &s_last_shot_ft, ROI_X0, ROI_Y0, ROI_X1, ROI_Y1);
                    if (s >= 0.0f) {
                        s_baseline_sat = s;
                        fprintf(stderr,
                            "[vision] baseline sat = %.3f; start search "
                            "[0x%06x, 0x%06x]\n",
                            s, g_search_lo, g_search_hi);
                        fflush(stderr);
                        g_search_active = 1;   /* arm the per-frame poker */
                        s_phase = V_POKE_HOLD;
                        s_frame_in_phase = 0;
                    } else if (s_frame_in_phase > MEASURE_TIMEOUT) {
                        fprintf(stderr,
                            "[vision] baseline PNG never landed; retrying\n");
                        fflush(stderr);
                        m64p_ss_vision_snap();
                        s_frame_in_phase = 0;
                    }
                }
            }
            else if (s_phase == V_POKE_HOLD) {
                /* credit-search style poking ran earlier in this function
                 * via g_search_active; nothing to do here but wait. */
                if (s_frame_in_phase >= POKE_HOLD_FRAMES) {
                    m64p_ss_vision_snap();
                    s_phase = V_MEASURE;
                    s_frame_in_phase = 0;
                }
            }
            else if (s_phase == V_MEASURE) {
                float s = m64p_ss_vision_measure_roi(
                    &s_last_shot_ft, ROI_X0, ROI_Y0, ROI_X1, ROI_Y1);
                if (s >= 0.0f) {
                    float delta = s - s_baseline_sat;
                    int lit = (delta > LIT_DELTA);
                    s_iter++;
                    fprintf(stderr,
                        "[vision] iter=%d [0x%06x,0x%06x] sat=%.3f "
                        "delta=%.3f -> %s\n",
                        s_iter, g_search_lo, g_search_hi, s, delta,
                        lit ? "LIT" : "dark");
                    fflush(stderr);

                    /* Always restore the bytes we poked. */
                    if (g_search_has_backup) {
                        uint32_t len = g_search_backup_hi
                                     - g_search_backup_lo + 1;
                        memcpy(s_rdram + g_search_backup_lo,
                               g_search_backup, len);
                    }

                    if (g_search_hi - g_search_lo + 1 <= STOP_WIDTH) {
                        fprintf(stderr,
                            "[vision] *** DONE *** final range "
                            "[0x%06x, 0x%06x] width=%u bytes, "
                            "lit=%d on final probe\n",
                            g_search_lo, g_search_hi,
                            (unsigned)(g_search_hi - g_search_lo + 1),
                            lit);
                        fflush(stderr);
                        g_search_active = 0;
                        s_phase = V_DONE;
                    } else {
                        uint32_t mid = (g_search_lo + g_search_hi) / 2;
                        if (lit) {
                            g_search_hi = mid;
                        } else {
                            g_search_lo = mid + 1;
                        }
                        g_search_backup_lo = g_search_lo;
                        g_search_backup_hi = g_search_hi;
                        uint32_t len = g_search_hi - g_search_lo + 1;
                        memcpy(g_search_backup,
                               s_rdram + g_search_lo, len);
                        g_search_has_backup = 1;
                        g_search_active = 1;
                        s_phase = V_POKE_HOLD;
                        s_frame_in_phase = 0;
                    }
                } else if (s_frame_in_phase > MEASURE_TIMEOUT) {
                    fprintf(stderr,
                        "[vision] post-poke PNG never landed; re-snap\n");
                    fflush(stderr);
                    m64p_ss_vision_snap();
                    s_frame_in_phase = 0;
                }
            }
            /* V_DONE: nothing to do, just idle. */
        }
    }
#endif  /* _WIN32 */

    /* DMA intercept REMOVED. PI DOM2 0x08000000-0x08007FFF is SRAM (the
     * kiosk ROM saves game state there). Overwriting it with smart_card.c
     * data corrupts saves. And CPU-direct osEPiReadIo/osEPiWriteIo to the
     * smart-card registers don't touch PI DMA regs, so we can't observe
     * them at this layer anyway. Correct PI DOM2 interception for the
     * smart-card reader requires core-side patches (out of scope of the
     * RMG binary we're shipping). */

    s_last_cart = cart;
    s_last_dram = dram;
    s_last_status = status;
}

/* --- Chained SDL input plugin (ports 1-3) ---------------------------- */

typedef m64p_error (CALL *ptr_PluginStartup)(m64p_dynlib_handle, void*, void(*)(void*,int,const char*));
typedef m64p_error (CALL *ptr_PluginShutdown)(void);
typedef void       (CALL *ptr_InitiateControllers)(CONTROL_INFO);
typedef void       (CALL *ptr_GetKeys)(int, BUTTONS*);
typedef void       (CALL *ptr_Controller)(int, unsigned char*);
typedef void       (CALL *ptr_Rom)(void);
typedef void       (CALL *ptr_SDL_Key)(int, int);

static struct {
#ifdef _WIN32
    HMODULE dll;
#endif
    ptr_PluginStartup       startup;
    ptr_PluginShutdown      shutdown;
    ptr_InitiateControllers init;
    ptr_GetKeys             getkeys;
    ptr_Controller          cmd;
    ptr_Controller          read;
    ptr_Rom                 romopen;
    ptr_Rom                 romclosed;
    ptr_SDL_Key             keydown;
    ptr_SDL_Key             keyup;
    int                     loaded;
} g_sdl;

static int chain_sdl_load(m64p_dynlib_handle core, void *ctx,
                          void (*dbg)(void*,int,const char*))
{
#ifdef _WIN32
    g_sdl.dll = LoadLibraryA("mupen64plus-input-sdl.dll");
    if (!g_sdl.dll) {
        fprintf(stderr, "[snapstation-input] could not load mupen64plus-input-sdl.dll "
                        "(err=%lu); ports 1-3 will be dead\n", GetLastError());
        return 0;
    }
    g_sdl.startup   = (ptr_PluginStartup)      GetProcAddress(g_sdl.dll, "PluginStartup");
    g_sdl.shutdown  = (ptr_PluginShutdown)     GetProcAddress(g_sdl.dll, "PluginShutdown");
    g_sdl.init      = (ptr_InitiateControllers)GetProcAddress(g_sdl.dll, "InitiateControllers");
    g_sdl.getkeys   = (ptr_GetKeys)            GetProcAddress(g_sdl.dll, "GetKeys");
    g_sdl.cmd       = (ptr_Controller)         GetProcAddress(g_sdl.dll, "ControllerCommand");
    g_sdl.read      = (ptr_Controller)         GetProcAddress(g_sdl.dll, "ReadController");
    g_sdl.romopen   = (ptr_Rom)                GetProcAddress(g_sdl.dll, "RomOpen");
    g_sdl.romclosed = (ptr_Rom)                GetProcAddress(g_sdl.dll, "RomClosed");
    g_sdl.keydown   = (ptr_SDL_Key)            GetProcAddress(g_sdl.dll, "SDL_KeyDown");
    g_sdl.keyup     = (ptr_SDL_Key)            GetProcAddress(g_sdl.dll, "SDL_KeyUp");
    if (!g_sdl.startup || !g_sdl.init || !g_sdl.getkeys) {
        fprintf(stderr, "[snapstation-input] SDL plugin missing required exports\n");
        FreeLibrary(g_sdl.dll);
        g_sdl.dll = NULL;
        return 0;
    }
    if (g_sdl.startup(core, ctx, dbg) != 0) {
        fprintf(stderr, "[snapstation-input] SDL plugin startup failed\n");
        FreeLibrary(g_sdl.dll);
        g_sdl.dll = NULL;
        return 0;
    }
    fprintf(stderr, "[snapstation-input] chained mupen64plus-input-sdl.dll for ports 1-3\n");
    g_sdl.loaded = 1;
    return 1;
#else
    (void)core; (void)ctx; (void)dbg;
    return 0;
#endif
}

static void chain_sdl_unload(void)
{
#ifdef _WIN32
    if (g_sdl.shutdown) g_sdl.shutdown();
    if (g_sdl.dll) FreeLibrary(g_sdl.dll);
    memset(&g_sdl, 0, sizeof(g_sdl));
#endif
}

/* --- Internal state -------------------------------------------------- */
/* (g_station declared earlier, alongside g_sc_reader/g_sc_card, so the
 *  per-VI on_core_frame hook can reference it.) */

static void on_event(const char *msg, void *user) {
    (void)user;
    fprintf(stderr, "[snapstation-input] %s\n", msg);
}

static void on_sc_event(const char *msg, void *user) {
    (void)user;
    fprintf(stderr, "[snapstation-input] sc: %s\n", msg);
}

/* Peek/poke helpers for locating NPHE's in-RAM credit cache. The PI DMA
 * watch log shows NPHE mirrors its smart-card reads into RDRAM at
 * DRAM = 0x000c2160 + 2*(CART - 0x08000000), so card-memory offset 0x10
 * (SC_BALANCE_OFFSET) should land at RDRAM 0x000c2180. These helpers let
 * us verify that empirically via button-triggered dumps and pokes. */
#ifdef _WIN32
static void peek_rdram(uint32_t addr, size_t len)
{
    if (!g_dbg.GetPointer) return;
    uint8_t *rdram = (uint8_t *)g_dbg.GetPointer(M64P_DBG_PTR_RDRAM);
    if (!rdram || addr + len > 0x800000) return;
    char line[256]; int n = 0;
    n += snprintf(line + n, sizeof(line) - n, "[peek 0x%08x] ", addr);
    for (size_t i = 0; i < len && n < (int)sizeof(line) - 8; i++) {
        n += snprintf(line + n, sizeof(line) - n, "%02x%s",
                      rdram[addr + i], (i % 4 == 3) ? " " : "");
    }
    n += snprintf(line + n, sizeof(line) - n, " |");
    for (size_t i = 0; i < len && n < (int)sizeof(line) - 2; i++) {
        uint8_t c = rdram[addr + i];
        line[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    if (n < (int)sizeof(line) - 2) { line[n++] = '|'; line[n] = 0; }
    fprintf(stderr, "[snapstation-input] %s\n", line);
    fflush(stderr);
}

static void poke_rdram_u32_be(uint32_t addr, uint32_t value)
{
    if (!g_dbg.GetPointer) return;
    uint8_t *rdram = (uint8_t *)g_dbg.GetPointer(M64P_DBG_PTR_RDRAM);
    if (!rdram || addr + 4 > 0x800000) return;
    rdram[addr + 0] = (value >> 24) & 0xFF;
    rdram[addr + 1] = (value >> 16) & 0xFF;
    rdram[addr + 2] = (value >>  8) & 0xFF;
    rdram[addr + 3] = (value      ) & 0xFF;
    fprintf(stderr, "[snapstation-input] poke 0x%08x = 0x%08x\n", addr, value);
    fflush(stderr);
}
#else
static void peek_rdram(uint32_t addr, size_t len) { (void)addr; (void)len; }
static void poke_rdram_u32_be(uint32_t a, uint32_t v) { (void)a; (void)v; }
#endif

/* On-demand RDRAM snapshot for RAM diffing. Writes the full 8 MB of
 * emulated RDRAM to a named file so we can diff two snapshots to find
 * game state (e.g. NPHE's credit counter). Uses DebugMemGetPointer,
 * which is exported even when the core isn't built with --enable-debugger.
 *
 * Path: %TEMP%\snap_station_emu\ram_<label>_<index>.bin */
static void take_ram_snapshot(const char *label, unsigned idx)
{
#ifdef _WIN32
    if (!g_dbg.GetPointer) return;
    uint8_t *rdram = (uint8_t *)g_dbg.GetPointer(M64P_DBG_PTR_RDRAM);
    if (!rdram) {
        fprintf(stderr, "[snapstation-input] ram snapshot: RDRAM ptr null\n");
        fflush(stderr);
        return;
    }
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = ".";
    char dir[256];
    snprintf(dir, sizeof(dir), "%s\\snap_station_emu", tmp);
    CreateDirectoryA(dir, NULL);
    char path[256];
    snprintf(path, sizeof(path), "%s\\ram_%s_%u.bin", dir, label, idx);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "[snapstation-input] ram snapshot: open %s failed\n", path);
        fflush(stderr);
        return;
    }
    fwrite(rdram, 1, 0x800000, fp);
    fclose(fp);
    fprintf(stderr, "[snapstation-input] ram snapshot '%s' #%u -> %s\n",
            label, idx, path);
    fflush(stderr);
#else
    (void)label; (void)idx;
#endif
}

/* Minus-button handler: add SS_MINUS_CREDIT_GRANT credits to the card in
 * the reader, creating one if none is inserted. Prints the new balance.
 *
 * NOTE: credits become visible to the running ROM only after the PI DOM2
 * cart-bus hook is wired - see README "Smart-card emulation" section.
 * Until then this keeps the smart_card backend warm for testing. */
static void ss_insert_credits(void)
{
    if (!g_sc_reader) return;
    if (!g_sc_card) {
        g_sc_card = sc_card_create(SS_MINUS_CREDIT_GRANT);
        sc_reader_insert(g_sc_reader, g_sc_card);
        fprintf(stderr, "[snapstation-input] sc: MINUS pressed - inserted card with %d credits\n",
                SS_MINUS_CREDIT_GRANT);
    } else {
        uint32_t bal = sc_card_balance(g_sc_card);
        sc_card_set_balance(g_sc_card, bal + SS_MINUS_CREDIT_GRANT);
        fprintf(stderr, "[snapstation-input] sc: MINUS pressed - balance %u -> %u\n",
                (unsigned)bal, (unsigned)(bal + SS_MINUS_CREDIT_GRANT));
    }
    fflush(stderr);
}

/* Backend callbacks. These forward to the Win32 bridge at runtime; we
 * declare the function pointer slot here so the plugin links cleanly
 * even when the Win32 bridge isn't present (Linux builds). */
extern void m64p_ss_take_screenshot(int photo_idx);
extern int  m64p_ss_compose_and_print(void);
extern void m64p_ss_request_reset(void);
extern void m64p_ss_begin_job(void);
#ifdef _WIN32
/* windows.h already included at the top of this file; FILETIME is
 * declared there. The vision helpers live in snap_station_win32.c. */
extern int   m64p_ss_vision_snap(void);
extern float m64p_ss_vision_measure_roi(FILETIME *baseline_io,
                                        float x0, float y0,
                                        float x1, float y1);
#endif

static void on_capture_photo(int idx, void *user) {
    (void)user;
    /* Record the baseline of existing screenshots at the start of a
     * session so compose can later tell this job's captures apart from
     * any leftover PNGs in the screenshot directory. We mark the start
     * on the first capture (idx == 0) rather than on DISPLAY_BEGIN
     * because the screenshot directory may only be resolvable once we
     * know the core is live. */
    if (idx == 0) m64p_ss_begin_job();
    m64p_ss_take_screenshot(idx);
}
static void on_print_ready(void *user) {
    (void)user;
    /* HLE credit deduction: only burn a credit once compose_and_print
     * confirms a print job was actually accepted. If the user cancels
     * the preview dialog, cancels PrintDlg, or StartDoc/EndDoc fails,
     * compose_and_print returns non-zero and the balance stays intact -
     * the ROM will see the same credit count on its next 0xD000 read. */
    int rc = m64p_ss_compose_and_print();
    if (rc != 0) {
        fprintf(stderr,
            "[snapstation-input] sc: print NOT accepted (rc=%d) - credit preserved\n",
            rc);
        fflush(stderr);
        return;
    }
    if (g_sc_card) {
        uint32_t bal = sc_card_balance(g_sc_card);
        if (bal > 0) {
            sc_card_set_balance(g_sc_card, bal - 1);
            fprintf(stderr,
                "[snapstation-input] sc: print accepted - credit %u -> %u\n",
                (unsigned)bal, (unsigned)(bal - 1));
            fflush(stderr);
        }
    }
}
static void on_request_reset(void *user) {
    (void)user;
    m64p_ss_request_reset();
}

/* HLE: backend hook that reports the current credit balance for the
 * 0xD000 JoyBus channel. Returns 0 if no card is inserted. */
static uint32_t query_credit_balance(void *user) {
    (void)user;
    return g_sc_card ? sc_card_balance(g_sc_card) : 0;
}

/* --- Plugin ABI -------------------------------------------------------- */

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core,
                                     void *context,
                                     void (*debug_callback)(void*, int, const char*))
{
    jbs_backend_t b;
    memset(&b, 0, sizeof(b));
    b.on_event             = on_event;
    b.on_capture_photo     = on_capture_photo;
    b.on_print_ready       = on_print_ready;
    b.on_request_reset     = on_request_reset;
    b.query_credit_balance = query_credit_balance;

    g_station = jbs_create(&b);
    if (!g_station) return -1;

    /* HLE: disable the post-0x02 busy stall. On real hardware the
     * station returns 0x08 for ~0.5 s while the printer captures;
     * in the emulator the "capture" (a CoreDoCommand screenshot) is
     * synchronous, so stalling the ROM just wastes frames and risks
     * confusing libdragon's retry logic if jbs_tick doesn't drain the
     * counter fast enough. The ROM's own wait_ms between photos is
     * what sequences the screenshot timing. */
    jbs_set_capture_stall(g_station, 0);

    sc_reader_backend_t scb = { .on_event = on_sc_event, .user = NULL };
    g_sc_reader = sc_reader_create(&scb);
    if (!g_sc_reader) {
        fprintf(stderr, "[snapstation-input] sc: reader init failed\n");
    }

    /* HLE: pre-insert a card with SS_MINUS_CREDIT_GRANT credits so demo
     * ROMs can run the full print flow without a user first pressing
     * Minus. Interactive Minus presses still work via ss_insert_credits;
     * they add more credits to this same card. */
    if (g_sc_reader && !g_sc_card) {
        g_sc_card = sc_card_create(SS_MINUS_CREDIT_GRANT);
        if (g_sc_card) {
            sc_reader_insert(g_sc_reader, g_sc_card);
            fprintf(stderr,
                "[snapstation-input] sc: auto-inserted card with %d credits\n",
                SS_MINUS_CREDIT_GRANT);
        }
    }

    /* Resolve core APIs now so they're ready for later use. The actual
     * frame-callback registration must wait for RomOpen (after the ROM
     * loads; SET_FRAME_CALLBACK returns INPUT_INVALID before that). */
    resolve_dbg(core);

    chain_sdl_load(core, context, debug_callback);
    return 0;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    chain_sdl_unload();
    if (g_sc_reader) { sc_reader_destroy(g_sc_reader); g_sc_reader = NULL; }
    if (g_sc_card)   { sc_card_destroy(g_sc_card);     g_sc_card   = NULL; }
    jbs_destroy(g_station);
    g_station = NULL;
    return 0;
}

EXPORT m64p_error CALL PluginGetVersion(uint16_t *plugin_type, uint16_t *plugin_version,
                                        uint16_t *api_version, const char **plugin_name,
                                        int *capabilities)
{
    if (plugin_type)    *plugin_type    = M64PLUGIN_INPUT;
    if (plugin_version) *plugin_version = 0x010000;
    if (api_version)    *api_version    = M64P_PLUGIN_API_VERSION;
    if (plugin_name)    *plugin_name    = "Snap Station";
    if (capabilities)   *capabilities   = 0;
    return 0;
}

/* InitiateControllers: declare which ports we own. Port 4 (index 3) is
 * marked Present with RawData mode (so the core routes JoyBus ops to us)
 * and Plugin=1 (mempak) to match the Snap Station's pak-present
 * advertisement. */
EXPORT void CALL InitiateControllers(CONTROL_INFO control_info)
{
    /* Let SDL claim ports 1-3 first (it writes Present/Plugin based on its
     * config + attached gamepads). Then we stamp port 4 as the Snap Station. */
    if (g_sdl.loaded && g_sdl.init) g_sdl.init(control_info);

    if (control_info.Controls) {
        /* Diagnostic: log state right after SDL so we can see what it did. */
        for (int i = 0; i < 4; i++) {
            fprintf(stderr,
                "[snapstation-input] ic(post-sdl): ctrl[%d] Present=%d "
                "RawData=%d Plugin=%d\n", i,
                control_info.Controls[i].Present,
                control_info.Controls[i].RawData,
                control_info.Controls[i].Plugin);
        }
        control_info.Controls[3].Present = 1;
        control_info.Controls[3].RawData = 1;
        control_info.Controls[3].Plugin  = 1;   /* mempak (we spoof as pak) */
        fprintf(stderr,
            "[snapstation-input] ic(final): ctrl[3] Present=%d RawData=%d "
            "Plugin=%d\n",
            control_info.Controls[3].Present,
            control_info.Controls[3].RawData,
            control_info.Controls[3].Plugin);
        fflush(stderr);
    }
}

/* Direct SDL joystick probes — resolved lazily once SDL2 is loaded by the
 * input plugin. Lets us compare what the SDL plugin writes to BUTTONS
 * against what SDL2 itself sees for the same axes. */
typedef int16_t (*ptr_SDL_JoystickGetAxis)(void*, int);
typedef void*   (*ptr_SDL_JoystickOpen)(int);
typedef int     (*ptr_SDL_NumJoysticks)(void);
typedef int     (*ptr_SDL_JoystickNumAxes)(void*);
typedef void    (*ptr_SDL_JoystickUpdate)(void);
static ptr_SDL_JoystickGetAxis  q_GetAxis;
static ptr_SDL_JoystickOpen     q_Open;
static ptr_SDL_NumJoysticks     q_Num;
static ptr_SDL_JoystickNumAxes  q_NumAxes;
static ptr_SDL_JoystickUpdate   q_Update;
static void                    *q_js;
static int                      q_probed;

static void probe_sdl_axes(int8_t out[6])
{
#ifdef _WIN32
    if (!q_probed) {
        q_probed = 1;
        HMODULE h = GetModuleHandleA("SDL2.dll");
        if (!h) return;
        q_GetAxis = (ptr_SDL_JoystickGetAxis)GetProcAddress(h, "SDL_JoystickGetAxis");
        q_Open    = (ptr_SDL_JoystickOpen)   GetProcAddress(h, "SDL_JoystickOpen");
        q_Num     = (ptr_SDL_NumJoysticks)   GetProcAddress(h, "SDL_NumJoysticks");
        q_NumAxes = (ptr_SDL_JoystickNumAxes)GetProcAddress(h, "SDL_JoystickNumAxes");
        q_Update  = (ptr_SDL_JoystickUpdate) GetProcAddress(h, "SDL_JoystickUpdate");
        if (q_Num && q_Num() > 0 && q_Open) q_js = q_Open(0);
    }
    if (!q_js || !q_GetAxis) return;
    if (q_Update) q_Update();
    for (int i = 0; i < 6; i++) {
        int16_t raw = q_GetAxis(q_js, i);
        out[i] = (int8_t)(raw / 256);   /* compressed for log readability */
    }
#else
    (void)out;
#endif
}

EXPORT void CALL GetKeys(int Control, BUTTONS *Keys)
{
    if (Control != 3 && g_sdl.loaded && g_sdl.getkeys) {
        g_sdl.getkeys(Control, Keys);
        /* Poll PI regs piggybacked on port-0's per-frame GetKeys call.
         * RMG's core doesn't expose SET_FRAME_CALLBACK or DebugSetCallbacks,
         * but GetKeys fires every frame and DebugMemRead32 is exported
         * and functional. */
        if (Control == 0) on_core_frame(0);
        if (Control == 0 && Keys) {
            /* Edge-detect Minus (SDL button 4) by direct SDL poll. The
             * mupen64plus-input-sdl plugin doesn't expose Minus in its
             * N64 BUTTONS struct, so we bypass it here. */
#ifdef _WIN32
            /* Make sure q_js/q_GetAxis are populated BEFORE we use them.
             * probe_sdl_axes is what lazily opens the joystick handle. */
            { int8_t _scratch[6] = {0}; probe_sdl_axes(_scratch); }

            static int      s_minus_prev;
            static uint32_t s_btn_state;
            if (q_js) {
                HMODULE h = GetModuleHandleA("SDL2.dll");
                if (h) {
                    typedef uint8_t (*ptr_SDL_JoystickGetButton)(void*, int);
                    typedef int     (*ptr_SDL_JoystickNumButtons)(void*);
                    static ptr_SDL_JoystickGetButton  qb;
                    static ptr_SDL_JoystickNumButtons qn;
                    if (!qb) qb = (ptr_SDL_JoystickGetButton)
                        GetProcAddress(h, "SDL_JoystickGetButton");
                    if (!qn) qn = (ptr_SDL_JoystickNumButtons)
                        GetProcAddress(h, "SDL_JoystickNumButtons");

                    if (qb) {
                        /* Log every change in SDL's raw button state so we
                         * can see if the driver is reporting anything. */
                        int nb = qn ? qn(q_js) : 20;
                        if (nb > 32) nb = 32;
                        uint32_t cur = 0;
                        for (int i = 0; i < nb; i++)
                            if (qb(q_js, i)) cur |= (1u << i);
                        if (cur != s_btn_state) {
                            fprintf(stderr, "[snapstation-input] SDL btns: 0x%08x -> 0x%08x\n",
                                    s_btn_state, cur);
                            fflush(stderr);
                            s_btn_state = cur;
                        }
                        /* Button map (jstest ground truth):
                         *   4  = Minus  → insert credits, snapshot "after"
                         *   5  = Home   → poke candidate credit addresses
                         *                 with value 9 (BE u32)
                         *   6  = Plus   → snapshot "before"
                         *  15  = Capture → peek candidate credit region
                         *                 (hex + ASCII, 64 bytes)
                         * Edge-triggered so a held button fires once. */
                        static int       s_plus_prev;
                        static int       s_home_prev;
                        static int       s_capture_prev;
                        static unsigned  s_snap_idx;
                        int minus   = qb(q_js, 4);
                        int home    = qb(q_js, 5);
                        int plus    = qb(q_js, 6);
                        int capture = qb(q_js, 15);
                        /* When credit-search is active, Plus and Minus
                         * drive the binary-search instead of doing
                         * snapshots / card insert. Narrow to lower half
                         * on Plus, upper half on Minus. */
                        int in_search_mode = 0;
                        {
                            const char *e = getenv("SNAPSTATION_CREDIT_SEARCH");
                            in_search_mode = (e && e[0]);
                        }
                        if (plus && !s_plus_prev) {
                            if (in_search_mode && g_search_active
                                && g_dbg.GetPointer) {
                                uint8_t *rdram = (uint8_t *)g_dbg.GetPointer(
                                    M64P_DBG_PTR_RDRAM);
                                if (rdram && g_search_has_backup) {
                                    uint32_t len = g_search_backup_hi
                                                 - g_search_backup_lo + 1;
                                    memcpy(rdram + g_search_backup_lo,
                                           g_search_backup, len);
                                }
                                uint32_t mid =
                                    (g_search_lo + g_search_hi) / 2;
                                g_search_hi = mid;
                                if (rdram) {
                                    g_search_backup_lo = g_search_lo;
                                    g_search_backup_hi = g_search_hi;
                                    uint32_t len =
                                        g_search_hi - g_search_lo + 1;
                                    memcpy(g_search_backup,
                                           rdram + g_search_lo, len);
                                    g_search_has_backup = 1;
                                }
                                fprintf(stderr,
                                    "[search] NARROW LOWER -> "
                                    "[0x%06x, 0x%06x]\n",
                                    g_search_lo, g_search_hi);
                                fflush(stderr);
                            } else {
                                take_ram_snapshot("before", s_snap_idx);
                            }
                        }
                        if (minus && !s_minus_prev) {
                            if (in_search_mode && g_search_active
                                && g_dbg.GetPointer) {
                                uint8_t *rdram = (uint8_t *)g_dbg.GetPointer(
                                    M64P_DBG_PTR_RDRAM);
                                if (rdram && g_search_has_backup) {
                                    uint32_t len = g_search_backup_hi
                                                 - g_search_backup_lo + 1;
                                    memcpy(rdram + g_search_backup_lo,
                                           g_search_backup, len);
                                }
                                uint32_t mid =
                                    (g_search_lo + g_search_hi) / 2;
                                g_search_lo = mid + 1;
                                if (rdram) {
                                    g_search_backup_lo = g_search_lo;
                                    g_search_backup_hi = g_search_hi;
                                    uint32_t len =
                                        g_search_hi - g_search_lo + 1;
                                    memcpy(g_search_backup,
                                           rdram + g_search_lo, len);
                                    g_search_has_backup = 1;
                                }
                                fprintf(stderr,
                                    "[search] NARROW UPPER -> "
                                    "[0x%06x, 0x%06x]\n",
                                    g_search_lo, g_search_hi);
                                fflush(stderr);
                            } else {
                                ss_insert_credits();
                                take_ram_snapshot("after", s_snap_idx);
                                s_snap_idx++;
                            }
                        }
                        if (capture && !s_capture_prev) {
                            /* In vision mode, Capture is the "start /
                             * recalibrate from this frame" trigger.
                             * In plain credit-search mode, keep the
                             * old peek-rdram behaviour. */
                            const char *ve = getenv(
                                "SNAPSTATION_VISION_SEARCH");
                            if (ve && ve[0]) {
                                g_vision_reset = 1;
                                fprintf(stderr,
                                    "[vision] CAPTURE pressed - reset\n");
                                fflush(stderr);
                            } else {
                                peek_rdram(g_search_lo, 32);
                                peek_rdram(g_search_hi - 31, 32);
                            }
                        }
                        if (home && !s_home_prev) {
                            /* Reset binary search to the full 8MB. */
                            if (g_dbg.GetPointer) {
                                uint8_t *rdram = (uint8_t *)g_dbg.GetPointer(
                                    M64P_DBG_PTR_RDRAM);
                                if (rdram && g_search_has_backup) {
                                    uint32_t len = g_search_backup_hi
                                                 - g_search_backup_lo + 1;
                                    memcpy(rdram + g_search_backup_lo,
                                           g_search_backup, len);
                                }
                                if (rdram) {
                                    if (!g_search_backup)
                                        g_search_backup = malloc(0x800000);
                                    g_search_lo = 0x00000000;
                                    g_search_hi = 0x007FFFFF;
                                    g_search_backup_lo = g_search_lo;
                                    g_search_backup_hi = g_search_hi;
                                    memcpy(g_search_backup,
                                           rdram + g_search_lo, 0x800000);
                                    g_search_has_backup = 1;
                                    g_search_active = 1;
                                    fprintf(stderr,
                                        "[search] RESET [0x%06x, 0x%06x]\n",
                                        g_search_lo, g_search_hi);
                                    fflush(stderr);
                                }
                            }
                        }
                        s_plus_prev    = plus;
                        s_minus_prev   = minus;
                        s_home_prev    = home;
                        s_capture_prev = capture;
                    }
                }
            }
#endif
            /* Autonomous driver: press A every ~3 s for a few frames
             * so we can walk NPHE through any "press A to continue"
             * screens without needing a human. Also triggers a
             * periodic screenshot so we can read back and see what
             * NPHE is rendering. Controlled by env var
             * SNAPSTATION_AUTO_DRIVE (any non-empty value enables). */
            static int       s_auto_drive = -1;
            static unsigned  s_auto_tick;
            if (s_auto_drive < 0) {
                const char *e = getenv("SNAPSTATION_AUTO_DRIVE");
                s_auto_drive = (e && e[0]) ? 1 : 0;
                fprintf(stderr, "[snapstation-input] auto-drive=%d\n",
                        s_auto_drive);
            }
            if (s_auto_drive) {
                s_auto_tick++;
                /* Pattern, repeats every 900 frames (~15 s):
                 *   0-2:     A-button (advance prompts, select)
                 *   180-182: A-button
                 *   360-362: A-button
                 *   540-543: B-button (cancel/back)
                 *   720-723: Start (pause during a course -> menu)
                 *   840-843: A-button (confirm "give up" on pause menu)
                 * This cycles through press A, cancel back, exit to
                 * lab, so even in a course we eventually surface to the
                 * Oak-lab / album screens where the print button lives. */
                unsigned p = s_auto_tick % 900;
                if (p < 3 || (p >= 180 && p < 183) || (p >= 360 && p < 363)
                    || (p >= 840 && p < 843)) {
                    Keys->Value |= 0x80;  /* A_BUTTON */
                }
                if (p >= 540 && p < 543) {
                    Keys->Value |= 0x40;  /* B_BUTTON */
                }
                if (p >= 720 && p < 723) {
                    Keys->Value |= 0x10;  /* START_BUTTON (bit 4) */
                }
                /* Screenshot every 180 frames (~3 s). */
                if ((s_auto_tick % 180) == 90 && g_dbg.DoCommand) {
                    g_dbg.DoCommand(16, 0, NULL);  /* TAKE_NEXT_SCREENSHOT */
                    fprintf(stderr,
                        "[snapstation-input] auto-drive: shot @ tick=%u\n",
                        s_auto_tick);
                }
            }

            static unsigned long s_ticks;
            static uint32_t      s_last_value;
            static int8_t        s_last_raw[6];
            s_ticks++;
            uint32_t v = Keys->Value;
            int8_t raw[6] = {0};
            probe_sdl_axes(raw);
            int raw_change = 0;
            for (int i = 0; i < 6; i++) if (raw[i] != s_last_raw[i]) raw_change = 1;
            if (v != s_last_value || raw_change || (s_ticks % 180) == 0) {
                int8_t x = (int8_t)((v >> 16) & 0xFF);   /* bits 16-23 = X_AXIS per m64p_plugin.h */
                int8_t y = (int8_t)((v >> 24) & 0xFF);   /* bits 24-31 = Y_AXIS */
                fprintf(stderr, "[snapstation-input] port1 btn=0x%04x X=%+4d Y=%+4d  "
                                "SDL raw: a0=%+4d a1=%+4d a2=%+4d a3=%+4d a4=%+4d a5=%+4d\n",
                        v & 0xFFFF, (int)x, (int)y,
                        raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
                fflush(stderr);
                s_last_value = v;
                for (int i = 0; i < 6; i++) s_last_raw[i] = raw[i];
            }
        }
        return;
    }
    if (Keys) Keys->Value = 0;
}

/* ControllerCommand gets called with the raw JoyBus command payload for
 * each port. We inspect the opcode byte and route reads/writes into our
 * state machine. */
EXPORT void CALL ControllerCommand(int Control, unsigned char *Command)
{
    /* Diagnostic: log every JoyBus transaction. Rate-limit port != 3 to
     * avoid flooding the log with SDL-bound ident polling. Port 3
     * (our Snap Station) always logs. */
    if (Command) {
        static unsigned s_total;
        s_total++;
        int log_it = (Control == 3) || (s_total <= 40) || (s_total % 200 == 0);
        if (log_it) {
            fprintf(stderr,
                "[snapstation-input] cc: port=%d cmd=0x%02x tx=%u rx=%u "
                "[3..6]=%02x %02x %02x %02x\n",
                Control, Command[2], Command[0], Command[1],
                Command[3], Command[4], Command[5], Command[6]);
            fflush(stderr);
        }
    }

    if (Control != 3) {
        if (g_sdl.loaded && g_sdl.cmd) g_sdl.cmd(Control, Command);
        return;
    }
    if (!Command || !g_station) return;

    /* Command byte layout (per Mupen64Plus m64p_plugin_protocol.h):
     *   [0]  txBytes
     *   [1]  rxBytes
     *   [2]  command
     *   [3+] command-specific data (tx payload then rx buffer)
     */
    uint8_t  cmd     = Command[2];

    if (cmd == 0x00 || cmd == 0xFF) {
        /* Ident: txBytes=1, rxBytes=3, rx payload starts at [4]. */
        uint8_t id[3];
        jbs_joybus_ident(g_station, id);
        Command[4] = id[0];
        Command[5] = id[1];
        Command[6] = id[2];
        return;
    }

    if (cmd == 0x01) {
        /* Read controller buttons + stick (txBytes=1, rxBytes=4).
         * NPHE polls this on all ports during boot; if port 4 never
         * answers, the attract-screen loop stalls waiting for input.
         * We hard-report "no buttons pressed, stick centered" so the
         * real Snap Station silently participates without injecting
         * phantom input. Response layout (per m64p_plugin_protocol):
         *   byte[0..1] = button bitfield (all 0 = idle)
         *   byte[2]    = X axis (signed -128..127, 0 = center)
         *   byte[3]    = Y axis */
        Command[3] = 0x00;
        Command[4] = 0x00;
        Command[5] = 0x00;
        Command[6] = 0x00;
        return;
    }

    if (cmd == 0x02) {
        /* Pak read: txBytes=3 (cmd + 16-bit addr), rxBytes=33.
         * Address at [3..4], rx buffer at [5..37]. Byte 37 is the
         * CRC-8 over the 32 data bytes; libdragon verifies it and
         * returns BAD_CRC on mismatch, which triggers a retry loop. */
        uint16_t addr = ((uint16_t)Command[3] << 8) | Command[4];
        uint8_t  out[32];
        jbs_joybus_pak_read(g_station, addr, out);
        memcpy(&Command[5], out, 32);
        Command[5 + 32] = jbs_pak_crc(out);
        return;
    }

    if (cmd == 0x03) {
        /* Pak write: txBytes=35, rxBytes=1. Address at [3..4], data
         * at [5..36], response CRC at [37]. libdragon expects the
         * CRC-8 of the 32 data bytes it just sent (the backend's
         * rolling_response would fail libdragon's CRC check). */
        uint16_t addr = ((uint16_t)Command[3] << 8) | Command[4];
        (void)jbs_joybus_pak_write(g_station, addr, &Command[5]);
        Command[3 + 32 + 2] = jbs_pak_crc(&Command[5]);
        return;
    }
}

EXPORT void CALL ReadController(int Control, unsigned char *Command) {
    if (Control != 3) {
        if (g_sdl.loaded && g_sdl.read) g_sdl.read(Control, Command);
        return;
    }
    /* Port 3 (Snap Station): do NOT re-dispatch to ControllerCommand.
     * Mupen64Plus invokes both entry points for RawData controllers, and
     * forwarding here causes every JoyBus op to be processed twice - which
     * made photo_idx in the state machine double-increment, so only half
     * the 16 screenshots (8) made it to disk before photo_idx hit its cap. */
}

EXPORT void CALL ControllerReset(int Control) {
    if (Control == 3 && g_station) jbs_console_reset(g_station);
}

EXPORT void CALL RomOpen(void) {
    if (g_sdl.loaded && g_sdl.romopen) g_sdl.romopen();
}
EXPORT void CALL RomClosed(void) {
    if (g_sdl.loaded && g_sdl.romclosed) g_sdl.romclosed();
}
EXPORT int  CALL RomClose(void)   { return 0; }
EXPORT void CALL SDL_KeyDown(int keymod, int keysym) {
    if (g_sdl.loaded && g_sdl.keydown) g_sdl.keydown(keymod, keysym);
}
EXPORT void CALL SDL_KeyUp(int keymod, int keysym) {
    if (g_sdl.loaded && g_sdl.keyup) g_sdl.keyup(keymod, keysym);
}

/* --------------------------------------------------------------------- */
/* Weak fallback stubs for the Win32 bridge hooks. The real implementations
 * live in snap_station_win32.c (Windows) or can be overridden by tests.  */

#ifndef _WIN32
__attribute__((weak)) void m64p_ss_take_screenshot(int idx) { (void)idx; }
__attribute__((weak)) void m64p_ss_compose_and_print(void)  {}
__attribute__((weak)) void m64p_ss_request_reset(void)      {}
#endif
