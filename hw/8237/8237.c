/* 8237.c
 *
 * Intel 8237 DMA controller library.
 * (C) 2008-2012 Jonathan Campbell.
 * Hackipedia DOS library.
 *
 * This code is licensed under the LGPL.
 * <insert LGPL legal text here>
 *
 * Compiles for intended target environments:
 *   - MS-DOS [pure DOS mode, or Windows or OS/2 DOS Box]
 *
 * On IBM compatible hardware, the 8237 (or emulation thereof)
 * controls DMA transfers to/from system memory and devices on the
 * ISA bus. PCI devices are not involved, since under normal circumstances
 * PCI devices cannot use ISA DMA channels (though in the mid to late 1990s
 * it was somewhat common for PCI sound card drivers to use virtualization
 * or a special connection to the motherboard to emulate Sound Blaster audio).
 *
 * DMA channels are numbered 0 through 7. 0 through 3 carry forward from the
 * original PC architecture, while 4-7 were added with the 16-bit AT bus to
 * enable 16-bit wide DMA transfers. In most cases, 8-bit at a time transfers
 * are carried over DMA 0-3 and 16-bit at a time over 4-7. However on EISA/PCI
 * chipsets made in the 1994-1997 timeframe by Intel, there did exist control
 * registers to change the "width" of the DMA channel, including the ability
 * to transfer 32-bit at a time to EISA devices.
 *
 * DMA channel 4 is a "cascade" channel that is used internally by the two
 * controllers linked together, it is never used by the system for data
 * transfers.
 *
 * DMA controller addresses are limited to 24 bits (20 on really old hardware).
 * This is why on systems with >= 16MB of RAM it is not possible to DMA from
 * just any location in memory. In fact this limitation is the reason the x86
 * platform builds of the Linux kernel like to reserve lower "DMA" memory
 * (whatever it can reserve below 16MB). On some Intel chipsets used in the 1995-2000
 * timeframe however, there exist extended EISA registers that enable DMA from the
 * full 32-bit addressable range. These extended control registers lie in the 4xxh
 * range, however probing for them is nearly impossible because in most cases the
 * registers are write-only. The only way to detect it it seems, is to ask the
 * ISA PnP BIOS for the system DMA controller and see if the reported I/O range
 * includes I/O ports 400h-4FFh. We do not detect this case by default, because the
 * ISA PnP library is huge and we don't want to cause EXE bloat. Just like the Sound
 * Blaster library, the EISA/PCI support routines and detection code is in a separate
 * PnP-aware portion. */

/* TODO: PnP support code in 8237pnp.c, code that PnP aware programs can use to enable
 *       the extended code and extended functions. */

/* NTS: As of 2011/02/27 the 8254 routines no longer do cli/sti for us, we are expected
 *      to do them ourself. This is for performance reasons as well as for sanity reasons
 *      should we ever need to use the subroutines from within an interrupt handler */

#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <malloc.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/cpu/cpu.h>
#include <hw/dos/dos.h>
#include <hw/8237/8237.h>
#include <hw/dos/doswin.h>

/* default: no masking */
unsigned char d8237_16bit_pagemask = 0xFFU;
unsigned short d8237_16bit_addrmask = 0xFFFFU;

unsigned char d8237_flags = 0;
unsigned char d8237_channels = 0;
unsigned char d8237_dma_address_bits = 0;
unsigned char d8237_dma_counter_bits = 0;
uint32_t d8237_dma_address_mask = 0;
uint32_t d8237_dma_counter_mask = 0;
unsigned char *d8237_page_ioport_map = NULL;

#ifdef TARGET_PC98
unsigned char d8237_page_ioport_map_pc98[8] = {0x27,0x21,0x23,0x25, 0x27,0x21,0x23,0x25};
#else
unsigned char d8237_page_ioport_map_xt[8] = {0x87,0x83,0x81,0x82, 0x8F,0x8F,0x8F,0x8F}; /* TODO: how to detect PC/XT? */
unsigned char d8237_page_ioport_map_at[8] = {0x87,0x83,0x81,0x82, 0x8F,0x8B,0x89,0x8A};
#endif

#if TARGET_MSDOS == 16
static void _dos_freemem_wrap(void far *x) {
    if (x != NULL) _dos_freemem(FP_SEG(x));
}

