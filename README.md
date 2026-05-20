# Phase Firmware Task 5.2 — Pogo Pin Mode Switch Debounce

**Time budget:** 3–5 hours

---

## Background

Phase is a split gaming mouse. The two halves connect mechanically via
**pogo pins** — spring-loaded electrical contacts on the inner edges of each
half.

Each half has two pogo contacts: a **tx pin** (GPIO output, drives logic-1
onto the contact bus) and an **rx pin** (GPIO input, reads the bus). When the
halves clip together, the rx pin reads the driven high from the opposite half
(logic 1 → **Mouse mode**: both halves act as one combined mouse over UART).
When disconnected, the rx pin floats low (logic 0 → **Gamepad mode**: each
half is independent over BLE).

A `GPIO_INT_EDGE_BOTH` interrupt fires whenever the rx pin transitions. The
interrupt callback is the entry point for all debounce and mode-switch logic.

### The problem

The current implementation has three bugs in the interrupt handler:

1. **Blocking sleep in ISR context** — calling `usleep(200ms)` directly in
   a GPIO callback. On Zephyr this causes a kernel panic or hard fault on the
   nRF52832. On bare metal it holds the CPU for 200ms, blocking all other
   interrupts.

2. **No pin re-read after the sleep** — the mode switch commits based on the
   pin state at interrupt time. A glitch that resolves within 200ms still
   triggers a full mode change.

3. **Unsafe `printf` in ISR context** — `printf` acquires an internal lock.
   If the interrupted thread holds that lock, this deadlocks.

Additionally, two features are missing: asymmetric hold-times and an idle
watcher notification.

---

## What you're given

`mode_switch_handler.c` is a self-contained POSIX simulation of the real
Phase firmware module at `src/device_mode_switch/mode_switch_handler.c`. It
compiles on Linux, macOS, or WSL with no dependencies beyond pthreads.

```
make        # build
make run    # build and run
```

The Zephyr API stubs in Section 1 let you write `k_work_delayable`,
`atomic_cas`, and `k_uptime_get` exactly as you would on real Zephyr hardware:

| Stub in this file | Real Zephyr equivalent |
|---|---|
| `k_work_init_delayable` | `k_work_init_delayable` (same) |
| `k_work_schedule` | `k_work_schedule` (same) |
| `k_work_reschedule` | `k_work_reschedule` (same) |
| `atomic_t`, `atomic_cas`, `atomic_set` | `atomic_t`, `atomic_cas`, `atomic_set` (same) |
| `K_MSEC(ms)` | `K_MSEC(ms)` (same) |
| `pogo_pin_read()` | `gpio_pin_get_dt(&rx_pin)` |
| `submit_switch_event()` | `APP_EVENT_SUBMIT(new_mode_switch_event())` |
| `register_activity()` | `register_activity()` from `idle_watcher.h` |

**Run it first.** You will see the broken behavior — spurious mode switches on
every edge, including glitches that should be ignored.

---

## What to do

### 1. Fix `rx_isr_handler` — no blocking, no logging, no mode switch

The ISR must return immediately. It must only:
- Capture the latest pin state.
- Choose the hold-time for that transition direction.
- Schedule or reschedule the delayed work item so the debounce timer restarts
  on every edge.

Every GPIO edge must restart the debounce window. A mode switch may only
commit after the pin has remained stable for the full hold-time with no
further edges.

Use `k_work_reschedule()` so that each new edge cancels and restarts the
countdown. A solution that uses only `k_work_schedule` and ignores later edges
is incomplete: the pin may not have been stable for the required hold-time by
the time the handler fires.

### 2. Implement `post_rx_isr_handler`

This runs in thread context after the hold-time expires. It must:
- Call `register_activity()` to notify the idle watcher.
- Re-read the pin state.
- Call `switch_to_mouse_mode()` or `switch_to_gamepad_mode()` as appropriate.

The hold-time is already handled by `k_work_reschedule` — do **not** call
`k_msleep` in the handler.

### 3. Asymmetric hold-times

| Transition | Hold-time |
|---|---|
| Disconnect (pin 1 → 0) → GAMEPAD | 200 ms |
| Connect (pin 0 → 1) → MOUSE | 500 ms |

The longer connect threshold absorbs mechanical bounce as the user aligns the
halves. The shorter disconnect threshold keeps GAMEPAD response snappy.

### 4. Fill in `notes.md`

Answer all four questions (max 400 words). See the questions in `notes.md`.

---

## Expected output

With the broken code: a `[MODE]` line fires on every ISR call, including
glitches and every step of the rapid-toggle test.

With a correct implementation:

```
[SIM] --- Clean connect (hold 1000ms) ---
[INFO] Activity registered from: mode_switch_handler
[MODE] t=~1000ms  UNDEFINED --> MOUSE  (pin=1, +Xms since last)
      ^-- the connect edge fires at ~500ms; mode commits ~500ms after that edge

[SIM] --- Short glitch — disconnect for 50ms ---
(no [MODE] line — glitch recovered before the 200ms window expired)

[SIM] --- Clean disconnect (hold 1000ms) ---
[INFO] Activity registered from: mode_switch_handler
[MODE] t=~Xms  MOUSE --> GAMEPAD  (pin=0, +Yms since last)

[SIM] --- Rapid toggle x5 ---
(no [MODE] lines — each edge resets the timer; no direction holds for the
 full threshold)

[SIM] --- Stable reconnect ---
[INFO] Activity registered from: mode_switch_handler
[MODE] t=~Xms  GAMEPAD --> MOUSE  (pin=1, +Yms since last)
```

---

## Submission

### Git workflow

We review your **commit history** as part of the evaluation, not just the
final diff. Commit incrementally as you work. We expect:

- **Multiple focused commits** — not one big "fix everything" dump.
- **Commit messages that explain why**, not just what. The diff already shows
  what changed; the message should explain the reasoning.
- **Useful code comments** — only where the *why* is non-obvious. Do not
  comment every line. Do not describe what the code does; the code does that.

Example commit sequence (this is a guide, not a requirement):

```
fix(isr): remove blocking usleep and printf from interrupt context

Moving to a deferred work item; ISR must return in microseconds on
nRF52832 to avoid stalling BLE and sensor interrupt handlers.
```

```
feat(debounce): restart hold-time on every edge using k_work_reschedule

k_work_schedule ignores re-entry; k_work_reschedule cancels the in-flight
timer. Required so that a rapid-toggle sequence never commits a mode change
unless one direction is stable for the full hold-time.
```

```
feat(debounce): add asymmetric thresholds (200ms disconnect, 500ms connect)

Connect needs 500ms to absorb mechanical pogo bounce during alignment.
Disconnect can be faster — the user is deliberately pulling the halves apart.
```

```
docs(notes): answer the four notes.md questions
```

### Steps

1. Create a **private** repository on GitHub using this repo as a template
   (click "Use this template" → "Create a new repository" → set to Private).

2. Clone your new repo, make your changes, commit as you go.

3. When done, add `sharath@pixelpawlabs.com` as a collaborator
   (Settings → Collaborators → Add people).

4. Reply to the task email with:
   - Your repository URL
   - The terminal output of `make run` against your solution
   - Honest time estimate

---

## AI policy

You may use any AI assistant.

You are responsible for understanding every line you submit. If you cannot
explain a design decision live, it counts against you regardless of how the
code got there. If AI suggested something you disagreed with or had to
correct, mention it in `notes.md`.
