/*
 * jm60.h - Minimal MC9S08JM60/JM32 register definitions for SDCC (-ms08).
 *
 * Addresses and bit positions were taken from the CodeWarrior device header
 * (mc9s08jm60.h) and NXP AN3560. Only what this project needs is defined.
 */
#ifndef JM60_H
#define JM60_H

#include <stdint.h>

#ifdef HOST_TEST
/* Host unit tests: registers and USB RAM live in a plain array */
extern uint8_t host_mem[0x10000];
#define HW_PTR(a) ((volatile uint8_t *)(host_mem + (uint16_t)(a)))
#define __xdata                 /* SDCC storage class, not needed on the host */
#define __interrupt(n)          /* SDCC interrupt function, plain function here */
#define __reentrant             /* SDCC: locals on the stack */
#else
#define HW_PTR(a) ((volatile uint8_t *)(a))
#endif
#define REG8(a) (*HW_PTR(a))

/* ---- GPIO data / data direction (direct page) ---- */
#define PTAD   REG8(0x0000)
#define PTADD  REG8(0x0001)
#define PTBD   REG8(0x0002)
#define PTBDD  REG8(0x0003)
#define PTCD   REG8(0x0004)
#define PTCDD  REG8(0x0005)
#define PTDD   REG8(0x0006)
#define PTDDD  REG8(0x0007)
#define PTED   REG8(0x0008)
#define PTEDD  REG8(0x0009)
#define PTFD   REG8(0x000A)
#define PTFDD  REG8(0x000B)
#define PTGD   REG8(0x000C)
#define PTGDD  REG8(0x000D)

/* ---- TPM1 (used as 1 ms tick) ---- */
#define TPM1SC   REG8(0x0020)
#define TPM1MODH REG8(0x0023)
#define TPM1MODL REG8(0x0024)
#define TPMSC_TOF   0x80
#define TPMSC_CLKSA 0x08        /* clock source = bus clock */

/* ---- ADC (12 bit) ---- */
#define ADCSC1 REG8(0x0010)
#define ADCSC2 REG8(0x0011)
#define ADCRH  REG8(0x0012)
#define ADCRL  REG8(0x0013)
#define ADCCFG REG8(0x0016)
#define APCTL1 REG8(0x0017)
#define APCTL2 REG8(0x0018)
#define ADCSC1_COCO 0x80
#define ADC_CH_BANDGAP 27
#define ADC_CH_OFF     0x1F

/* ---- TPM2 (current sampling interrupt) ---- */
#define TPM2SC   REG8(0x0060)
#define TPM2MODH REG8(0x0063)
#define TPM2MODL REG8(0x0064)
#define TPMSC_TOIE  0x40
#define VECTOR_TPM2_OVF 18          /* 0xFFDA, redirected to 0xDBDA */

/* ---- MCG (clock generator) ---- */
#define MCGC1  REG8(0x0048)
#define MCGC2  REG8(0x0049)
#define MCGTRM REG8(0x004A)
#define MCGSC  REG8(0x004B)
#define MCGC3  REG8(0x004C)

#define MCGSC_OSCINIT 0x02
#define MCGSC_CLKST   0x0C
#define MCGSC_IREFST  0x10
#define MCGSC_PLLST   0x20
#define MCGSC_LOCK    0x40

/* ---- USB module (direct page) ---- */
#define USBCTL0 REG8(0x0080)
#define PERID   REG8(0x0088)
#define IDCOMP  REG8(0x0089)
#define USBREV  REG8(0x008A)
#define INTSTAT REG8(0x0090)
#define INTENB  REG8(0x0091)
#define ERRSTAT REG8(0x0092)
#define ERRENB  REG8(0x0093)
#define STAT    REG8(0x0094)
#define CTL     REG8(0x0095)
#define ADDR    REG8(0x0096)
#define FRMNUML REG8(0x0097)
#define FRMNUMH REG8(0x0098)
#define EPCTL(n) REG8(0x009D + (n))     /* n = 0..6 */

#define USBCTL0_USBPHYEN  0x01
#define USBCTL0_USBVREN   0x04
#define USBCTL0_LPRESF    0x10
#define USBCTL0_USBRESMEN 0x20
#define USBCTL0_USBPU     0x40
#define USBCTL0_USBRESET  0x80

#define INT_USBRST 0x01
#define INT_ERROR  0x02
#define INT_SOFTOK 0x04
#define INT_TOKDNE 0x08
#define INT_SLEEP  0x10
#define INT_RESUME 0x20
#define INT_STALL  0x80