static void far *_dos_allocmem_wrap(unsigned long sz) {
    unsigned segs,res,err;

    if (sz >= 0xBFFFFUL)
        return NULL;

    if (sz != 0)
        segs = (unsigned)((sz + 0xFUL) >> 4UL);
    else
        segs = 1;

    err = _dos_allocmem(segs,&res);
    if (err != 0)
        return NULL;

    return MK_FP(res,0);
}
#endif

/* one bit per channel that is 16-bit AND requires address shift (lower 16 bits >> 1) AND the counter is # of WORDs */
/* in the original implementation we take the lower 16 bits and shift right by 1 for WORD transfers,
 * the later Intel chipsets allow for 8, 16 & 32-bit transfers on any channel with or without
 * the 16-bit address shift, which is why this is a variable not a const */
unsigned char d8237_16bit_ashift = 0xF0;

static int d8237_readwrite_test4(unsigned int bch) {
    int j,i;

#ifdef TARGET_PC98
    if (bch & 4)
        return 0;

    for (j=8;j < 16;j++) {
        if (inp((j*2)+1) != 0xFF) break; /* found SOMETHING */
    }
#else
    if (bch & 4) {
        for (j=8;j < 16;j++) {
            if (inp(0xC0+(j*2)) != 0xFF) break; /* found SOMETHING */
        }
    }
    else {
        for (j=8;j < 16;j++) {
            if (inp(j) != 0xFF) break; /* found SOMETHING */
        }
    }
#endif

    if (j != 16) { /* OK. Now go through any DMA channel that still reads 0xFFFF 0xFFFF
              and try to modify the value. As for conflicts with active
              DMA channels? Well... if they're all reading 0xFFFF then none
              of them are active, so what's to worry about really? */
        for (i=0;i < 4;i++) {
            if (d8237_read_base_lo16(bch+i) == 0xFFFFU && d8237_read_count_lo16(bch+i) == 0xFFFFU) {
                d8237_write_count(bch+i,0x20); /* harmless short count */
                d8237_write_base(bch+i,0xBBBB0UL); /* direct it harmlessly at unused VGA RAM */
                if (d8237_read_base_lo16(bch+i) != 0xFFFFU || d8237_read_count_lo16(bch+i) != 0xFFFFU) {
                    d8237_write_count(bch+i,0xFFFF);
                    d8237_write_base(bch+i,0xFFFFFF);
                    return 1; /* found one */
                }
            }
            else {
                return 1; /* wait, found one! */
            }
        }
    }

    return 0;
}

/* probing function, for several reasons:
 *    - post-2011 systems may very well omit the DMA controller entirely, as legacy hardware
 *    - pre-AT hardware may not have the secondary DMA controller
 *    - EISA and PCI systems have a newer chipset with extended registers for addressing
 *      memory above the 16MB ISA barrier in transfer sizes up to 16MB (Intel 82357, Intel 82374, etc.) */
/* FIXME: Programming experience says that while most 1995-2000-ish motherboards do support these extended
 *        registers, reading the I/O ports will still yield 0xFF and therefore probing the I/O ports is
 *        not the way to detect them. You can however detect their presence if you use the ISA Plug & Play
 *        interface, because the DMA controller system node will list I/O ports in the 0x400-0x4FF range. */
/* FIXME: Just like the other libraries, add a 8237_pnp.lib file that ISA PnP aware programs can compile in
 *        to detect by PnP and gain access to the extended registers. */ 
