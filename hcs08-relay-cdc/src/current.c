/*
 * current.c - AC current measurement (see current.h).
 *
 * Integer only (no float library). Ranges for 12-bit samples, N = 100:
 *   d = x - mean            |d| <= 4095
 *   sum of d^2              <= 100 * 4095^2 = 1.7e9   (uint32)
 *   sum of d * table        <= 100 * 4095 * 127 = 52e6 (int32)
 */
#include "jm60.h"
#include "board.h"
#include "current.h"
#include "usb_cdc.h"
#include "timer.h"

#define N CURRENT_SAMPLES
#define CH 6

/* cos/sin of 2*pi*k/20 (50 Hz at 1 kHz sampling), scaled by 127 */
static const int8_t cos_t[20] = {
    127, 121, 103, 75, 39, 0, -39, -75, -103, -121,
    -127, -121, -103, -75, -39, 0, 39, 75, 103, 121
};
static const int8_t sin_t[20] = {
    0, 39, 75, 103, 121, 127, 121, 103, 75, 39,
    0, -39, -75, -103, -121, -127, -121, -103, -75, -39
};
/* A 50 Hz sine of amplitude A (ADC steps) gives |X| = A * P * 5, with
 * P = sum over one period of cos_t[k] * cos(2 pi k / 20) = 1272.17 (127 * 10
 * without the rounding of the table). Its RMS is A / sqrt(2), so
 * rms * 16 = (|X| >> 5) * 2032 / RMS50_DIV, RMS50_DIV = 5 P sqrt(2) * 2032 / 512
 * = 35701 (checked in test/host_test.c). */
#define RMS50_DIV 35701UL

static const uint8_t channel[CH] = CURRENT_ADC_CHANNELS;

static __xdata uint16_t buf[N][CH];
static volatile uint8_t idx;           /* next sample row, N = window full */
static uint16_t *wp;                    /* write pointer of the ISR */
static uint8_t paused;
static uint8_t valid;
static uint8_t irq_ok;                  /* sampling interrupt usable */
static __xdata uint16_t ma50[CH], marms[CH];

/* ------------------------------------------------------------------ */
/* Sampling                                                           */
/* ------------------------------------------------------------------ */

#if defined(HOST_TEST) || defined(SIM_TEST)
/* gcc host test / uCsim test: samples come from the test harness */
extern uint16_t host_adc(uint8_t ch);
#define ADC_READ(ch) host_adc(ch)
#define ISR_SIGNATURE void current_isr(void)
#else
#define ISR_SIGNATURE void current_isr(void) __interrupt(VECTOR_TPM2_OVF)
static uint16_t adc_read(uint8_t ch)
{
    ADCSC1 = ch;                        /* starts the conversion */
    while (!(ADCSC1 & ADCSC1_COCO))
        ;
    return ((uint16_t)ADCRH << 8) | ADCRL;
}
#endif

/* 1 kHz: one row of samples. No function calls and no multiplications on
 * the target: SDCC library routines are not reentrant and the main loop may
 * be inside one when the interrupt hits. Hence the write pointer instead
 * of buf[idx] (which would multiply). */
ISR_SIGNATURE
{
    uint8_t c;
    uint16_t *p;

    TPM2SC &= (uint8_t)~TPMSC_TOF;     /* read with TOF set, then write 0 */
    if (idx >= N)
        return;                         /* window full, waiting for main */
    if (idx == 0)
        wp = &buf[0][0];
    p = wp;
    for (c = 0; c < CH; c++) {
#if defined(HOST_TEST) || defined(SIM_TEST)
        *p++ = ADC_READ(channel[c]);
#else
        ADCSC1 = channel[c];
        while (!(ADCSC1 & ADCSC1_COCO))
            ;
        *p++ = ((uint16_t)ADCRH << 8) | ADCRL;
#endif
    }
    wp = p;
    idx++;
}

/* The CPU fetches the TPM2 vector from 0xFFDA, or - with vector
 * redirection (FNORED = 0 and flash protection on) - from the same offset
 * below the protected area. Interrupts are only enabled if that slot really
 * points to current_isr(). Bootloader 1.0 had redirection off by mistake:
 * enabling the interrupt there would jump into erased flash. */
static uint8_t vector_ok(void)
{
#if defined(HOST_TEST) || defined(SIM_TEST)
    return 1;
#else
    uint16_t va = 0xFFDA;
    if (!(FOPT & FOPT_FNORED) && !(FPROT & FPROT_FPDIS))
        va = (uint16_t)(((uint16_t)(FPROT & 0xFE) << 8) + 0x200 - 0x40 + (0xFFDA - 0xFFC0));
    return (((uint16_t)*HW_PTR(va) << 8) | *HW_PTR(va + 1)) == (uint16_t)current_isr;
#endif
}

uint8_t current_available(void)
{
    return irq_ok;
}

void current_init(void)
{
    uint8_t c;

    /* ADC: bus/2/2 = 6 MHz ADC clock, 12 bit, software trigger */
    ADCCFG = (1 << 5) | (1 << 2) | 1;
    ADCSC2 = 0;
    for (c = 0; c < CH; c++) {          /* sensor pins: digital input off */
        if (channel[c] < 8)
            APCTL1 |= (uint8_t)(1u << channel[c]);
        else
            APCTL2 |= (uint8_t)(1u << (channel[c] - 8));
    }
    ADCSC1 = ADC_CH_OFF;

    idx = 0;
    paused = 0;
    valid = 0;
    /* TPM2: bus clock, overflow every 24000 cycles = 1 ms, interrupt */
    TPM2SC = 0;
    TPM2MODH = (uint8_t)(23999u >> 8);
    TPM2MODL = (uint8_t)23999u;
    irq_ok = vector_ok();
    if (irq_ok)
        TPM2SC = TPMSC_TOIE | TPMSC_CLKSA;
}

