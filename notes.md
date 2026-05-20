# Task 5.2 — Notes

**Max 400 words total. Answer all four questions.**

---

## Q1 — ISR blocking mechanism

Why is calling `usleep()` (or `k_msleep()`) inside a Zephyr GPIO callback
dangerous? Explain the mechanism — not just "it's slow" but what specifically
breaks on the nRF52832.

<!-- your answer -->

---

## Q2 — State machine

Describe your debounce state machine. How many states? Draw it in ASCII or
plain English. What happens if a second GPIO edge fires while the work item
is pending (hold-time counting down)?

<!-- your answer -->

---

## Q3 — Work scheduling and atomicity

The file pre-declares `atomic_t work_submitted`. With a `k_work_reschedule`-based
solution, do you still need it? Explain why or why not.

If you did use `atomic_cas` in your ISR, explain what hardware mechanism it
relies on and why a plain C boolean check (`if (!work_submitted) { work_submitted = true; }`)
is unsafe in ISR context.

<!-- your answer -->

---

## Q4 — Real Zephyr translation

What would change if you implemented this on a real nRF52832 with Zephyr?
Name the specific APIs for GPIO interrupt registration, work queue
initialization, and event submission. Which parts of your simulation code
map to Zephyr unchanged?

<!-- your answer -->