int probe_8237() {
    int i;

    probe_dos();
    cpu_probe();
    detect_windows();
#if TARGET_MSDOS == 32
    dos_ltp_probe(); /* needed for DMA transfer code */
#endif

    d8237_flags = 0;
    /* test primary DMA controller */
    for (i=0;i < 4;) {
        if (d8237_read_base_lo16(i) != 0xFFFFU) break;
        if (d8237_read_count_lo16(i) != 0xFFFFU) break;
        i++;
    }

    /* BUGFIX: Apparently, on an old Sharp laptop, it's quite possible after cold boot
     *         for ALL 4 DMA CHANNELS to register as having base == 0xFFFF and count == 0xFFFF
     *         which falsely leads our code to believe there's no DMA controller! */
    if (i == 4 && d8237_readwrite_test4(0)) i = 0;
    if (i == 4) return 0; /* if not found, then quit */
    d8237_flags |= D8237_DMA_PRIMARY;

    /* are the page registers 8 bits wide, or 4? */
    {
        /* test DMA channel 2's page register, since it's unlikely the floppy controller
         * will be doing anything at this point */
#ifdef TARGET_PC98
        /* There's no way to test, because page registers are write-only *sigh* so just guess by the CPU in the system instead. */
        if (cpu_basic_level >= 2/*286 or higher*/)
            d8237_flags |= D8237_DMA_8BIT_PAGE;
#else
        unsigned char iop = d8237_page_ioport_map_xt[2],orig;
        orig = inp(iop);
        outp(iop,0xFE);
        if (inp(iop) == 0xFE) {
            outp(iop,0x0E);
            if (inp(iop) == 0x0E) {
                d8237_flags |= D8237_DMA_8BIT_PAGE;
            }
        }
        outp(iop,orig);
#endif
    }

#ifdef TARGET_PC98
    d8237_page_ioport_map = d8237_page_ioport_map_pc98;
    d8237_channels = 4;
#else
    /* test secondary DMA controller */
    for (i=4;i < 8;) {
        if (d8237_read_base_lo16(i) != 0xFFFFU) break;
        if (d8237_read_count_lo16(i) != 0xFFFFU) break;
        i++;
    }
    if (i == 8 && d8237_readwrite_test4(4)) i = 4;
    if (i != 8) d8237_flags |= D8237_DMA_SECONDARY; /* if found, then say so */

    if (d8237_flags & D8237_DMA_SECONDARY) {
        d8237_page_ioport_map = d8237_page_ioport_map_at;
        d8237_channels = 8;
    }
    else {
        d8237_page_ioport_map = d8237_page_ioport_map_xt;
        d8237_channels = 4;
    }
#endif

    if (d8237_flags & D8237_DMA_8BIT_PAGE)
        d8237_dma_address_bits = 24;
    else
        d8237_dma_address_bits = 20;

    d8237_dma_address_mask = (1UL << ((unsigned long)d8237_dma_address_bits)) - 1UL;
    d8237_dma_counter_mask = 0xFFFF;
    d8237_dma_counter_bits = 16;
    return 1;
}

void d8237_write_base(unsigned char ch,uint32_t o) {
    if (d8237_16bit_ashift & (1<<ch)) {
        d8237_write_base_lo16(ch,(o >> 1UL) & d8237_16bit_addrmask);
        outp(d8237_page_ioport_map[ch],(o >> 16UL) & d8237_16bit_pagemask);
    }
    else {
        d8237_write_base_lo16(ch,o);
        outp(d8237_page_ioport_map[ch],o >> 16UL);
    }
}

/* The "count" is BYTES - 1 not BYTES. On compatible AT controllers and newer controllers in
 * compat. mode DMA channels 4-7 transfer 16-bit WORDs at a time and the count is number
 * of WORDS - 1 */
uint32_t d8237_read_count(unsigned char ch) {
    uint32_t r = (uint32_t)d8237_read_count_lo16(ch) + (uint32_t)1UL;
    if (d8237_16bit_ashift & (1<<ch)) r <<= 1UL;
    return r;
}

void d8237_write_count(unsigned char ch,uint32_t o) {
    if (d8237_16bit_ashift & (1<<ch))   d8237_write_count_lo16(ch,(o>>1) - (uint32_t)1);
    else                    d8237_write_count_lo16(ch,o - (uint32_t)1);
}

void dma_8237_free_buffer(struct dma_8237_allocation *a) {
    if (a != NULL) {
        if (a->lin != NULL) {
#if TARGET_MSDOS == 32
            if (a->dos_selector != 0) {
                dpmi_free_dos(a->dos_selector);
                a->dos_selector = 0U;
                a->lin = NULL;
            }
            if (a->lin_handle != 0) {
                dpmi_linear_unlock((uint32_t)a->lin,a->length);
                dpmi_linear_free(a->lin_handle);
                a->lin_handle = 0U;
                a->lin = NULL;
            }
            if (a->lin != NULL) {
                dpmi_linear_unlock((uint32_t)a->lin,a->length);
                free(a->lin);
                a->lin = NULL;
            }
#else
            _dos_freemem_wrap(a->lin);
#endif
        }
        a->lin = NULL;
        a->phys = 0UL;
        a->length = 0UL;
        free(a);
    }
}

