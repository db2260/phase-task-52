/*
 * mode_switch_handler.c — Phase Pogo Pin Mode Switch (Simulation Harness)
 *
 * This is a POSIX simulation of the real Phase firmware module at the same
 * path: src/device_mode_switch/mode_switch_handler.c
 *
 * Build and run:
 *   make        — builds the binary
 *   make run    — builds and runs
 *
 * Or directly:
 *   gcc -Wall -std=c11 -O1 -o debounce mode_switch_handler.c -lpthread
 *   ./debounce
 *
 * The Zephyr kernel stubs in Section 1 let you write k_work_delayable,
 * atomic_cas, and k_uptime_get exactly as you would on Zephyr — they map
 * to pthreads + C11 atomics so the code compiles on any Linux / macOS / WSL
 * machine without a Zephyr install.
 *
 * Run the program first and read the output before editing anything.
 * You will see the broken behavior clearly.
 *
 * Your task: fix rx_isr_handler() and implement post_rx_isr_handler() so
 * the output matches the expected behavior in README.md.
 *
 * Sections:
 *   1. Zephyr-like stubs            — DO NOT MODIFY
 *   2. Pin state + mode definitions  — DO NOT MODIFY
 *   3. Mode switch handler           — FIX THIS
 *   4. Pin event simulator           — DO NOT MODIFY
 *   5. main()                        — DO NOT MODIFY
 */

#define _GNU_SOURCE  /* expose POSIX/GNU extensions: usleep, clock_gettime */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

/* =========================================================================
 * SECTION 1 — Zephyr-like stubs (DO NOT MODIFY)
 *
 * Use these in your solution exactly as you would on Zephyr.
 * k_work_init_delayable, k_work_schedule, k_work_reschedule, atomic_cas,
 * atomic_set, k_uptime_get, k_msleep, LOG_INF/WRN/DBG.
 * ========================================================================= */

/* --- Kernel work types -------------------------------------------------- */

struct k_work {
    int _pad; /* opaque — do not access directly */
};

typedef void (*k_work_handler_t)(struct k_work *work);

/*
 * k_work_delayable — a work item that runs after a programmable delay.
 *
 * On Zephyr this is a first-class kernel object backed by a k_timer and the
 * system workqueue. The POSIX implementation here uses a detached pthread as
 * the timer. Semantics match Zephyr:
 *   k_work_schedule   — schedule only if not already pending (no-op if busy)
 *   k_work_reschedule — always cancel-and-restart with the new delay
 *
 * On Zephyr the handler signature is void handler(struct k_work *work).
 * To recover the outer struct use CONTAINER_OF(work, struct k_work_delayable, work).
 * In this simulation the outer struct is not needed (all state is module-level),
 * so (void)work in your handler body is fine.
 */
struct k_work_delayable {
    struct k_work    work;       /* must be first — matches Zephyr layout */
    k_work_handler_t handler;
    pthread_mutex_t  lock;
    bool             pending;
    int              generation; /* incremented on each (re)schedule */
};

struct _dwork_ctx {
    struct k_work_delayable *w;
    int gen;
    int delay_ms;
};

static void *_dwork_runner(void *arg)
{
    struct _dwork_ctx *ctx = (struct _dwork_ctx *)arg;
    struct k_work_delayable *w = ctx->w;
    int my_gen   = ctx->gen;
    int delay_ms = ctx->delay_ms;
    free(ctx);

    usleep((useconds_t)delay_ms * 1000);

    pthread_mutex_lock(&w->lock);
    bool run = (w->generation == my_gen);
    if (run) w->pending = false;
    pthread_mutex_unlock(&w->lock);

    if (run) w->handler(&w->work);
    return NULL;
}

static void k_work_init_delayable(struct k_work_delayable *w,
                                   k_work_handler_t handler)
{
    memset(w, 0, sizeof(*w));
    w->handler = handler;
    pthread_mutex_init(&w->lock, NULL);
}

/*
 * k_work_schedule — submit work to run after delay_ms.
 * No-op (returns 0) if the work item is already pending.
 * Returns 1 if newly scheduled.
 *
 * ISR-safe on Zephyr. Use this when you want "fire once, ignore re-entry."
 */
