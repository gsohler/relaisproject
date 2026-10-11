/*
 * current.h - AC current measurement with the ACS712 sensors.
 *
 * TPM2 interrupt at 1 kHz: each tick converts the six sensor channels.
 * After 100 ms (100 samples per channel = 5 mains periods) the main loop
 * evaluates the window: mean (sensor offset) removed, then
 *   - the 50 Hz component (single-bin DFT, rejects noise and harmonics),
 *   - the true RMS of the AC part (includes harmonics and noise).
 * Then the next window starts, so values are at most ~0.2 s old.
 */
#ifndef CURRENT_H
#define CURRENT_H

#include <stdint.h>
#include "jm60.h"

#define CURRENT_SAMPLES 100     /* per channel and window, at 1 kHz */

void    current_init(void);     /* ADC + TPM2; interrupts must be enabled afterwards */
void    current_task(void);     /* call from the main loop */

/* 0 if the sampling interrupt cannot be used (vector check failed, e.g.
 * bootloader 1.0 without vector redirection). ADC report still works. */
uint8_t current_available(void);

/* Latest values of relay idx (0..5) in mA. Returns 0 before the first window. */
uint8_t current_get(uint8_t idx, uint16_t *ma_50hz, uint16_t *ma_rms);

/* Around flash writes (no interrupts allowed while the flash is busy):
 * pause stops sampling, resume restarts the window that was disturbed. */
void    current_pause(void);
void    current_resume(void);

/* Debug: mean and peak-peak of all ADC inputs AD0..AD11 and an estimate of
 * VDD from the bandgap, printed over CDC. Takes about 1 s. */
void    current_adc_report(void) __reentrant;

/* TPM2 overflow interrupt. SDCC only builds the vector table entry if this
 * prototype is visible in the file with main(). */
#if defined(HOST_TEST) || defined(SIM_TEST)
void    current_isr(void);
#else
void    current_isr(void) __interrupt(VECTOR_TPM2_OVF);
#endif

#endif