struct dma_8237_allocation *dma_8237_alloc_buffer(uint32_t len) {
    return dma_8237_alloc_buffer_dw(len,0);
}

struct dma_8237_allocation *dma_8237_alloc_buffer_dw(uint32_t len,uint8_t dma_width) {
    struct dma_8237_allocation *a = malloc(sizeof(struct dma_8237_allocation));
    const unsigned int leeway = 4096;
    uint32_t limit_mask;

    /* ^ I will remove this leeway value when DOSBox 0.74 no longer has a fucking panic attack
     *   over the DMA pointer getting too close to the 64KB boundary edge [FIXME: I believe I
     *   did something else to this code to further resolve that, do we still need this leeway value?] */
    memset(a,0,sizeof(*a));

    /* "dma_width" must be 0, 8, or 16 */
    if ((dma_width & 7) != 0 || dma_width > 16)
        goto fail;
    /* if "dma_width" == 0 then assume 8-bit */
    if (dma_width == 0)
        dma_width = 8;

    if (dma_width == 16 && d8237_can_do_16bit_128k())
        limit_mask = d8237_16bit_limit_mask();
    else
        limit_mask = d8237_8bit_limit_mask();

    if (len >= (limit_mask - 0xFUL))
        goto fail;

    if (a != NULL) {
#if TARGET_MSDOS == 32
        /* first try high memory, but only if paging is disabled or the DOS library is able to translate linear to physical */
        if (!dos_ltp_info.paging) {
            uint32_t handles[16],handle=0;
            uint32_t patience=16,o;
            uint64_t phys,base;
            int ok=0;

            while (!ok && patience-- != 0UL) {
                /* NOTE: If this actually works I wan't to know! */
                a->length = len;
                a->lin = dpmi_linear_alloc(0/*whatever linear addr works*/,len,1/*committed pages*/,&a->lin_handle);
                if (a->lin != NULL) {
                    /* wait: make sure we can locate it's physical location and that it's contigious in physical memory */
                    dpmi_linear_lock((uint32_t)(a->lin),a->length);
                    base = dos_linear_to_phys((uint32_t)(a->lin));
                    if (base != DOS_LTP_FAILED && base <= (uint64_t)d8237_dma_address_mask) {
                        /* make sure it's contiguous in memory */
                        for (o=4096UL;o < a->length;o += 4096UL) {
                            phys = dos_linear_to_phys((uint32_t)(a->lin) + o);
                            if (phys == DOS_LTP_FAILED) break;
                            if (phys != (base+o)) break;
                            if (phys > (uint64_t)d8237_dma_address_mask) break;
                            /* if we crossed a DMA boundary, then abort */
                            if (((phys+leeway)&(~limit_mask)) != (base&(~limit_mask))) break;
                        }

                        if (o >= a->length) {
                            a->phys = base;
                            ok = 1;
                            break;
                        }
                    }

                    /* not worthy, unlock and free later */
                    dpmi_linear_unlock((uint32_t)(a->lin),a->length);
                    handles[handle++] = a->lin_handle;
                    a->lin_handle = 0UL;
                    a->lin = NULL;
                }
                else {
                    a->lin_handle = 0UL;
                    break;
                }
            }

            while (handle > 0)
                dpmi_linear_free(handles[--handle]);
        }

        /* Yeah, that DPMI 1.x call hardly works. But we can malloc() and see if it ends up as DMA-able.
           Note that this won't work with any paging-like system like EMM386.EXE or Windows
           DOS Box, but we can work around that by allocating the buffer in DOS where the
           programs remap and translate DMA */
        if (!dos_ltp_info.paging) {
            void *handles[16];
            uint32_t handle=0;
            uint32_t base,patience=16;
            int ok=0;

            while (!ok && patience-- != 0UL) {
                a->length = len;
                a->lin = malloc(len);
                if (a->lin != NULL) {
                    base = (uint32_t)(a->lin);
                    if ((base&(~limit_mask)) == ((base+len-1)&(~limit_mask))) {
                        a->phys = base; /* OK */
                    }
                    else {
                        /* not worthy, unlock and free later */
                        handles[handle++] = a->lin;
                        a->lin = NULL;
                    }
                }
                else {
                    break;
                }
            }

            while (handle > 0)
                free(handles[--handle]);
        }

        /* if that failed, try DOS memory, but only if it's mapped 1:1 or we're assured DMA is translated along with the remapping */
        if (a->lin == NULL && (!dos_ltp_info.dos_remap || dos_ltp_info.dma_dos_xlate)) {
            uint16_t handles[16],handle=0;
            uint32_t patience=16;
            int ok=0;

            a->lin_handle = 0UL;
            while (!ok && patience-- != 0UL) {
                a->length = len;
                a->lin = dpmi_alloc_dos(a->length,&a->dos_selector);
                if (a->lin != NULL) {
                    a->phys = (uint32_t)(a->lin);
                    /* if it crosses a DMA boundary then fail */
                    if ((a->phys&(~limit_mask)) != ((a->phys+a->length+leeway)&(~limit_mask))) {
                        ok = 0;
                    }
                    else {
                        ok = 1;
                    }
                }

                if (ok) break;
                handles[handle++] = a->dos_selector;
                a->dos_selector = 0U;
                a->lin = NULL;
            }

            while (handle > 0)
                dpmi_free_dos(handles[--handle]);
        }

        if (a->lin == NULL) goto fail;
#else
        a->lin = _dos_allocmem_wrap(len);
        if (a->lin == NULL) goto fail;
        a->phys = ((uint32_t)FP_SEG(a->lin) << 4UL) + FP_OFF(a->lin);
        a->length = len;

        /* if it crosses a DMA 64KB boundary, then we have to try again */
        if ((a->phys & (~limit_mask)) != ((a->phys + len + leeway) & (~limit_mask))) {
            unsigned char FAR *s2 = _dos_allocmem_wrap(len);
            if (s2 == NULL) goto fail;
            _dos_freemem_wrap(a->lin);
            a->lin = s2;
            a->phys = ((uint32_t)FP_SEG(a->lin) << 4UL) + FP_OFF(a->lin);
            /* if it crosses again, then obviously memory is too fragmented */
            if ((a->phys & (~limit_mask)) != ((a->phys + len + leeway) & (~limit_mask)))
                goto fail;
        }
#endif
    }