static int k_work_schedule(struct k_work_delayable *w, int delay_ms)
{
    pthread_mutex_lock(&w->lock);
    if (w->pending) {
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    w->pending = true;
    w->generation++;
    struct _dwork_ctx *ctx = malloc(sizeof(*ctx));
    ctx->w        = w;
    ctx->gen      = w->generation;
    ctx->delay_ms = delay_ms;
    pthread_t t;
    pthread_create(&t, NULL, _dwork_runner, ctx);
    pthread_detach(t);
    pthread_mutex_unlock(&w->lock);
    return 1;
}

/*
 * k_work_reschedule — cancel any pending run and start a fresh timer.
 * Always restarts the countdown, even if work is already pending.
 * Returns 1 always.
 *
 * ISR-safe on Zephyr. Use this when each new edge should reset the window.
 */
static int k_work_reschedule(struct k_work_delayable *w, int delay_ms)
{
    pthread_mutex_lock(&w->lock);
    w->pending = true;
    w->generation++; /* old timer thread will exit without running */
    struct _dwork_ctx *ctx = malloc(sizeof(*ctx));
    ctx->w        = w;
    ctx->gen      = w->generation;
    ctx->delay_ms = delay_ms;
    pthread_t t;
    pthread_create(&t, NULL, _dwork_runner, ctx);
    pthread_detach(t);
    pthread_mutex_unlock(&w->lock);
    return 1;
}

/* --- k_timeout_t -------------------------------------------------------- */

typedef int k_timeout_t; /* milliseconds in this simulation */

/* K_MSEC — convert ms to k_timeout_t (matches Zephyr macro). */
#define K_MSEC(ms) (ms)

/* --- Atomic operations -------------------------------------------------- */

/*
 * These map Zephyr's atomic_t API to C11 <stdatomic.h>.
 *
 * On nRF52832 (Cortex-M4), Zephyr's atomic_cas compiles to an LDREX/STREX
 * pair — a hardware exclusive-access operation that is indivisible even
 * across cores and ISR preemption. C11 atomic_compare_exchange_strong gives
 * the same guarantee on x86/ARM when compiled with -O1 or higher.
 */
typedef atomic_int atomic_t;
#define ATOMIC_INIT(val) (val)

/*
 * atomic_cas — compare-and-swap.
 * If *ptr == old_val, atomically set *ptr = new_val and return true.
 * Otherwise leave *ptr unchanged and return false.
 */
static inline bool atomic_cas(atomic_t *ptr, int old_val, int new_val)
{
    return atomic_compare_exchange_strong(ptr, &old_val, new_val);
}

/* atomic_set — unconditional atomic write. */
static inline void atomic_set(atomic_t *ptr, int val)
{
    atomic_store(ptr, val);
}

/* --- Time --------------------------------------------------------------- */

static int64_t _prog_start_ms;

/* k_uptime_get — milliseconds elapsed since program start. */
static int64_t k_uptime_get(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return now_ms - _prog_start_ms;
}

/* k_msleep — sleep for N milliseconds (thread context only). */
static void k_msleep(int ms)
{
    usleep((useconds_t)ms * 1000);
}

/* --- Logging ------------------------------------------------------------ */

/*
 * On Zephyr, LOG_INF / LOG_WRN write to a deferred ring buffer — safe from
 * both ISR and thread context. Here they are plain printf, which is safe ONLY
 * from thread context. Calling printf from rx_isr_handler is Bug 3.
 */
#define LOG_LEVEL_DBG 4
#define LOG_MODULE_REGISTER(name, level) /* no-op in POSIX simulation */
#define LOG_DBG(fmt, ...) printf("[DBG]  " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) printf("[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) printf("[ERR]  " fmt "\n", ##__VA_ARGS__)


/* =========================================================================
 * SECTION 2 — Pin state + mode definitions (DO NOT MODIFY)
 *
 * Hardware context:
 *   The real Phase firmware uses two pogo pins on each half:
 *     tx_pin — GPIO_OUTPUT_ACTIVE: drives a logic-1 onto the contact bus.
 *     rx_pin — GPIO_INPUT: reads the bus.
 *
 *   When both halves are clipped together, rx_pin sees tx_pin's driven high
 *   (logic 1 → Mouse mode). When separated, rx_pin pulls low (logic 0 →
 *   Gamepad mode). A GPIO_INT_EDGE_BOTH interrupt fires on either transition.
 *
 *   Here we simulate both pins as a single integer: g_pogo_pin.
 * ========================================================================= */

/* Simulated rx_pin: 1 = halves connected, 0 = disconnected. */
static volatile int g_pogo_pin = 0;

/* gpio_pin_get_dt(&rx_pin) equivalent. */
static int pogo_pin_read(void)
{
    return g_pogo_pin;
}

/* Matches src/events/mode_switch_event.h */
typedef enum {
    MOUSE_MODE    = 0,
    GAMEPAD_MODE  = 1,
    UNDEFINED_MODE = 2,
} device_mode_t;

static device_mode_t g_current_mode  = UNDEFINED_MODE;
static int64_t       g_last_switch_ms = 0;

/*
 * register_activity — in the real firmware this calls register_activity()
 * from idle_watcher.h, resetting the idle-sleep countdown so the device does
 * not power off mid-connection-sequence.
 * Must be called from thread context only (not ISR).
 */
static void register_activity(void)
{
    LOG_INF("Activity registered from: mode_switch_handler");
}

/*
 * submit_switch_event — stand-in for APP_EVENT_SUBMIT(new_mode_switch_event()).
 * In the real firmware this posts a mode_switch_event to the app_event_manager;
 * radio modules (ble_hid_transport, esb_txrx) subscribe and switch transports.
 * Must be called from thread context only.
 */
static void submit_switch_event(device_mode_t new_mode)
{
    int64_t now   = k_uptime_get();
    int64_t delta = now - g_last_switch_ms;
    g_last_switch_ms = now;

    const char *from = (g_current_mode == MOUSE_MODE)    ? "MOUSE"
                     : (g_current_mode == GAMEPAD_MODE)  ? "GAMEPAD"
                     :                                     "UNDEFINED";
    const char *to   = (new_mode == MOUSE_MODE) ? "MOUSE" : "GAMEPAD";

    printf("[MODE] t=%lldms  %s --> %s  (pin=%d, +%lldms since last)\n",
           (long long)now, from, to, pogo_pin_read(), (long long)delta);

    g_current_mode = new_mode;
}

/* Matches switch_to_mouse_mode() in mode_switch_handler.c */
static void switch_to_mouse_mode(void)
{
    if (g_current_mode == MOUSE_MODE) return;
    LOG_DBG("Switch to MOUSE mode");
    submit_switch_event(MOUSE_MODE);
}

/* Matches switch_to_gamepad_mode() in mode_switch_handler.c */
static void switch_to_gamepad_mode(void)
{
    if (g_current_mode == GAMEPAD_MODE) return;
    LOG_DBG("Switch to GAMEPAD mode");
    submit_switch_event(GAMEPAD_MODE);
}


/* =========================================================================
 * SECTION 3 — Mode switch handler (FIX THIS SECTION)
 *
 * There are three bugs in rx_isr_handler and two features are missing.
 * Find them, fix rx_isr_handler, and implement post_rx_isr_handler.
 * ========================================================================= */

#define MODULE mode_switch_handler
LOG_MODULE_REGISTER(MODULE, LOG_LEVEL_DBG);

/* Hold-time before committing a disconnect → GAMEPAD transition. */
#define RX_PIN_DEBOUNCE_DELAY_MS 200
/* Hold-time before committing a connect → MOUSE transition. */
#define RX_PIN_CONNECT_DELAY_MS  500

static struct k_work_delayable post_rx_isr_work;

/*
 * work_submitted — atomic gate sometimes used to prevent double-scheduling.
 * 0 = ready; 1 = work item is already pending.
 * Whether your solution requires this depends on which scheduling API you
 * choose. See notes.md question 3.
 */
atomic_t work_submitted = ATOMIC_INIT(0);

/*
 * g_edge_pin — pin state captured at the moment the ISR fires.
 * Used to determine the correct hold-time (direction-dependent hysteresis).
 */
static int g_edge_pin = 0;

/*
 * TODO — Implement post_rx_isr_handler.
 *
 * This runs in thread context after the debounce hold-time expires.
 * The hold-time was applied by k_work_reschedule, so do NOT call k_msleep here.
 *
 * It should:
 *   a) Call register_activity() to notify the idle watcher.
 *   b) Re-read the pin state with pogo_pin_read().
 *   c) Call switch_to_mouse_mode() or switch_to_gamepad_mode() as appropriate.
 *
 * On real Zephyr the signature is void handler(struct k_work *work).
 * Use (void)work — the parameter exists for API compatibility only.
 */
static void post_rx_isr_handler(struct k_work *work)
{
    /* TODO: implement me */
    (void)work;
}

/*
 * rx_isr_handler — called when the GPIO interrupt fires (pin state changed).
 *
 * BUG 1: usleep() is called here. On Zephyr, any blocking call from a GPIO
 *   callback (ISR context) is undefined behavior: k_msleep() tries to yield
 *   the scheduler, which is illegal in ISR context and causes a kernel panic
 *   or hard fault on nRF52832. On bare metal it holds the CPU for 200ms,
 *   delaying every other interrupt — sensor reads, BLE events, timers.
 *
 * BUG 2: The mode switch commits immediately without re-reading the pin.
 *   If the pin glitched and returned to its previous state during the 200ms
 *   sleep, the switch happens anyway — a phantom mode change.
 *
 * BUG 3: printf() acquires an internal lock (flockfile). If the interrupted
 *   thread already holds that lock, calling printf from the ISR deadlocks.
 *   Zephyr's LOG_INF defers writes to a ring buffer precisely to avoid this.
 *
 * Fix: The ISR must only capture g_edge_pin, choose the hold-time, and call
 *   k_work_reschedule so the debounce timer restarts on every edge.
 *   All blocking, logging, and mode-switch logic belongs in
 *   post_rx_isr_handler (thread context).
 */
static void rx_isr_handler(void)
{
    g_edge_pin = pogo_pin_read();

    int hold_ms = (g_edge_pin == 1) ? RX_PIN_CONNECT_DELAY_MS
                                    : RX_PIN_DEBOUNCE_DELAY_MS;

    k_work_reschedule(&post_rx_isr_work, K_MSEC(hold_ms));
}


/* =========================================================================
 * SECTION 4 — Pin event simulator (DO NOT MODIFY)
 *
 * Fires pogo-pin events with realistic timing, including glitches that a
 * correct debounce implementation must ignore.
 * ========================================================================= */

static void fire_pin_event(int new_state, const char *description)
{
    if (description) printf("\n[SIM] --- %s ---\n", description);
    g_pogo_pin = new_state;
    rx_isr_handler();
}

static void *simulator_thread(void *arg)
{
    (void)arg;
    printf("[SIM] Starting pin event sequence. Observe the output.\n");
    printf("[SIM] With the broken code you will see spurious mode switches.\n\n");

    k_msleep(500);

    /* --- Test 1: Clean connect.
     * Hold pin=1 for 1 second. A correct implementation switches GAMEPAD →
     * MOUSE after the 500ms connect hold-time. */
    fire_pin_event(1, "Clean connect (hold 1000ms) — expect MOUSE after 500ms");
    k_msleep(1000);

    /* --- Test 2: Short glitch — disconnect then reconnect within 50ms.
     * 50ms < 200ms disconnect threshold. A correct implementation must NOT
     * switch to GAMEPAD. */
    fire_pin_event(0, "Short glitch — disconnect for 50ms (expect NO mode change)");
    k_msleep(50);
    fire_pin_event(1, NULL); /* reconnect — still in MOUSE */
    k_msleep(1000);

    /* --- Test 3: Clean disconnect.
     * Hold pin=0 for 1 second. A correct implementation switches MOUSE →
     * GAMEPAD after the 200ms disconnect hold-time. */
    fire_pin_event(0, "Clean disconnect (hold 1000ms) — expect GAMEPAD after 200ms");
    k_msleep(1000);

    /* --- Test 4: Rapid toggling.
     * g_pogo_pin is currently 0 (after the clean disconnect above).
     * Five connect/disconnect edges, each 80ms apart. After 5 toggles from
     * 0 the pin ends at 1 (connected).
     *
     * Broken code:   5 mode switches — one per ISR, immediate commit.
     * Correct fix:   0 mode changes during the sequence — each edge restarts
     *                the debounce timer; no single direction is held long
     *                enough to commit. One clean switch after the pin
     *                stabilises at the end.
     */
    printf("\n[SIM] --- Rapid toggle x5 (80ms each, starting from pin=0) ---\n");
    printf("[SIM]   Broken code:   5 mode switches\n");
    printf("[SIM]   Correct fix:   0 during toggles, 1 after stabilising\n\n");
    for (int i = 0; i < 5; i++) {
        g_pogo_pin = !g_pogo_pin;
        rx_isr_handler();
        k_msleep(80);
    }
    /* After 5 toggles from 0: pin ends at 1 (connected). */
    k_msleep(700);

    /* --- Test 5: Stable reconnect after rapid toggling.
     * g_pogo_pin is 1 (from the last rapid toggle). Firing the ISR again
     * confirms a stable connect. Expect: GAMEPAD → MOUSE after 500ms.
     * (If test 4 produced no mode change, we are still in GAMEPAD here.) */
    fire_pin_event(1, "Stable reconnect — expect MOUSE after 500ms");
    k_msleep(1500);

    printf("\n[SIM] Sequence complete.\n");
    return NULL;
}


/* =========================================================================
 * SECTION 5 — main() (DO NOT MODIFY)
 * ========================================================================= */

int main(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    _prog_start_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    k_work_init_delayable(&post_rx_isr_work, post_rx_isr_handler);
    g_last_switch_ms = k_uptime_get();

    pthread_t sim;
    pthread_create(&sim, NULL, simulator_thread, NULL);
    pthread_join(sim, NULL);

    /* Give any pending work a moment to finish */
    k_msleep(600);
    printf("\n[DONE]\n");
    return 0;
}
