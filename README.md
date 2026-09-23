# HW-Lab1-LED-blinking-and-RTOS-API
FreeRTOS-based LED control on STM32L475 (B-L475E-IOT01A) — two mutex-protected tasks trigger distinct LED blink patterns via button interrupt (short/long press) and a periodic software timer.

# STM32L475 FreeRTOS LED Control

A FreeRTOS (CMSIS-RTOS2) application on the STM32L475E-IOT01A Discovery board
that demonstrates inter-task synchronization using semaphores, a mutex, a
message queue, and a software timer.

## Overview

Two tasks share a single LED (LED2) and are mutually exclusive via a mutex,
so their blink patterns never interfere with each other:

- **Task_1** is triggered by the user button (PC13). A short press blinks
  the LED at 1 Hz for 5 seconds; a long press (held ≥ 1 s) blinks it at
  10 Hz for 5 seconds. The button edge is debounced in a dedicated task
  (via `osDelay` confirmation after an EXTI-notified semaphore), and the
  resulting press type is passed to Task_1 through a message queue.
- **Task_2** is triggered automatically every 10 seconds by a periodic
  software timer, blinking the LED at 10 Hz for 2 seconds.

## Key RTOS concepts demonstrated

- Binary semaphores for ISR-to-task and timer-callback-to-task notification
- A mutex to serialize access to a shared peripheral (the LED)
- A message queue to pass distinguishable event data (short vs. long press)
- A software timer for periodic task triggering
- Deferred interrupt handling and button debouncing without blocking the ISR

## Hardware

- Board: B-L475E-IOT01A (STM32L475VG)
- Button: PC13 (user button, active-low, on-board pull-up)
- LED: PB14 (LED2)