void current_pause(void)
{
    DisableInterrupts();
    paused = 1;
}

void current_resume(void)
{
    if (idx < N)
        idx = 0;                        /* the gap would distort this window */
    paused = 0;
    EnableInterrupts();
}

/* ------------------------------------------------------------------ */
/* Evaluation                                                         */
/* ------------------------------------------------------------------ */

static uint16_t isqrt32(uint32_t v) __reentrant
{
    uint32_t bit = 1UL << 30, r = 0;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint16_t)r;
}

/* RMS in ADC steps * 16 -> mA */
static uint16_t x16_to_ma(uint32_t x16) __reentrant
{
    uint32_t ma = x16 * CURRENT_UA_PER_LSB / 16000UL;
    return ma > 65535UL ? 65535u : (uint16_t)ma;
}

/* __reentrant: the many 32-bit locals live on the stack, not in the small
 * direct page */
static void evaluate(uint8_t c) __reentrant
{
    uint32_t sum = 0, sq = 0, mag;
    int32_t re = 0, im = 0;
    uint16_t mean;
    uint8_t k, t = 0, shift = 0;

    for (k = 0; k < N; k++)
        sum += buf[k][c];
    mean = (uint16_t)((sum + N / 2) / N);

    for (k = 0; k < N; k++) {
        int16_t d = (int16_t)(buf[k][c] - mean);
        sq += (uint32_t)((int32_t)d * d);
        re += (int32_t)d * cos_t[t];
        im += (int32_t)d * sin_t[t];
        if (++t == 20)
            t = 0;
    }

    /* true RMS of the AC part */
    marms[c] = x16_to_ma(isqrt32((sq / N) << 8));

    /* 50 Hz: |X| = sqrt(re^2 + im^2), scaled down until the squares fit */
    if (re < 0) re = -re;
    if (im < 0) im = -im;
    while (re > 46000L || im > 46000L) {
        re >>= 1;
        im >>= 1;
        shift++;
    }
    mag = (uint32_t)isqrt32((uint32_t)(re * re) + (uint32_t)(im * im)) << shift;
    ma50[c] = x16_to_ma((mag >> 5) * 2032UL / RMS50_DIV);
}

void current_task(void)
{
    uint8_t c;
    if (idx < N || paused)
        return;
    for (c = 0; c < CH; c++)
        evaluate(c);
    valid = 1;
    idx = 0;                            /* next window */
}

uint8_t current_get(uint8_t i, uint16_t *ma_50hz, uint16_t *ma_rms)
{
    if (i >= CH || !valid)
        return 0;
    *ma_50hz = ma50[i];
    *ma_rms = marms[i];
    return 1;
}

/* ------------------------------------------------------------------ */
/* ADC debug report                                                   */
/* ------------------------------------------------------------------ */

static void put_dec(uint16_t v)
{
    char s[6];
    uint8_t n = 0;
    do {
        s[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        cdc_putc(s[--n]);
}

#if defined(HOST_TEST) || defined(SIM_TEST)
#define DEBUG_READ(ch) host_adc(ch)
#else
#define DEBUG_READ(ch) adc_read(ch)
#endif

void current_adc_report(void) __reentrant
{
    static const uint8_t list[13] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, ADC_CH_BANDGAP };
    uint8_t i, k, save1 = APCTL1, save2 = APCTL2;
    uint16_t bg = 0;

    TPM2SC &= (uint8_t)~TPMSC_TOIE;     /* stop the sampling interrupt */
    APCTL1 = 0xFF;                      /* AD0-AD7 = PTB0-PTB7 analog */
    APCTL2 = 0x0F;                      /* AD8-AD11 = PTD0, PTD1, PTD3, PTD4 */
    SPMSC1 |= SPMSC1_BGBE;              /* bandgap buffer on */

    for (i = 0; i < 13; i++) {
        uint8_t ch = list[i];
        uint16_t v, lo = 0xFFFF, hi = 0;
        uint32_t sum = 0;
        for (k = 0; k < 64; k++) {      /* 64 ms = 3.2 mains periods */
            v = DEBUG_READ(ch);
            sum += v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            delay_ms(1);
        }
        v = (uint16_t)(sum / 64);
        if (ch == ADC_CH_BANDGAP) {
            bg = v;
            continue;
        }
        cdc_puts("ADC ");
        put_dec(ch);
        cdc_puts(" mean ");
        put_dec(v);
        cdc_puts(" pp ");
        put_dec((uint16_t)(hi - lo));
        cdc_puts("\r\n");
    }
    /* bandgap 1.20 V typical: VDD = 1200 mV * 4096 / reading */
    cdc_puts("ADC VDD ");
    put_dec(bg ? (uint16_t)(1200UL * 4096UL / bg) : 0);
    cdc_puts(" mV (bandgap ");
    put_dec(bg);
    cdc_puts(")\r\n");

    APCTL1 = save1;
    APCTL2 = save2;
    ADCSC1 = ADC_CH_OFF;
    idx = 0;
    if (irq_ok)
        TPM2SC |= TPMSC_TOIE;
}