/* INTSTAT/ERRSTAT flags are cleared by writing 1 */
#ifdef HOST_TEST
#define INTSTAT_CLR(m) (INTSTAT &= (uint8_t)~(m))
#define ERRSTAT_CLR(m) (ERRSTAT &= (uint8_t)~(m))
#else
#define INTSTAT_CLR(m) (INTSTAT = (m))
#define ERRSTAT_CLR(m) (ERRSTAT = (m))
#endif

#define STAT_ODD   0x04
#define STAT_IN    0x08            /* 1 = last token was IN (device -> host) */
#define STAT_ENDP(s) ((uint8_t)((s) >> 4))

#define CTL_USBEN    0x01
#define CTL_ODDRST   0x02
#define CTL_CRESUME  0x04
#define CTL_TSUSPEND 0x20

#define EPCTL_EPHSHK   0x01
#define EPCTL_EPSTALL  0x02
#define EPCTL_EPTXEN   0x04
#define EPCTL_EPRXEN   0x08
#define EPCTL_EPCTLDIS 0x10

/* ---- System (high page) ---- */
#define SRS    REG8(0x1800)
#define SOPT1  REG8(0x1802)        /* write-once after reset */
#define SOPT2  REG8(0x1803)
#define SPMSC1 REG8(0x1809)
#define SPMSC1_BGBE 0x01            /* bandgap buffer for ADC channel 27 */
#define SPMSC2 REG8(0x180A)

#define SOPT1_STOPE 0x20
#define SOPT1_COPT_1S 0xC0          /* COP: 2^10 cycles of the 1 kHz LPO */

/* SRS read: cause of the last reset */
#define SRS_POR  0x80
#define SRS_PIN  0x40
#define SRS_COP  0x20
#define SRS_ILOP 0x10
#define SRS_ILAD 0x08
#define SRS_LOC  0x04
#define SRS_LVD  0x02

/* Service the COP watchdog (0x55, 0xAA to SRS; harmless if it is off) */
#define COP_SERVICE() do { SRS = 0x55; SRS = 0xAA; } while (0)

/* ---- Pull-up enables (high page) ---- */
#define PTCPE  REG8(0x1848)

/* ---- Flash controller (high page) ---- */
#define FCDIV  REG8(0x1820)        /* write-once after reset */
#define FOPT   REG8(0x1821)
#define FCNFG  REG8(0x1823)
#define FPROT  REG8(0x1824)
#define FSTAT  REG8(0x1825)
#define FCMD   REG8(0x1826)

#define FOPT_FNORED   0x40          /* 1 = no vector redirection */
#define FPROT_FPDIS   0x01          /* 1 = no flash protection */
#define FCDIV_FDIVLD  0x80
#define FCDIV_PRDIV8  0x40
#define FSTAT_FCBEF   0x80
#define FSTAT_FCCF    0x40
#define FSTAT_FPVIOL  0x20
#define FSTAT_FACCERR 0x10

#define FCMD_BYTE_PROG  0x20
#define FCMD_PAGE_ERASE 0x40

/* ---- USB RAM / buffer descriptor table ----
 * USB RAM: 0x1860..0x195F (256 bytes). BDT at 0x1860, 10 BDs x 3 bytes
 * + 2 reserved bytes = 32 bytes. Order (AN3560): EP0 IN, EP0 OUT, EP1..EP4,
 * EP5 even/odd, EP6 even/odd. Buffers must be 16-byte aligned.
 */
#define USB_RAM_BASE 0x1860

typedef struct {
    uint8_t stat;   /* OWN|DATA01|..|DTS|BDTSTALL (write) / PID in [5:2] (read) */
    uint8_t cnt;    /* byte count */
    uint8_t epadr;  /* (buffer - 0x1860) >> 2 */
} bd_t;

#define BD_OWN      0x80           /* 1 = SIE owns the BD */
#define BD_DATA1    0x40
#define BD_DTS      0x08           /* data toggle synchronisation enable */
#define BD_BDTSTALL 0x04
#define BD_PID(s)   ((uint8_t)(((s) >> 2) & 0x0F))

#define PID_OUT   0x1
#define PID_IN    0x9
#define PID_SETUP 0xD

#define BD_EP0_IN   0
#define BD_EP0_OUT  1
#define BD_EP(n)    (1 + (n))      /* single-buffered EP1..EP4 */

#define BDT ((volatile bd_t *)HW_PTR(USB_RAM_BASE))
#define USB_EPADR(addr) ((uint8_t)(((addr) - USB_RAM_BASE) >> 2))

/* Interrupt helpers */
#ifndef HOST_TEST
#define EnableInterrupts()  __asm__("cli")
#define DisableInterrupts() __asm__("sei")
#else
#define EnableInterrupts()
#define DisableInterrupts()
#endif

#endif
