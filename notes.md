# Task 5.2 — Notes

**Max 400 words total. Answer all four questions.**

---

## Q1 — ISR blocking mechanism

Why is calling `usleep()` (or `k_msleep()`) inside a Zephyr GPIO callback
dangerous? Explain the mechanism — not just "it's slow" but what specifically
breaks on the nRF52832.

<!-- your answer -->

Calling usleep() or k_msleep() inside a Zephyr GPIO callback is unsafe because the callback executes in ISR context. k_msleep() attempts to suspend the current context and invoke the scheduler, but ISRs cannot sleep or yield on the nRF52832. Zephyr therefore triggers a kernel panic, assertion, or hard fault. Even with bare-metal delays, the CPU remains stuck inside the ISR, delaying BLE timing, timers, UART, and other interrupts. Another issue is committing the mode switch without re-reading the GPIO after the delay, allowing glitches to create false transitions. printf() is also unsafe because it may lock internal stdio resources and deadlock if interrupted mid-print.

---

## Q2 — State machine

Describe your debounce state machine. How many states? Draw it in ASCII or
plain English. What happens if a second GPIO edge fires while the work item
is pending (hold-time counting down)?

<!-- your answer -->

IDLE
  -> GPIO edge
DEBOUNCE_PENDING
  -> new edge: restart timer
  -> timer expires: validate pin and switch mode
IDLE

The ISR captures the pin state and schedules delayed work:

pin = 1 → 500 ms connect delay
pin = 0 → 200 ms disconnect delay

k_work_reschedule() restarts the debounce timer on every edge. When the timer expires, the worker re-reads the GPIO and commits the mode only if the signal stayed stable for the full hold time.

If another GPIO edge occurs while work is pending, the existing timer is canceled and restarted, preventing bounce-induced mode changes.

---

## Q3 — Work scheduling and atomicity

The file pre-declares `atomic_t work_submitted`. With a `k_work_reschedule`-based
solution, do you still need it? Explain why or why not.

If you did use `atomic_cas` in your ISR, explain what hardware mechanism it
relies on and why a plain C boolean check (`if (!work_submitted) { work_submitted = true; }`)
is unsafe in ISR context.

<!-- your answer -->

With k_work_reschedule(), work_submitted is unnecessary because the API already guarantees “cancel old timer and restart with new delay.” Only the newest debounce window survives.

If atomic_cas() were used, it would rely on Cortex-M4 exclusive-access instructions (LDREX/STREX) to make the compare-and-swap operation indivisible, even during interrupt preemption.

A plain boolean check:

if (!work_submitted) {
    work_submitted = true;
}

is unsafe because the read and write are separate operations. Another ISR or thread could preempt between them, causing race conditions and duplicate scheduling.

---

## Q4 — Real Zephyr translation

What would change if you implemented this on a real nRF52832 with Zephyr?
Name the specific APIs for GPIO interrupt registration, work queue
initialization, and event submission. Which parts of your simulation code
map to Zephyr unchanged?

<!-- your answer -->

Real Zephyr APIs would include:

GPIO setup: gpio_pin_configure_dt()
Interrupt registration: gpio_init_callback(), gpio_add_callback(), gpio_pin_interrupt_configure_dt()
Workqueue handling: k_work_init_delayable(), k_work_reschedule()
Event submission: APP_EVENT_SUBMIT(new_mode_switch_event())

The debounce architecture maps almost unchanged:

ISR only captures edges
delayed work performs validation
atomic_t, k_work_delayable, and k_uptime_get() remain the same
mode switching still occurs from thread context rather than ISR context.