    a->dma_width = dma_width;
    return a;
fail:
    if (a != NULL) {
#if TARGET_MSDOS == 32
        if (a->lin_handle) {
            dpmi_linear_free(a->lin_handle);
            a->lin_handle = 0UL;
            a->lin = NULL;
        }
        if (a->dos_selector) {
            dpmi_free_dos(a->dos_selector);
            a->dos_selector = 0U;
            a->lin = NULL;
        }
        if (a->lin != NULL) {
            free(a->lin);
            a->lin = NULL;
        }
#else
        if (a->lin) _dos_freemem_wrap(a->lin);
        a->lin = NULL;
#endif
        free(a);
    }

    return NULL;
}

uint16_t d8237_read_count_lo16(unsigned char ch) {
    unsigned int flags = get_cpu_flags();
    uint16_t r,r2,patience=32;

    /* The DMA controller as emulated by DOSBox and on real hardware does not guarantee latching the count while reading.
     * it can change between reading the upper and lower bytes. so we need to do this loop to read a coherent value. */
    _cli();
    d8237_reset_flipflop(ch);
    r2 = d8237_read_count_lo16_direct_ncli(ch);
    do {
        r   = r2;
        r2 = d8237_read_count_lo16_direct_ncli(ch);
        /* the counter must be decreasing! */
        if ((r&0xFF00) != (r2&0xFF00)) continue;
        if (r == 0xFFFF) break; /* if at terminal count, then break now */
        if (r2 > r) continue;
        break;
    } while (--patience != 0U);
    _sti_if_flags(flags);

    return r2;
}

uint32_t d8237_16bit_limit_mask() {
    if ((d8237_flags & D8237_DMA_16BIT_CANDO_128K) &&   // DMA controller is said to support 128K
            d8237_16bit_pagemask == 0xFEU &&                // bit 0 of page register ignored
            d8237_16bit_addrmask == 0xFFFFU)                // bit 15 of addr register used
        return 0x1FFFFUL;                               // therefore, DMA range is limited to bits 0-16

    return 0xFFFFUL;
}

/* vim: set tabstop=4 softtabstop=4 shiftwidth=4 expandtab */

