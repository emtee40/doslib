/* FIXME: This code is failing to detect that it's running under the latest (4.3) VirtualBox. Why? */

/* TODO: Add a flag to the sound blaster context to track whether or not DSP playback/record is active. */

/* TODO: How to add support for full duplex audio? ESS 688 and 1869 chipsets for example can do full
 *       duplex but only at the same sample rate. I'm not sure if Sound Blaster 16 ViBRA cards can
 *       do full duplex. */

/* FIXME: This just in: your IRQ testing routine can hang the system especially (as in DOSBox) when IRQs that are
 *        normally masked have a corresponding interrupt vector set to NULL (0000:0000). You need to double-check
 *        the probe function and make sure there is no possible way uninitialized vectors execute when any
 *        particular IRQ is fired. */

/* FIXME: On a laptop with ESS 688 playing 16-bit PCM with auto-init DSP and auto-init DMA, the IRQ does not
 *        fire periodically as it should. Any other mode (auto-init DMA with single-cycle DMA, or any of the
 *        8-bit modes) fires the IRQ periodically as in auto-init. This is important for anyone who might try
 *        to use this library to play music entirely from the interrupt. */

/* FIXME: There might be a way to deal with some troubles you've been having with this code:
 *
 *        Troubles:
 *          - Gravis SBOS/MEGA-EM NMI causes DOS4GW.EXE to hard-reset the machine rather than deal with it
 *          - Even when your code caught the NMI and forwarded it to Gravis's TSRs, Sound Blaster emulation
 *            failed to signal your interrupt handler, which is probably why 16-bit real mode builds worked
 *            fine with SBOS/MEGA-EM while 32-bit builds did not.
 *          - Under DOS4GW.EXE (but not DOS32A.EXE) using IRQ 8-15 eventually resulted in missing an IRQ.
 *            I initially panned it as DOS4GW.EXE being a cheap son-of-a-bitch but it might be related instead
 *            to a combination of the BIOS forwarding interrupts in real mode, and the lack of a real-mode
 *            interrupt handler for the Sound Blaster library.
 *
 *        Things to do:
 *          - Add code to set the interrupt vector for the IRQ on behalf of the calling program. The calling
 *            program provides a function to call, and you set the vector (and keep track of the previous one).
 *          - Also add function to remove interrupt vector. Write the code so that if someone else hooked,
 *            the unhook function returns an error.
 *          - Add code to interrupt vector hooking code so that in 32-bit builds, if MEGA-EM or SBOS is resident
 *            or DOS4GW.EXE is resident and IRQ 8-15 is to be used, this code automatically installs a real-mode
 *            interrupt vector that reflects the interrupt into protected mode.
 *          - Add code so that, in 32-bit builds, if MEGA-EM or SBOS is resident, the library hooks the NMI
 *            interrupt in protected mode and reflects it to real-mode so Gravis's emulation can work. [FIXME
 *            can you also resolve DOS4GW.EXE hard-crashing in this situation?] */

/* Primary Sound Blaster support library.
 * This compiles into sndsb.lib.
 * This DOES NOT include support for Plug & Play versions,
 * functions for that are in sndsbpnp.c -> sndsbpnp.lib */

#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <assert.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/dos/dos.h>
#include <hw/dos/dosbox.h>
#include <hw/8237/8237.h>		/* 8237 DMA */
#include <hw/8254/8254.h>		/* 8254 timer */
#include <hw/8259/8259.h>		/* 8259 PIC interrupts */
#include <hw/sndsb/sndsb.h>
#include <hw/dos/doswin.h>
#include <hw/dos/tgusmega.h>
#include <hw/dos/tgussbos.h>

/* Windows 9x VxD enumeration */
#include <windows/w9xvmm/vxd_enum.h>

/* uncomment this to enable debugging messages */
//#define DBG

#if defined(DBG)
# define DEBUG(x) (x)
#else
# define DEBUG(x)
#endif

void (*sndsb_detect_windows_dosbox_vm_quirks_CB)(struct sndsb_ctx *cx) = NULL;
void (*sndsb_read_sb16_irqdma_resources_CB)(struct sndsb_ctx *cx) = NULL;
void (*sndsb_ess_extensions_probe_CB)(struct sndsb_ctx *cx) = NULL;
int (*sndsb_read_sc400_config_CB)(struct sndsb_ctx *cx) = NULL;

unsigned char sndsb_virtualbox_emulation = 0;
struct sndsb_probe_opts sndsb_probe_options={0};
struct sndsb_ctx sndsb_card[SNDSB_MAX_CARDS];
struct sndsb_ctx *sndsb_card_blaster=NULL;
int sndsb_card_next = 0;

void sndsb_timer_tick_gen(struct sndsb_ctx *cx) {
	cx->timer_tick_signal = 1;
}

void free_sndsb() {
#if TARGET_MSDOS == 32
	if (sndsb_nmi_32_hook) do_nmi_32_unhook();
#endif
}

int init_sndsb() {
	int i;

	memset(sndsb_card,0,sizeof(sndsb_card));
	for (i=0;i < SNDSB_MAX_CARDS;i++)
		sndsb_card[i].pnp_bios_node = 0xFF;

	sndsb_card_blaster = NULL;
	sndsb_card_next = 0;
#if TARGET_MSDOS == 32
	sndsb_nmi_32_hook = sb_nmi_32_auto_choose_hook();
	if (sndsb_nmi_32_hook) do_nmi_32_hook();
#endif
	return 1;
}

struct sndsb_ctx *sndsb_alloc_card() {
	int i;

	for (i=0;i < SNDSB_MAX_CARDS;i++) {
		if (sndsb_card[i].baseio == 0)
			return sndsb_card+i;
	}

	return NULL;
}

void sndsb_free_card(struct sndsb_ctx *c) {
#if TARGET_MSDOS == 32
	if (c->goldplay_dma) {
		dma_8237_free_buffer(c->goldplay_dma);
		c->goldplay_dma = NULL;
	}
#endif
	memset(c,0,sizeof(*c));
	c->pnp_bios_node = 0xFF;
	if (c == sndsb_card_blaster) sndsb_card_blaster = NULL;
}

void sndsb_validate_dma_against_8237(struct sndsb_ctx *cx) {
	/* check DMA against the DMA controller presence.
	 * If there is no 16-bit DMA (channels 4-7) then we cannot use
	 * those channels */
	if (!(d8237_flags&D8237_DMA_SECONDARY)) {
		if (cx->dma16 >= 4) cx->dma16 = -1;
		if (cx->dma8 >= 4) cx->dma8 = -1;
	}
	if (!(d8237_flags&D8237_DMA_PRIMARY)) {
		if (cx->dma16 >= 0 && cx->dma16 < 4) cx->dma16 = -1;
		if (cx->dma8 >= 0 && cx->dma8 < 4) cx->dma8 = -1;
	}
}

void sndsb_update_capabilities(struct sndsb_ctx *cx) {
	if (cx->dsp_vmaj >= 4) {
		/* Highspeed DSP commands don't matter anymore, they're just an alias to older commands */
		cx->hispeed_matters = 0;
		cx->hispeed_blocking = 0;
		/* The DSP is responsive even during hispeed mode, you can nag it then just fine */
		cx->dsp_nag_hispeed = 1;
		/* FIXME: At exactly what DSP version did SB16 allow going up to 48KHz?
		 * I'm going by the ViBRA test card I own having DSP 4.13 vs DOSBox sbtype=sb16
		 * reporting DSP v4.5 */
		if (cx->dsp_vmaj == 4 && cx->dsp_vmin > 5)
			cx->max_sample_rate_dsp4xx = 48000;
		else
			cx->max_sample_rate_dsp4xx = 44100;

		cx->enable_adpcm_autoinit = 1; /* NTS: Unless there are DSP 4.xx SB clones out there that don't, we can assume auto-init ADPCM */
		cx->max_sample_rate_sb_hispeed_rec = cx->max_sample_rate_dsp4xx;
		cx->max_sample_rate_sb_hispeed = cx->max_sample_rate_dsp4xx;
		cx->max_sample_rate_sb_play = cx->max_sample_rate_dsp4xx;
		cx->max_sample_rate_sb_rec = cx->max_sample_rate_dsp4xx;
		if (cx->max_sample_rate_dsp4xx > 44100) { /* SB16 ViBRA cards apparently allow Direct DAC output up to 24KHz instead of 23KHz */
			cx->max_sample_rate_sb_play_dac = 24000;
			/* TODO: Is recording speed affected? */
		}
	}
	else if (cx->dsp_vmaj == 3) {
		if (cx->ess_chipset != 0) { /* ESS 688/1869 */
			/* NTS: The ESS 688 (Sharp laptop) and ESS 1869 (Compaq desktop) I test against seems quite capable
			 *      of playing back at 48KHz, in fact it will happily go beyond 48KHz up to 64KHz in my tests
			 *      barring ISA bus limitations (16-bit stereo at 54KHz audibly "warbles" for example). For
			 *      for consistentcy's sake, we'll just go ahead and say the chip goes up to 48KHz */
			cx->dsp_direct_dac_poll_retry_timeout = 4; /* DSP is responsive to direct DAC to allow lesser timeout */
			cx->max_sample_rate_dsp4xx = 48000;
			cx->max_sample_rate_sb_hispeed_rec = 48000;
			cx->max_sample_rate_sb_hispeed = 48000;
			cx->max_sample_rate_sb_play = 48000;
			cx->max_sample_rate_sb_rec = 48000;
			cx->enable_adpcm_autoinit = 0; /* does NOT support auto-init ADPCM */
			/* also: hi-speed DSP is blocking, and it matters: to go above 23KHz you have to use hi-speed DSP commands */
		}
		else if (cx->is_gallant_sc6600) { /* SC-6600 clone card */
			cx->dsp_direct_dac_poll_retry_timeout = 4; /* DSP is responsive to direct DAC to allow lesser timeout */
			/* NTS: Officially, the max sample rate is 24000Hz, but the DSP seems to allow up to 25000Hz,
			 *      then limit the sample rate to that up until about 35000Hz where it suddenly clamps
			 *      the rate down to 24000Hz. Mildly strange bug. */
			cx->max_sample_rate_dsp4xx = 44100;
			cx->max_sample_rate_sb_hispeed_rec = 44100; /* playback and recording rate (it's halved to 22050Hz for stereo) */
			cx->max_sample_rate_sb_hispeed = 44100; /* playback and recording rate (it's halved to 22050Hz for stereo) */
			cx->max_sample_rate_sb_play = 25000; /* non-hispeed mode (and it's halved to 11500Hz for stereo) */
			cx->max_sample_rate_sb_rec = 25000; /* non-hispeed mode (and it's halved to 11500Hz for stereo) */
			cx->enable_adpcm_autoinit = 0; /* does NOT support auto-init ADPCM */
			/* also: hi-speed DSP is blocking, and it matters: to go above 23KHz you have to use hi-speed DSP commands */
		}
		else { /* Sound Blaster Pro */
			cx->max_sample_rate_dsp4xx = 0;
			cx->max_sample_rate_sb_hispeed_rec = 44100; /* playback and recording rate (it's halved to 22050Hz for stereo) */
			cx->max_sample_rate_sb_hispeed = 44100; /* playback and recording rate (it's halved to 22050Hz for stereo) */
			cx->max_sample_rate_sb_play = 23000; /* non-hispeed mode (and it's halved to 11500Hz for stereo) */
			cx->max_sample_rate_sb_rec = 23000; /* non-hispeed mode (and it's halved to 11500Hz for stereo) */
		}
	}
	else if (cx->dsp_vmaj == 2) {
		if (cx->dsp_vmin >= 1) { /* Sound Blaster 2.01 */
			cx->max_sample_rate_dsp4xx = 0;
			cx->max_sample_rate_sb_hispeed_rec = 15000;
			cx->max_sample_rate_sb_rec = 13000;
			cx->max_sample_rate_sb_hispeed = 44100; /* NTS: On actual SB 2.1 hardware I own you can apparently go up to 46KHz? */
			cx->max_sample_rate_sb_play = 23000;
		}
		else { /* Sound Blaster 2.0, without hispeed DSP commands */
			cx->max_sample_rate_dsp4xx = 0;
			cx->max_sample_rate_sb_hispeed_rec = cx->max_sample_rate_sb_rec = 13000;
			cx->max_sample_rate_sb_hispeed = cx->max_sample_rate_sb_play = 23000;
		}
	}
	else { /* Sound Blaster 1.x */
		cx->max_sample_rate_dsp4xx = 0;
		cx->max_sample_rate_sb_hispeed_rec = cx->max_sample_rate_sb_rec = 13000;
		cx->max_sample_rate_sb_hispeed = cx->max_sample_rate_sb_play = 23000;
	}

	/* DSP 2xx and earlier do not have auto-init commands */
	if (cx->dsp_vmaj < 2 || (cx->dsp_vmaj == 2 && cx->dsp_vmin == 0))
		cx->dsp_autoinit_command = 0;
	if (cx->irq < 0) {
		if (cx->dsp_autoinit_command)
			cx->dsp_nag_mode = 0;
		else
			cx->dsp_nag_mode = 1;
	}
}

/* NTS: This routine NO LONGER probes the mixer */
/* NTS: We do not test IRQ and DMA channels here */
/* NTS: The caller may have set irq == -1, dma8 == -1, or dma16 == -1, such as
 *      when probing. If any of them are -1, and this code knows how to deduce
 *      it directly from the hardware, then they will be updated */
int sndsb_init_card(struct sndsb_ctx *cx) {
	unsigned int i;

	/* some of our detection relies on knowing what OS we're running under */
	cpu_probe();
	probe_dos();
	detect_windows();

#if TARGET_MSDOS == 32
	cx->goldplay_dma = NULL;
#endif
	cx->backwards = 0;
	cx->ess_chipset = 0;
	cx->dsp_nag_mode = 0;
	cx->ess_extensions = 0;
	cx->dsp_nag_hispeed = 0;
	cx->ess_extended_mode = 0;
	cx->hispeed_matters = 1; /* assume it does */
	cx->hispeed_blocking = 1; /* assume it does */
	cx->timer_tick_signal = 0;
	cx->timer_tick_func = NULL;
	cx->poll_ack_when_no_irq = 1;
	cx->reason_not_supported = NULL;
	cx->virtualbox_emulation = sndsb_virtualbox_emulation;
	cx->dsp_alias_port = sndsb_probe_options.use_dsp_alias;
	cx->dsp_direct_dac_poll_retry_timeout = 16; /* assume at least 16 I/O reads to wait for DSP ready */
	cx->dsp_direct_dac_read_after_command = 0;
	cx->windows_creative_sb16_drivers_ver = 0;
	cx->windows_creative_sb16_drivers = 0;
	cx->dsp_4xx_fifo_single_cycle = 0;
	cx->windows_9x_me_sbemul_sys = 0;
	cx->audio_data_flipped_sign = 0;
	cx->dsp_4xx_fifo_autoinit = 1;
	cx->dsp_autoinit_command = 1;
	cx->buffer_irq_interval = 0;
	cx->windows_springwait = 0;
	cx->chose_autoinit_dma = 0;
	cx->chose_autoinit_dsp = 0;
	cx->do_not_probe_irq = 0;
	cx->do_not_probe_dma = 0;
	cx->is_gallant_sc6600 = 0;
	cx->windows_emulation = 0;
	cx->windows_xp_ntvdm = 0;
	cx->dsp_copyright[0] = 0;
	cx->dsp_autoinit_dma = 1;
	cx->buffer_last_io = 0;
	cx->direct_dsp_io = 0;
	cx->goldplay_mode = 0;
	cx->force_hispeed = 0;
	cx->chose_use_dma = 0;
	cx->vdmsound = 0;
	cx->mega_em = 0;
	cx->sbos = 0;
	cx->dsp_ok = 0;
	cx->mixer_ok = 0;
	cx->mixer_probed = 0;
	cx->dsp_vmaj = 0;
	cx->dsp_vmin = 0;
	cx->buffer_phys = 0;
	cx->buffer_lin = NULL;
	cx->buffer_rate = 22050;
	cx->enable_adpcm_autoinit = 0;
	cx->dsp_adpcm = 0;
	cx->dsp_record = 0;
	cx->max_sample_rate_sb_play_dac = 23000;
	cx->max_sample_rate_sb_rec_dac = 13000;

	if (!sndsb_reset_dsp(cx)) return 0;
	if (!sndsb_query_dsp_version(cx)) return 0; // FIXME: Do all Sound Blaster cards support this? Are there any shitty SB clones that don't?

	/* Gravis Ultrasound SBOS, when unloaded, might leave the read data port at 0xAA which
	 * we then read back as DSP version 0xAA 0xAA. */
	if (cx->dsp_vmaj == 0xAA && cx->dsp_vmin == 0xAA) return 0;

	/* OK */
	cx->dsp_ok = 1;

	/* It seems to me the safest way to know whether or not to read the
	 * copyright string is to assume anything before DSP version 3.xx
	 * does not. Sound Blaster 2.0 and earlier don't have one, and
	 * neither does Sound Blaster Pro (DSP 3.xx). The other reason is
	 * that some emulation code (Gravis Ultrasound SBOS/MEGA-EM) emulate
	 * DSP 2.xx or 1.xx and they do not handle the copyright string command
	 * well at all, in fact, MEGA-EM will halt the system if you try. */
	if (cx->dsp_vmaj >= 3)
		sndsb_read_dsp_copyright(cx,cx->dsp_copyright,sizeof(cx->dsp_copyright));
	else if (cx->dsp_vmaj == 1 || cx->dsp_vmaj == 2) {
		if (gravis_mega_em_detect(&megaem_info)) { // MEGA-EM emulates DSP 1.xx. It can emulate DSP 2.01, but badly
			cx->mega_em = 1;
			cx->dsp_autoinit_dma = 0;
		}
		else if (gravis_sbos_detect() >= 0) { // SBOS only emulates DSP 1.xx
			cx->sbos = 1;
			cx->dsp_autoinit_dma = 0;
		}
	}

	/* Sound Blaster 16 (DSP 4.xx): we read the mixer registers, unless this card was initialized from a PnP device */
	/* Earlier cards: we have to probe around for it */
	if (sndsb_read_sb16_irqdma_resources_CB != NULL && cx->dsp_vmaj == 4 && !sndsb_probe_options.disable_sb16_read_config_byte && cx->pnp_id == 0)
		sndsb_read_sb16_irqdma_resources_CB(cx);

	/* Reveal SC400 SB16 clone: I have this card and I can tell
	 * from programming experience that while it reports itself
	 * as DSP v3.5 (Sound Blaster Pro) it actually has a SB16
	 * type mixer and supports most (but not all) of the SB16
	 * type DSP commands. It lacks however the configuration
	 * registers in the mixer for DMA and IRQ settings, on
	 * this card you use secret undocumented DSP commands.
	 * The card also has a "Windows Sound System" interface
	 * at 0x530, which is not relevent here since we focus on
	 * the Sound Blaster part.
	 *
	 * It also has a amusing hardware bug where I can set the
	 * DSP up as if doing a DMA transfer, and then my code can
	 * fake the DMA transfer by writing to the DSP command
	 * port, something I took advantage of prior to figuring out
	 * the DMA controller back in the day */
	if (sndsb_read_sc400_config_CB != NULL && !strcmp(cx->dsp_copyright,"SC-6000"))
		sndsb_read_sc400_config_CB(cx);

	if (sndsb_detect_windows_dosbox_vm_quirks_CB != NULL && windows_mode != WINDOWS_NONE)
		sndsb_detect_windows_dosbox_vm_quirks_CB(cx);

	/* DSP v3.1 and no copyright string means it might be an ESS 688/1869 chipset */
	/* FIXME: A freak accident during development shows me it's possible to change the DSP version to v2.1 */
	if (sndsb_ess_extensions_probe_CB != NULL && !cx->windows_emulation && !cx->is_gallant_sc6600 && cx->dsp_vmaj == 3 && cx->dsp_vmin == 1 &&
		cx->dsp_copyright[0] == 0 && !sndsb_probe_options.disable_ess_extensions)
		sndsb_ess_extensions_probe_CB(cx);

	/* If the context refers to DMA channels that don't exist on the system, then mark them off appropriately */
	sndsb_validate_dma_against_8237(cx);

	/* Using what we know of the card, update the capabilities in the context */
	sndsb_update_capabilities(cx);

	/* make sure IRQs are cleared */
	for (i=0;i < 3;i++) sndsb_interrupt_ack(cx,3);

	/* auto-determine the best way to play audio */
	sndsb_determine_ideal_dsp_play_method(cx);
	return 1;
}

int sndsb_determine_ideal_dsp_play_method(struct sndsb_ctx *cx) {
	if (cx->dma8 < 0) /* No IRQ, no DMA, fallback to direct */
		cx->dsp_play_method = SNDSB_DSPOUTMETHOD_DIRECT;
	else if (cx->dsp_vmaj >= 4 || cx->is_gallant_sc6600)
		cx->dsp_play_method = SNDSB_DSPOUTMETHOD_4xx;
	else if (cx->dsp_vmaj == 3)
		cx->dsp_play_method = SNDSB_DSPOUTMETHOD_3xx;
	else if (cx->dsp_vmaj == 2 && cx->dsp_vmin >= 1) {
		/* Gravis SBOS does not do auto-init at all.
		   Gravis MEGA-EM will fucking hang the computer and gripe
		   about "unknown DSP command 1Ch" despite reporting itself
		   as DSP v2.1 (EMUSET -X2). So don't do it! */
		if (cx->sbos || cx->mega_em)
			cx->dsp_play_method = SNDSB_DSPOUTMETHOD_1xx;
		else
			cx->dsp_play_method = SNDSB_DSPOUTMETHOD_201;
	}
	else if (cx->dsp_vmaj == 2)
		cx->dsp_play_method = SNDSB_DSPOUTMETHOD_200;
	else if (cx->dsp_vmaj == 1)
		cx->dsp_play_method = SNDSB_DSPOUTMETHOD_1xx;
	else
		cx->dsp_play_method = SNDSB_DSPOUTMETHOD_DIRECT;

	return 1;
}

#if TARGET_MSDOS == 16 && (defined(__COMPACT__) || defined(__SMALL__))
#else
static unsigned char sb_test_irq_number = 0;
static volatile unsigned short int sb_test_irq_flag = 0;
static void interrupt far sb_test_irq() {
	sb_test_irq_flag++;
	if (sb_test_irq_number >= 8) p8259_OCW2(8,P8259_OCW2_NON_SPECIFIC_EOI);
	p8259_OCW2(0,P8259_OCW2_NON_SPECIFIC_EOI);
}
#endif

/* alternative "lite" IRQ probing that hooks the interrupt and wait for an event.
 * Microsoft Windows friendly version that avoids 1) PIC commands to read back
 * events and 2) The undocumented DSP command 0xF2 that triggers an interrupt.
 *
 * While the primary method in manual_probe_irq() works well in pure DOS and
 * some DOS boxes, this lite version works better in virtualized environments
 * like Windows NT/9x DOS boxes. */
void sndsb_alt_lite_probe_irq(struct sndsb_ctx *cx) {
#if TARGET_MSDOS == 16 && (defined(__COMPACT__) || defined(__SMALL__))
	/* too much code */
#else
	void (interrupt *old_irq)() = NULL;
	unsigned int round = 0,tolerance;
	unsigned char ml,mh,maybe;
	unsigned int patience = 0;
	unsigned short eliminated = 0U,possible;
	unsigned char tries[] = {5,7},tri;
	unsigned int testlen = 22050/20; /* 1/20th of a second */
	struct dma_8237_allocation *dma;
	const unsigned char timeconst = (unsigned char)((65536UL - (256000000UL / 22050UL)) >> 8UL);
	DEBUG(fprintf(stdout,"Sound blaster IRQ unknown, I'm going to have to probe for it [alt lite]\n"));

	/* for this test we initiate playback of short blocks. so we must ensure that this
	 * card has a known DMA channel assignment. */
	if (cx->dma8 < 0) return;

	dma = dma_8237_alloc_buffer(testlen);
	if (dma == NULL) return;

#if TARGET_MSDOS == 32
	memset(dma->lin,128,testlen);
#else
	_fmemset(dma->lin,128,testlen);
#endif

	/* save the IRQ mask */
	_cli();
	ml = p8259_read_mask(0);	/* IRQ0-7 */
	mh = p8259_read_mask(8);	/* IRQ8-15 */
	
	round = 0;
	do {
		if (++round >= 8)
			break;

		possible = 0;
		/* go through the remaining ones, one at a time */
		for (tri=0;tri < sizeof(tries);tri++) {
			if (eliminated & (1U << tries[tri]))
				continue;
			if (!sndsb_reset_dsp(cx)) {
				DEBUG(fprintf(stdout,"WARNING: DSP reset failed, aborting IRQ probe\n"));
				break;
			}

			DEBUG(fprintf(stdout,"  Now testing IRQ %u\n",tries[tri]));

			/* clear SoundBlaster's previous interrupt */
			inp(cx->baseio+SNDSB_BIO_DSP_READ_STATUS);

			p8259_mask(tries[tri]);
			/* hook the interrupt, reset the flag, unmask the interrupt */
			sb_test_irq_flag = 0;
			sb_test_irq_number = tries[tri];
			old_irq = _dos_getvect(irq2int(tries[tri]));
			_dos_setvect(irq2int(tries[tri]),sb_test_irq);
			p8259_unmask(tries[tri]);

			/* wait for IRQ to show response (prior to triggering one) */
			_sti();
			maybe = 0;
			patience = 140;
			tolerance = 0;
			do {
				if (sb_test_irq_flag) {
					_cli();
					/* VDMSOUND bugfix: a previous invocation of this program without playing sound
					 * leaves the IRQ primed and ready to trigger the instant this code tests again,
					 * leading to false "Caught IRQ prior to DSP command" situations. It's sort of
					 * like the "stuck IRQ" situation and Gravis Ultrasound cards. */
					if (tri == 0 && tolerance == 0) {
						sb_test_irq_flag = 0;
						tolerance++;
						patience = 250;
						_sti();
					}
					else {
						break;
					}
				}
				t8254_wait(t8254_us2ticks(1000));
			} while (--patience != 0);

			/* if the IRQ triggered between unmasking and NOW, then clearly it doesn't belong to the SB */
			if (sb_test_irq_flag) {
				eliminated |= 1U << tries[tri];
				DEBUG(fprintf(stdout,"Caught IRQ prior to DSP command, updating IRQ elimination: 0x%04x\n",eliminated));
				p8259_mask(tries[tri]);
				_dos_setvect(irq2int(tries[tri]),old_irq);
				continue;
			}

			/* make the SoundBlaster trigger an interrupt by playing a short sample block */
			outp(D8237_REG_W_SINGLE_MASK,D8237_MASK_CHANNEL(cx->dma8) | D8237_MASK_SET); /* mask */
			outp(D8237_REG_W_WRITE_MODE,
				D8237_MODER_CHANNEL(cx->dma8) |
				D8237_MODER_TRANSFER(D8237_MODER_XFER_READ) |
				D8237_MODER_MODESEL(D8237_MODER_MODESEL_SINGLE));
			d8237_write_base(cx->dma8,dma->phys); /* RAM location with not much around */
			d8237_write_count(cx->dma8,testlen);
			outp(D8237_REG_W_SINGLE_MASK,D8237_MASK_CHANNEL(cx->dma8)); /* unmask */

			/* Time Constant */
			if (!sndsb_write_dsp_timeconst(cx,timeconst) || !sndsb_write_dsp(cx,0x14) ||
				!sndsb_write_dsp(cx,testlen-1) || !sndsb_write_dsp(cx,(testlen-1)>>8)) {
				outp(D8237_REG_W_SINGLE_MASK,D8237_MASK_CHANNEL(cx->dma8) | D8237_MASK_SET); /* unmask */
				p8259_mask(tries[tri]);
				_dos_setvect(irq2int(tries[tri]),old_irq);
				continue;
			}

			/* wait for IRQ to show response */
			_sti();
			maybe = 0;
			patience = 140;
			do {
				if (sb_test_irq_flag) {
					DEBUG(fprintf(stdout,"Flag with %ums to go for IRQ %d\n",patience,tries[tri]));
					_cli();
					sb_test_irq_flag = 0; /* immediately clear it */
					maybe = 1;
					break;
				}
				t8254_wait(t8254_us2ticks(1000));
			} while (--patience != 0);
			outp(D8237_REG_W_SINGLE_MASK,D8237_MASK_CHANNEL(cx->dma8) | D8237_MASK_SET); /* unmask */

			DEBUG(fprintf(stdout,"    maybe=%u\n",maybe));
			if (maybe == 0) {
				p8259_mask(tries[tri]);
				_dos_setvect(irq2int(tries[tri]),old_irq);
				continue;
			}

			if (!sndsb_reset_dsp(cx)) {
				DEBUG(fprintf(stdout,"WARNING: DSP reset failed, aborting IRQ probe\n"));
				p8259_mask(tries[tri]);
				_dos_setvect(irq2int(tries[tri]),old_irq);
				break;
			}

			/* wait for IRQ to show response (prior to triggering one) */
			_cli();
			sb_test_irq_flag = 0;
			_sti();
			maybe = 0;
			patience = 140;
			do {
				if (sb_test_irq_flag) break;
				t8254_wait(t8254_us2ticks(1000));
			} while (--patience != 0);

			/* if the IRQ triggered between unmasking and NOW, then clearly it doesn't belong to the SB */
			if (sb_test_irq_flag) {
				eliminated |= 1U << tries[tri];
				DEBUG(fprintf(stdout,"Caught IRQ prior to DSP command, updating IRQ elimination: 0x%04x\n",eliminated));
				p8259_mask(tries[tri]);
				_dos_setvect(irq2int(tries[tri]),old_irq);
				continue;
			}

			/* make the SoundBlaster trigger an interrupt by playing a short sample block */
			outp(D8237_REG_W_SINGLE_MASK,D8237_MASK_CHANNEL(cx->dma8) | D8237_MASK_SET); /* mask */
			outp(D8237_REG_W_WRITE_MODE,
				D8237_MODER_CHANNEL(cx->dma8) |
				D8237_MODER_TRANSFER(D8237_MODER_XFER_READ) |
				D8237_MODER_MODESEL(D8237_MODER_MODESEL_SINGLE));
			d8237_write_base(cx->dma8,dma->phys); /* RAM location with not much around */
			d8237_write_count(cx->dma8,testlen);
			outp(D8237_REG_W_SINGLE_MASK,D8237_MASK_CHANNEL(cx->dma8)); /* unmask */

			/* Time Constant */
			if (!sndsb_write_dsp_timeconst(cx,timeconst) || !sndsb_write_dsp(cx,0x14) ||
				!sndsb_write_dsp(cx,testlen-1) || !sndsb_write_dsp(cx,(testlen-1)>>8)) {
				outp(D8237_REG_W_SINGLE_MASK,D8237_MASK_CHANNEL(cx->dma8) | D8237_MASK_SET); /* unmask */
				p8259_mask(tries[tri]);
				_dos_setvect(irq2int(tries[tri]),old_irq);
				continue;
			}

			/* wait for IRQ to show response */
			_sti();
			maybe = 0;
			patience = 140;
			do {
				if (sb_test_irq_flag) {
					DEBUG(fprintf(stdout,"Flag with %ums to go on IRQ %d\n",patience,tries[tri]));
					_cli();
					sb_test_irq_flag = 0; /* immediately clear it */
					maybe = 1;
					break;
				}
				t8254_wait(t8254_us2ticks(1000));
			} while (--patience != 0);
			outp(D8237_REG_W_SINGLE_MASK,D8237_MASK_CHANNEL(cx->dma8) | D8237_MASK_SET); /* unmask */

			DEBUG(fprintf(stdout,"    maybe2=%u\n",maybe));
			if (maybe == 0) {
				p8259_mask(tries[tri]);
				_dos_setvect(irq2int(tries[tri]),old_irq);
				continue;
			}

			if (!sndsb_reset_dsp(cx)) {
				DEBUG(fprintf(stdout,"WARNING: DSP reset failed, aborting IRQ probe\n"));
				p8259_mask(tries[tri]);
				_dos_setvect(irq2int(tries[tri]),old_irq);
				break;
			}

			/* OK cleanup */
			p8259_mask(tries[tri]);
			_dos_setvect(irq2int(tries[tri]),old_irq);

			possible |= 1U << tries[tri];
			DEBUG(fprintf(stdout,"Possible=0x%04X\n",possible));
		}
		/* loop while we see possibilities, but more than one IRQ appears to be it */
		DEBUG(fprintf(stdout,"Round %u result: possible=0x%04x\n",possible));
	} while (possible != 0 && (possible&(possible-1)) != 0);

	if (possible != 0 && (possible&(possible-1)) == 0) {
		for (tri=0;tri < sizeof(tries);tri++) {
			if (possible & (1U << tries[tri])) {
				cx->irq = tries[tri];
				break;
			}
		}
	}

	/* release DMA buffer */
	dma_8237_free_buffer(dma);

	/* restore interrupt mask */
	_cli();
	p8259_write_mask(0,ml);
	p8259_write_mask(8,mh);
	_sti();
#endif
}

/* On Sound Blaster cards prior to the SB16 the only way to autodetect the IRQ
 * was to cause a SB IRQ and watch the interrupt controller to see which one
 * went off. that's what this function does. */
/* NTS: This doesn't work in some situations:
 *        - Windows XP native Sound Blaster emulation under NTVDM.EXE
 *            Workaround: use the SBLASTER environment variable given by NTVDM.EXE itself
 *        - Sun/Oracle VirtualBox SB16 emulation (short DSP blocks fail to trigger IRQ activity)
 *            Workaround: read the SB16 compatible mixer byte to obtain configuration
 *        - Microsoft Virtual PC SB16 emulation (short DSP blocks fail to trigger IRQ activity)
 *            Workaround: read the SB16 compatible mixer byte to obtain configuration */
void sndsb_manual_probe_irq(struct sndsb_ctx *cx) {
#if TARGET_MSDOS == 16 && (defined(__COMPACT__) || defined(__SMALL__))
	/* too much code */
#else
	unsigned int round = 0;
	unsigned char ml,mh,maybe;
	unsigned int patience = 0;
	unsigned short eliminated = 0U,irr,possible;
	unsigned char tries[] = {2,3,5,7,10},tri;
	DEBUG(fprintf(stdout,"Sound blaster IRQ unknown, I'm going to have to probe for it\n"));

	_cli();
	ml = p8259_read_mask(0);	/* IRQ0-7 */
	mh = p8259_read_mask(8);	/* IRQ8-15 */
	p8259_write_mask(0,0xFF);	/* mask off all interrupts */
	p8259_write_mask(8,0xFF);

	/* wait a bit. during the wait, mark off any interrupts
	 * that happen while we're waiting because they're obviously
	 * not coming from the Sound Blaster */
	patience = 250;
	do {
		t8254_wait(t8254_us2ticks(1000));
		irr  = (unsigned short)p8259_read_IRR(0);
		irr |= (unsigned short)p8259_read_IRR(8) << 8U;
		for (tri=0;tri < sizeof(tries);tri++) {
			if (irr & (1U << tries[tri])) {
				eliminated |= 1U << tries[tri];
			}
		}
	} while (--patience != 0);
	DEBUG(fprintf(stdout,"Pre-test IRQ elimination: 0x%04X\n",eliminated));

	/* restore interrupt mask */
	p8259_write_mask(0,ml);
	p8259_write_mask(8,mh);
	_sti();

	round = 0;
	do {
		if (++round >= 8)
			break;

		/* go through the remaining ones, one at a time */
		possible = 0;
		for (tri=0;tri < sizeof(tries);tri++) {
			if (eliminated & (1U << tries[tri]))
				continue;
			if (!sndsb_reset_dsp(cx)) {
				DEBUG(fprintf(stdout,"WARNING: DSP reset failed, aborting IRQ probe\n"));
				break;
			}

			DEBUG(fprintf(stdout,"  Now testing IRQ %u\n",tries[tri]));

			/* clear SoundBlaster's previous interrupt */
			inp(cx->baseio+SNDSB_BIO_DSP_READ_STATUS);

			_cli();
			p8259_write_mask(0,0xFF);	/* mask off all interrupts */
			p8259_write_mask(8,0xFF);

			/* did this IRQ already trigger? then the SB didn't do it */
			irr = (unsigned short)p8259_read_IRR(tries[tri]);
			if (irr & (1 << (tries[tri] & 7))) {
				eliminated |= 1U << tries[tri];
				DEBUG(fprintf(stdout,"Caught IRQ prior to DSP command, updating IRQ elimination: 0x%04x\n",eliminated));
				continue;
			}

			/* make the SoundBlaster trigger an interrupt */
			if (!sndsb_write_dsp(cx,0xF2)) {
				if (!sndsb_write_dsp(cx,0xF2)) {
					DEBUG(fprintf(stdout,"WARNING: DSP write failed, aborting IRQ probe\n"));
					break;
				}
			}

			/* wait for IRQ to show response */
			maybe = 0;
			patience = 10;
			do {
				irr = (unsigned short)p8259_read_IRR(tries[tri]);
				if (irr & (1 << (tries[tri] & 7))) {
					maybe = 1;
					break;
				}
				t8254_wait(t8254_us2ticks(1000));
			} while (--patience != 0);

			DEBUG(fprintf(stdout,"    maybe=%u\n",maybe));
			if (maybe == 0)
				continue;

			/* restore interrupt mask */
			p8259_write_mask(0,ml);
			p8259_write_mask(8,mh);
			_sti();

			if (!sndsb_reset_dsp(cx)) {
				DEBUG(fprintf(stdout,"WARNING: DSP reset failed, aborting IRQ probe\n"));
				break;
			}

			/* clear SoundBlaster's previous interrupt */
			inp(cx->baseio+SNDSB_BIO_DSP_READ_STATUS);

			_cli();
			p8259_write_mask(0,0xFF);	/* mask off all interrupts */
			p8259_write_mask(8,0xFF);

			/* did this IRQ already trigger? then the SB didn't do it */
			irr = (unsigned short)p8259_read_IRR(tries[tri]);
			if (irr & (1 << (tries[tri] & 7))) {
				eliminated |= 1U << tries[tri];
				DEBUG(fprintf(stdout,"Caught IRQ prior to DSP command, updating IRQ elimination: 0x%04x\n",eliminated));
				continue;
			}

			/* make the SoundBlaster trigger an interrupt */
			if (!sndsb_write_dsp(cx,0xF2)) {
				if (!sndsb_write_dsp(cx,0xF2)) {
					DEBUG(fprintf(stdout,"WARNING: DSP write failed, aborting IRQ probe\n"));
					break;
				}
			}

			/* wait for IRQ to show response */
			maybe = 0;
			patience = 10;
			do {
				irr = (unsigned short)p8259_read_IRR(tries[tri]);
				if (irr & (1 << (tries[tri] & 7))) {
					maybe = 1;
					break;
				}
				t8254_wait(t8254_us2ticks(1000));
			} while (--patience != 0);

			DEBUG(fprintf(stdout,"    maybe2=%u\n",maybe));
			if (maybe == 0)
				continue;

			possible |= 1U << tries[tri];
		}
		/* loop while we see possibilities, but more than one IRQ appears to be it */
		DEBUG(fprintf(stdout,"Round %u result: possible=0x%04x\n",possible));
	} while (possible != 0 && (possible&(possible-1)) != 0);

	if (possible != 0 && (possible&(possible-1)) == 0) {
		for (tri=0;tri < sizeof(tries);tri++) {
			if (possible & (1U << tries[tri])) {
				cx->irq = tries[tri];
				break;
			}
		}
	}

	/* restore interrupt mask */
	p8259_write_mask(0,ml);
	p8259_write_mask(8,mh);
	_sti();
#endif
}

void sndsb_manual_probe_high_dma(struct sndsb_ctx *cx) {
#if TARGET_MSDOS == 16 && (defined(__COMPACT__) || defined(__SMALL__))
	/* too much code */
#else
	/* NTS: Original code test-played 8192 bytes at 8KHz.
	 *      On every test machine, this meant a considerably long delay when probing.
	 *      To help speed it up, we now play a much shorter sample at 22KHz.
	 *      Unfortunately a sample playback block that short doesn't trigger an IRQ
	 *      under certain emulators like VirtualBox or Virtual PC. If we detect that we're
	 *      running under such emulators we then use a longer block size. */
	unsigned int testlen = 22050/20; /* 1/20th of a second */
	unsigned char tries[] = {5,6,7},tri;
	unsigned int srate = 22050;
	unsigned char dma_count_began = 0;
	unsigned char eliminated = 0;
	uint16_t prev[sizeof(tries)];
	unsigned int patience = 0,rem;
	struct dma_8237_allocation *dma;
	DEBUG(fprintf(stdout,"Sound blaster high DMA unknown, I'm going to have to probe for it\n"));

	if (windows_mode != WINDOWS_NT) {
		/* Sun/Oracle VirtualBox: Sound transfers that are too short are dropped without any
		 * IRQ signal from the emulated SB16 card. Apparently this also has to do with a bug
		 * in their DMA controller emulation where 'terminal count' is the original programmed
		 * value rather than the 0xFFFF value most DMA controllers return. In other words,
		 * we're compensating for VirtualBox's mediocre DMA emulation. */
		if (cx->virtualbox_emulation) {
			DEBUG(fprintf(stdout,"Setting test duration to longer period to work with VirtualBox\n"));
			testlen = 22050/5;
		}
	}

	/* sit back for a bit and watch the DMA channels. if any of them
	 * are cycling, then they are active. NTS: Because the SB16 is
	 * the only one using high DMA and it has a function to tell us
	 * directly, we only probe the lower 8-bit channels */
	_cli();
	for (tri=0;tri < sizeof(tries);tri++) prev[tri] = d8237_read_count_lo16(tries[tri]);
	patience = 500;
	do {
		for (tri=0;tri < sizeof(tries);tri++) {
			if (eliminated & (1U << tries[tri]))
				continue;
			if (prev[tri] != d8237_read_count_lo16(tries[tri]))
				eliminated |= 1U << tries[tri];
		}
	} while (--patience != 0);
	DEBUG(fprintf(stdout,"Pre-test DMA elimination 0x%02x\n",eliminated));

	dma = dma_8237_alloc_buffer(testlen);
	if (dma != NULL) {
#if TARGET_MSDOS == 32
		memset(dma->lin,0,testlen);
#else
		_fmemset(dma->lin,0,testlen);
#endif

		for (tri=0;tri < sizeof(tries);tri++) {
			if (eliminated & (1U << tries[tri]))
				continue;
			if (!sndsb_reset_dsp(cx))
				break;

			/* clear SoundBlaster's previous interrupt */
			/* note that some emulations of the card will fail to play the block
			 * unless we clear the interrupt status. */
			inp(cx->baseio+SNDSB_BIO_DSP_READ_STATUS);
			inp(cx->baseio+SNDSB_BIO_DSP_READ_STATUS16);

			DEBUG(fprintf(stdout,"     Testing DMA channel %u\n",tries[tri]));

			/* set up the DMA channel */
			outp(d8237_ioport(tries[tri],D8237_REG_W_SINGLE_MASK),
				D8237_MASK_CHANNEL(tries[tri]) | D8237_MASK_SET); /* mask */
			outp(d8237_ioport(tries[tri],D8237_REG_W_WRITE_MODE),
				D8237_MODER_CHANNEL(tries[tri]) |
				D8237_MODER_TRANSFER(D8237_MODER_XFER_READ) |
				D8237_MODER_MODESEL(D8237_MODER_MODESEL_SINGLE));
			d8237_write_base(tries[tri],dma->phys); /* RAM location with not much around */
			d8237_write_count(tries[tri],testlen);
			outp(d8237_ioport(tries[tri],D8237_REG_W_SINGLE_MASK),
				D8237_MASK_CHANNEL(tries[tri])); /* unmask */

			/* Time Constant */
			if (!sndsb_write_dsp_outrate(cx,srate))
				continue;

			/* play a short block */
			if (!sndsb_write_dsp(cx,0xB0|0x02)) continue; /* 16-bit single block FIFO on */
			if (!sndsb_write_dsp(cx,0x10)) continue; /* mono signed */
			if (!sndsb_write_dsp(cx,testlen)) continue;
			if (!sndsb_write_dsp(cx,testlen>>8)) continue;
			DEBUG(fprintf(stdout,"        DSP block started\n",tries[tri]));

			/* wait */
			dma_count_began = 0;
			patience = (unsigned int)(((unsigned long)testlen * 1500UL) / (unsigned long)srate);
			do {
				rem = d8237_read_count(tries[tri]);
				if (rem <= 2 || rem >= 0xFFFF) break; /* if below 2 or at terminal count */

				/* explanation: it turns out some emulation software doesn't quite do the DMA
				 * controllers correctly: on terminal count their counter register reverts to
				 * the value we originally set it to, rather than 0xFFFF. so to detect terminal
				 * count we have to watch it count down, then return to 0xFFFF or to it's
				 * original value.
				 *
				 * This hack is necessary to detect DMA cycling under Sun/Oracle VirtualBox */
				if (dma_count_began) {
					if (rem == testlen) {
						DEBUG(fprintf(stdout,
				"DMA controller snafu: Terminal count appears to be the original\n"
				"counter value, not the 0xFFFF value returned by most controllers.\n"
				"Expect other DOS programs to choke on it too!\n"));
						rem = 0;
						break;
					}
				}
				else {
					if (rem != testlen)
						dma_count_began = 1;
				}

				t8254_wait(t8254_us2ticks(1000));
			} while (--patience != 0);
			if (rem >= 0xFFFF) rem = 0; /* the DMA counter might return 0xFFFF when terminal count reached */
			outp(d8237_ioport(tries[tri],D8237_REG_W_SINGLE_MASK),
				D8237_MASK_CHANNEL(tries[tri]) | D8237_MASK_SET); /* mask */
			sndsb_reset_dsp(cx);

			/* clear SoundBlaster's previous interrupt */
			inp(cx->baseio+SNDSB_BIO_DSP_READ_STATUS);
			inp(cx->baseio+SNDSB_BIO_DSP_READ_STATUS16);

			if ((unsigned int)(rem+1) < testlen) { /* it moved, this must be the right one */
				DEBUG(fprintf(stdout,"        This one changed, must be the right one\n"));
				cx->dma16 = tries[tri];
				break;
			}
		}

		dma_8237_free_buffer(dma);
	}

	_sti();
#endif
}

void sndsb_manual_probe_dma(struct sndsb_ctx *cx) {
#if TARGET_MSDOS == 16 && (defined(__COMPACT__) || defined(__SMALL__))
	/* too much code */
#else
	/* NTS: Original code test-played 8192 bytes at 8KHz.
	 *      On every test machine, this meant a considerably long delay when probing.
	 *      To help speed it up, we now play a much shorter sample at 22KHz.
	 *      Unfortunately a sample playback block that short doesn't trigger an IRQ
	 *      under certain emulators like VirtualBox or Virtual PC. If we detect that we're
	 *      running under such emulators we then use a longer block size. */
	unsigned char timeconst = (unsigned char)((65536UL - (256000000UL / 22050UL)) >> 8UL);
	unsigned int testlen = 22050/20; /* 1/20th of a second */
	unsigned char tries[] = {0,1,3},tri;
	unsigned int srate = 22050;
	unsigned char dma_count_began = 0;
	unsigned char eliminated = 0;
	uint16_t prev[sizeof(tries)];
	unsigned int patience = 0,rem;
	struct dma_8237_allocation *dma;
	DEBUG(fprintf(stdout,"Sound blaster DMA unknown, I'm going to have to probe for it\n"));

	if (windows_mode != WINDOWS_NT) {
		/* Sun/Oracle VirtualBox: Sound transfers that are too short are dropped without any
		 * IRQ signal from the emulated SB16 card. Apparently this also has to do with a bug
		 * in their DMA controller emulation where 'terminal count' is the original programmed
		 * value rather than the 0xFFFF value most DMA controllers return. In other words,
		 * we're compensating for VirtualBox's mediocre DMA emulation. */
		if (cx->virtualbox_emulation) {
			DEBUG(fprintf(stdout,"Setting test duration to longer period to work with VirtualBox\n"));
			testlen = 22050/5;
		}
	}

	/* sit back for a bit and watch the DMA channels. if any of them
	 * are cycling, then they are active. NTS: Because the SB16 is
	 * the only one using high DMA and it has a function to tell us
	 * directly, we only probe the lower 8-bit channels */
	_cli();
	for (tri=0;tri < sizeof(tries);tri++) prev[tri] = d8237_read_count_lo16(tries[tri]);
	patience = 500;
	do {
		for (tri=0;tri < sizeof(tries);tri++) {
			if (eliminated & (1U << tries[tri]))
				continue;
			if (prev[tri] != d8237_read_count_lo16(tries[tri]))
				eliminated |= 1U << tries[tri];
		}
	} while (--patience != 0);
	DEBUG(fprintf(stdout,"Pre-test DMA elimination 0x%02x\n",eliminated));

	dma = dma_8237_alloc_buffer(testlen);
	if (dma != NULL) {
#if TARGET_MSDOS == 32
		memset(dma->lin,128,testlen);
#else
		_fmemset(dma->lin,128,testlen);
#endif

		/* then, initiate short playback tests to figure out which one */
		/* EMULATOR NOTES:
		 *      - Microsoft Virtual PC:              works
		 *      - DOSBox:                            works
		 *      - Sun/Oracle VirtualBox:             works
		 *
		 * Some emulators like VPC and VirtualBox are not concerned with
		 * accurate emulation. Unfortunately for us this means any attempt
		 * to play really short blocks would fail, because those emulators
		 * would just drop the block and not fire the IRQ. */
		for (tri=0;tri < sizeof(tries);tri++) {
			if (eliminated & (1U << tries[tri]))
				continue;
			if (!(d8237_flags&D8237_DMA_SECONDARY) && tries[tri] >= 4)
				continue;
			if (!(d8237_flags&D8237_DMA_PRIMARY) && tries[tri] < 4)
				continue;
			if (!sndsb_reset_dsp(cx))
				break;

			/* clear SoundBlaster's previous interrupt */
			/* note that some emulations of the card will fail to play the block
			 * unless we clear the interrupt status. */
			inp(cx->baseio+SNDSB_BIO_DSP_READ_STATUS);

			DEBUG(fprintf(stdout,"     Testing DMA channel %u\n",tries[tri]));

			/* set up the DMA channel */
			outp(D8237_REG_W_SINGLE_MASK,D8237_MASK_CHANNEL(tries[tri]) | D8237_MASK_SET); /* mask */
			outp(D8237_REG_W_WRITE_MODE,
					D8237_MODER_CHANNEL(tries[tri]) |
					D8237_MODER_TRANSFER(D8237_MODER_XFER_READ) |
					D8237_MODER_MODESEL(D8237_MODER_MODESEL_SINGLE));
			d8237_write_base(tries[tri],dma->phys); /* RAM location with not much around */
			d8237_write_count(tries[tri],testlen);
			outp(D8237_REG_W_SINGLE_MASK,D8237_MASK_CHANNEL(tries[tri])); /* unmask */

			/* Time Constant */
			if (!sndsb_write_dsp_timeconst(cx,timeconst))
				continue;

			/* play a short block */
			if (!sndsb_write_dsp(cx,0x14)) continue;
			if (!sndsb_write_dsp(cx,testlen-1)) continue;
			if (!sndsb_write_dsp(cx,(testlen-1)>>8)) continue;
			DEBUG(fprintf(stdout,"        DSP block started\n",tries[tri]));

			/* wait */
			dma_count_began = 0;
			patience = (unsigned int)(((unsigned long)testlen * 1500UL) / (unsigned long)srate);
			do {
				rem = d8237_read_count(tries[tri]);
				if (rem <= 2 || rem >= 0xFFFF) break; /* if below 2 or at terminal count */

				/* explanation: it turns out some emulation software doesn't quite do the DMA
				 * controllers correctly: on terminal count their counter register reverts to
				 * the value we originally set it to, rather than 0xFFFF. so to detect terminal
				 * count we have to watch it count down, then return to 0xFFFF or to it's
				 * original value.
				 *
				 * This hack is necessary to detect DMA cycling under Sun/Oracle VirtualBox */
				if (dma_count_began) {
					if (rem == testlen) {
						DEBUG(fprintf(stdout,
				"DMA controller snafu: Terminal count appears to be the original\n"
				"counter value, not the 0xFFFF value returned by most controllers.\n"
				"Expect other DOS programs to choke on it too!\n"));
						rem = 0;
						break;
					}
				}
				else {
					if (rem != testlen)
						dma_count_began = 1;
				}

				t8254_wait(t8254_us2ticks(1000));
			} while (--patience != 0);
			if (rem >= 0xFFFF) rem = 0; /* the DMA counter might return 0xFFFF when terminal count reached */
			outp(D8237_REG_W_SINGLE_MASK,D8237_MASK_CHANNEL(tries[tri]) | D8237_MASK_SET); /* mask */
			sndsb_reset_dsp(cx);

			if ((unsigned int)(rem+1) < testlen) { /* it moved, this must be the right one */
				DEBUG(fprintf(stdout,"        This one changed, must be the right one\n"));
				cx->dma8 = tries[tri];
				break;
			}
		}

		dma_8237_free_buffer(dma);
	}

	_sti();
#endif
}

/* this is for taking a base address and probing the I/O ports there to see if something like a SB DSP is there. */
/* it is STRONGLY recommended that you don't do this unless you try only 0x220 or 0x240 and you know that nothing
 * else important is there */
int sndsb_try_base(uint16_t iobase) {
	struct sndsb_ctx *cx;

	if ((iobase&0xF) != 0)
		return 0;
	if (iobase < 0x210 || iobase > 0x270)
		return 0;
	if (sndsb_by_base(iobase) != NULL)
		return 0;

	/* some of our detection relies on knowing what OS we're running under */
	cpu_probe();
	probe_dos();
	detect_windows();

	cx = sndsb_alloc_card();
	if (cx == NULL) return 0;

	DEBUG(fprintf(stdout,"sndsb_try_base(0x%03X)\n",iobase));

	cx->baseio = iobase;
	cx->dma8 = cx->dma16 = cx->irq = -1; /* NTS: zero HERE, the init card routine might figure them out */
	if (!sndsb_init_card(cx)) {
		DEBUG(fprintf(stdout,"failed to init card\n"));
		sndsb_free_card(cx);
		return 0;
	}

	/* if we still have to figure out the IRQ, and it's not PnP then probe around to figure it out */
	if (cx->dsp_ok && !cx->is_gallant_sc6600 && !cx->do_not_probe_irq && cx->pnp_id == 0 && cx->irq == -1 &&
		windows_mode == WINDOWS_NONE && !sndsb_probe_options.disable_manual_irq_probing)
		sndsb_manual_probe_irq(cx);

	/* if we have to, detect the DMA channel. */
	if (cx->dsp_ok && !cx->is_gallant_sc6600 && !cx->do_not_probe_dma && cx->pnp_id == 0 && cx->dma8 == -1 &&
		!sndsb_probe_options.disable_manual_dma_probing)
		sndsb_manual_probe_dma(cx);
	/* and the high DMA channel too, if a SB16 or compatible. */
	if (cx->dsp_ok && !cx->is_gallant_sc6600 && !cx->do_not_probe_dma && cx->pnp_id == 0 && cx->dma16 == -1 &&
		cx->dsp_play_method >= SNDSB_DSPOUTMETHOD_4xx && !sndsb_probe_options.disable_manual_high_dma_probing)
		sndsb_manual_probe_high_dma(cx);

	/* if we still have to figure out the IRQ, then probe around to figure it out */
	if (cx->dsp_ok && !cx->is_gallant_sc6600 && !cx->do_not_probe_irq && cx->pnp_id == 0 && cx->irq == -1 &&
		!sndsb_probe_options.disable_alt_irq_probing)
		sndsb_alt_lite_probe_irq(cx);

	/* If an ESS chipset, there's a good chance that 16-bit PCM is played over the 8-bit DMA channel */
	if (cx->ess_extensions && cx->dma16 < 0 && cx->dma8 >= 0)
		cx->dma16 = cx->dma8;

	sndsb_determine_ideal_dsp_play_method(cx);
	return 1;
}

int sndsb_prepare_dsp_playback(struct sndsb_ctx *cx,unsigned long rate,unsigned char stereo,unsigned char bit16) {
	unsigned long lm;

	/* TODO: Don't play if already playing */

	cx->chose_use_dma = 0;
	cx->chose_autoinit_dma = 0;
	cx->chose_autoinit_dsp = 0;
	cx->direct_dac_sent_command = 0;
	if (cx->dsp_play_method == SNDSB_DSPOUTMETHOD_DIRECT && cx->windows_emulation)
		return 0;

	/* set up the params. if we already did (windows spring hack) then don't do it again, but proceed directly
	 * to programming the hardware */
	if (cx->windows_springwait == 0) {
		cx->buffer_stereo = stereo;
		cx->buffer_16bit = bit16;
		cx->buffer_rate = rate;
		cx->buffer_hispeed = 0;
		cx->buffer_dma_started = 0;
		cx->buffer_last_io = 0;
		cx->dsp_stopping = 0;

		lm = cx->buffer_size;
		if (cx->dsp_adpcm == 0) {
			if (bit16) lm >>= 1UL;
			if (stereo) lm >>= 1UL;
		}

		/* if IRQ interval is not assigned, give it the buffer length.
		   we must also ensure the requested interval is less than the
		   buffer length. */
		if (cx->buffer_irq_interval == 0 ||
			cx->buffer_irq_interval > lm)
			cx->buffer_irq_interval = lm;

		/* Windows XP SB emulation: Microsoft's shameful NTVDM.EXE Sound Blaster emulation
		 * attempts to mimic the auto-init modes of DSP v2.0/v2.1 but has a very stupid bug:
		 * if the interval (DSP block size) you specify is not precisely 1/1, 1/2, 1/4, etc.
		 * of the total buffer size (DMA transfer length), their implementation will miss the
		 * end of the DMA transfer and run off into the weeds.
		 *
		 * Another bug: if the block size is too large (4KB or larger?!?) their implementation
		 * will randomly drop portions of the audio and the audio will seem to play extra fast.
		 *
		 * So we have to restrict the irq interval according to these stupid bugs in order to
		 * produce anything close to glitch free audio when under Windows XP's DOS box.
		 *
		 * Shame on you, Microsoft! */
		if (cx->windows_emulation && cx->windows_xp_ntvdm) {
			if (cx->buffer_irq_interval <= (lm / 16UL) || (cx->buffer_size/8) > 4096)
				cx->buffer_irq_interval = (lm / 16UL);
			else if (cx->buffer_irq_interval <= (lm / 8UL) || (cx->buffer_size/4) > 4096)
				cx->buffer_irq_interval = (lm / 8UL);
			else if (cx->buffer_irq_interval <= (lm / 4UL) || (cx->buffer_size/2) > 4096)
				cx->buffer_irq_interval = (lm / 4UL);
			/* Microsoft's shitty implementation also doesn't mesh well with our circular buffer
			 * implementation when the interval is equal to the buffer size. Ther implementation
			 * makes no effort to simulate a DMA transfer going along at the sample rate, it just
			 * "jumps" forward on IRQ. Just as bad as Gravis's SBOS emulation and their shitty
			 * DMA timing. */
			else
				cx->buffer_irq_interval = (lm / 2UL);
		}
		else if (cx->ess_extensions) {
			/* ESS 688/1869 chipsets: Unless using Goldplay mode we normally tell the chipset
			 * to use 2 or 4 byte demand transfers to optimize ISA bandwidth. If not using
			 * auto-init DMA, this method of transfer will fail if the interval is not a
			 * multiple of 4 bytes.
			 *
			 * I *think* that this might be responsible for why non-auto-init DSP+DMA playback
			 * eventually stalls on one ESS 688-based laptop SB clone I test on. */
			if (!cx->goldplay_mode && !cx->dsp_autoinit_dma) {
				if (bit16) lm <<= 1UL;
				if (stereo) lm <<= 1UL;
				lm &= ~3; /* round down 4 bytes */
				if (lm == 0) lm = 4;
				if (bit16) lm >>= 1UL;
				if (stereo) lm >>= 1UL;
			}
		}

		/* don't let the API play 16-bit audio if less than DSP 4.xx because 16-bit audio played
		 * as 8-bit sounds like very loud garbage, be kind to the user */
		if (bit16) {
			if (cx->ess_extensions) {
				if (cx->dsp_play_method < SNDSB_DSPOUTMETHOD_3xx)
					return 0;
			}
			else if (cx->dsp_play_method < SNDSB_DSPOUTMETHOD_4xx) {
				return 0;
			}
		}

		/* NTS: we use the "can do" function to reject obvious configurations that will never work
		 *      on the card, verses an unsupported configuration that we advise not using */
		if (!sndsb_dsp_out_method_can_do(cx,rate,stereo,bit16))
			return 0;
	}

	/* if we're doing the Windows "spring" buffer hack, then don't do anything.
	 * later when the calling program queries the DMA position, we'll setup DSP playback and call this function again */
	if (cx->windows_emulation && cx->windows_springwait == 0 && cx->windows_xp_ntvdm)
		return 1;

	/* clear any pending DSP events (DSP 4.xx) */
	if (cx->dsp_vmaj >= 4)
		sndsb_interrupt_ack(cx,3);

	/* NTS: I have an old CT1350 that requires the "speaker on" command
	 * even for direct command (0x10) audio to work (or else, you get a
	 * quiet staticky sound that resembles your audio). So while this
	 * command is pointless for Sound Blaster 16 and later, it is vital
	 * for older Sound Blasters.
	 *
	 * CT1350 Detail: DSP v2.2, no mixer chip, does not support stereo,
	 *                maxes out at 44.1KHz, and on the Pentium MMX 200MHz
	 *                system I test it on the DSP has problems playing at
	 *                22050Hz if the floppy drive is running (the audio
	 *                audibly warbles). The card is 8-bit ISA. I also
	 *                noticed modern computer mics don't work with it.
	 *                It was designed for unpowered mics, which were
	 *                common at the time and often used with tape recorders.  */
	sndsb_write_dsp(cx,cx->dsp_record ? 0xD3 : 0xD1); /* turn off speaker if recording, else, turn on */

	/* these methods involve DMA */
	cx->chose_use_dma = 1;
	/* use auto-init DMA unless for some reason we can't */
	cx->chose_autoinit_dma = cx->dsp_autoinit_dma;
	cx->chose_autoinit_dsp = cx->dsp_autoinit_command;

	/* Gravis Ultrasound SBOS/MEGA-EM don't handle auto-init 1.xx very well.
	   the only way to cooperate with their shitty emulation is to strictly
	   limit DMA count to the IRQ interval and to NOT set the auto-init flag */
	if (cx->sbos || cx->mega_em)
		cx->chose_autoinit_dma = cx->chose_autoinit_dsp = 0;

	if (cx->dsp_adpcm > 0) {
		sndsb_write_dsp_timeconst(cx,sndsb_rate_to_time_constant(cx,rate));
		if (stereo || bit16 || cx->dsp_record || cx->goldplay_mode)
			return 0; /* ADPCM modes do not support stereo or 16 bit nor recording */

		/* if DSP 2.xx mode or higher and ADPCM auto-init enabled, enable autoinit */
		if (cx->dsp_play_method >= SNDSB_DSPOUTMETHOD_200 && cx->enable_adpcm_autoinit && cx->dsp_autoinit_command) {
			sndsb_write_dsp_blocksize(cx,cx->buffer_irq_interval);
			cx->chose_autoinit_dsp = 1;
		}
		else {
			cx->chose_autoinit_dsp = 0;
		}
	}
	else if (cx->dsp_play_method == SNDSB_DSPOUTMETHOD_1xx) {
		/* NTS: Apparently, issuing Pause & Resume commands at this stage hard-crashes DOSBox 0.74? */
		sndsb_write_dsp_timeconst(cx,sndsb_rate_to_time_constant(cx,rate * (cx->buffer_stereo ? 2UL : 1UL)));
		cx->chose_autoinit_dsp = 0; /* DSP 1.xx does not support auto-init DSP commands */
	}
	else if (cx->ess_extensions && cx->dsp_play_method == SNDSB_DSPOUTMETHOD_3xx) {
		/* do nothing----using SBPro DSP commands then programming ESS registers serves only to
		 * confuse the chip and cause it to stop responding. */
	}
	else if (cx->dsp_play_method >= SNDSB_DSPOUTMETHOD_200 && cx->dsp_play_method <= SNDSB_DSPOUTMETHOD_3xx) {
		/* DSP 2.00, 2.01+, and DSP 3.xx */
		unsigned long total_rate = rate * (cx->buffer_stereo ? 2UL : 1UL);

		/* NTS: Apparently, issuing Pause & Resume commands at this stage hard-crashes DOSBox 0.74? */
		sndsb_write_dsp_timeconst(cx,sndsb_rate_to_time_constant(cx,total_rate));

		/* DSP 2.01 and higher can do "high-speed" DMA transfers up to 44.1KHz */
		if (cx->dsp_play_method >= SNDSB_DSPOUTMETHOD_201) {
			/* NTS: I have a CT1350B card that has audible problems with the ISA bus when driven up to
			 *      22050Hz in non-hispeed modes (if I have something else run, like reading the floppy
			 *      drive, the audio "warbles", changing speed periodically). So while Creative suggests
			 *      enabling hispeed mode for rates 23KHz and above, I think it would be wiser instead
			 *      to do hispeed mode for 16KHz or higher instead. [1]
			 *         [DSP v2.2 with no copyright string]
			 *         [Tested on Pentium MMX 200MHz system with ISA and PCI slots]
			 *         [Applying fix [1] indeed resolved the audible warbling]
			 *         [Is this fix needed for any other Sound Blaster products of that era?] */
			if (cx->ess_extensions && cx->dsp_play_method == SNDSB_DSPOUTMETHOD_3xx) /* ESS 688/1869 use of the extensions it doesn't matter */
				cx->buffer_hispeed = 0;
			else if (cx->force_hispeed)
				cx->buffer_hispeed = 1;
			else if (cx->dsp_vmaj == 2 && cx->dsp_vmin == 2) /* [1] */
				cx->buffer_hispeed = (total_rate >= (cx->dsp_record ? 8000 : 16000));
			else
				cx->buffer_hispeed = (total_rate >= (cx->dsp_record ? 13000 : 23000));

			/* DSP 3.xx stereo management */
			if (cx->dsp_play_method == SNDSB_DSPOUTMETHOD_3xx) {
				/* Sound Blaster Pro requires the "set input mode to mono/stereo" commands if recording,
				 * and sets mono/stereo mode with a bit defined in a specific mixer register */
				if (cx->dsp_record) sndsb_write_dsp(cx,cx->buffer_stereo ? 0xA8 : 0xA0);
				sndsb_write_mixer(cx,0x0E,(cx->buffer_rate >= 15000 ? 0x20 : 0x00) | (cx->buffer_stereo ? 0x02 : 0x00));
			}

			/* if we need to, transmit block length */
			if (cx->buffer_hispeed || cx->chose_autoinit_dsp)
				sndsb_write_dsp_blocksize(cx,cx->buffer_irq_interval * (stereo?2:1));
		}
		else {
			cx->buffer_hispeed = 0;
			if (cx->chose_autoinit_dsp)
				sndsb_write_dsp_blocksize(cx,cx->buffer_irq_interval * (stereo?2:1));
		}
	}
	else if (cx->dsp_play_method == SNDSB_DSPOUTMETHOD_4xx) {
		/* DSP 4.xx management is much simpler here */
		sndsb_write_dsp_outrate(cx,rate);
	}

	/* auto-init DSP modes require auto-init DMA. if auto-init DMA
	 * is not available, then don't use auto-init DSP commands. */
	if (!cx->chose_autoinit_dma) cx->chose_autoinit_dsp = 0;

	/* pick the DMA buffer length to be programmed.
	 * if auto-init, then we can safely give the entire buffer size.
	 * else, we must match the IRQ interval */
	if (cx->chose_autoinit_dma) {
		cx->buffer_dma_started_length = cx->buffer_size;
	}
	else {
		cx->buffer_dma_started_length = cx->buffer_irq_interval;
		if (cx->dsp_adpcm == 0) {
			if (bit16) cx->buffer_dma_started_length <<= 1UL;
			if (stereo) cx->buffer_dma_started_length <<= 1UL;
		}

		if (cx->backwards)
			cx->buffer_dma_started = cx->buffer_size - cx->buffer_dma_started_length;
	}

	return 1;
}

int sndsb_begin_dsp_playback(struct sndsb_ctx *cx) {
	if (cx->dsp_play_method == SNDSB_DSPOUTMETHOD_DIRECT) {
		cx->gold_memcpy = 0;
		if (cx->dsp_record)
			cx->timer_tick_func = sndsb_timer_tick_directi_cmd;
		else
			cx->timer_tick_func = sndsb_timer_tick_directo_cmd;
	}
	else if (cx->goldplay_mode) {
#if TARGET_MSDOS == 32
		if (cx->goldplay_dma == NULL)
			return 0;
#endif

		cx->gold_memcpy = (cx->buffer_16bit?2:1)*(cx->buffer_stereo?2:1);
		if (cx->dsp_record)
			cx->timer_tick_func = sndsb_timer_tick_goldi_cpy;
		else
			cx->timer_tick_func = sndsb_timer_tick_goldo_cpy;
	}
	else {
		if (cx->dsp_nag_mode)
			cx->timer_tick_func = sndsb_timer_tick_gen;
		else
			cx->timer_tick_func = NULL;

		cx->gold_memcpy = 0;
	}

	if (cx->dsp_play_method == SNDSB_DSPOUTMETHOD_DIRECT) /* do nothing */
		return 1;

	/* defer beginning playback until the program first asks for the DMA position */
	if (cx->windows_emulation && cx->windows_springwait == 0 && cx->windows_xp_ntvdm) {
		cx->windows_springwait = 1;
		return 1;
	}

	if (cx->dsp_adpcm > 0) {
		if (cx->dsp_record || cx->goldplay_mode)
			return 0;

		if (cx->chose_autoinit_dsp) {
			if (cx->dsp_adpcm == ADPCM_4BIT)
				sndsb_write_dsp(cx,0x7D); /* with ref. byte */
			else if (cx->dsp_adpcm == ADPCM_2_6BIT)
				sndsb_write_dsp(cx,0x7F); /* with ref. byte */
			else if (cx->dsp_adpcm == ADPCM_2BIT)
				sndsb_write_dsp(cx,0x1F); /* with ref. byte */
		}
		else {
			unsigned short lv;

			lv = cx->buffer_irq_interval - 1;
			if (cx->dsp_adpcm == ADPCM_4BIT)
				sndsb_write_dsp(cx,0x75); /* with ref. byte */
			else if (cx->dsp_adpcm == ADPCM_2_6BIT)
				sndsb_write_dsp(cx,0x77); /* with ref. byte */
			else if (cx->dsp_adpcm == ADPCM_2BIT)
				sndsb_write_dsp(cx,0x17); /* with ref. byte */
			sndsb_write_dsp(cx,lv);
			sndsb_write_dsp(cx,lv >> 8);
		}
	}
	else if (cx->dsp_play_method >= SNDSB_DSPOUTMETHOD_1xx && cx->dsp_play_method <= SNDSB_DSPOUTMETHOD_3xx) {
		unsigned short lv = (cx->buffer_irq_interval * (cx->buffer_stereo?2:1) * (cx->buffer_16bit?2:1)) - 1;

		if (cx->ess_extensions && cx->dsp_play_method == SNDSB_DSPOUTMETHOD_3xx) {
			/* ESS 688/1869 chipset specific DSP playback.
			   using this mode bypasses a lot of the Sound Blaster Pro emulation
			   and restrictions and allows us to run up to 48KHz 16-bit stereo */
			unsigned short t16;
			int b;

			_cli();

			/* clear IRQ */
			sndsb_interrupt_ack(cx,3);

			b = 0x00; /* DMA disable */
			b |= (cx->chose_autoinit_dsp) ? 0x04 : 0x00;
			b |= (cx->dsp_record) ? 0x0A : 0x00; /* [3]=DMA converter in ADC mode [1]=DMA read for ADC */
			if (sndsb_ess_write_controller(cx,0xB8,b) == -1) {
				_sti();
				return 0;
			}

			b = sndsb_ess_read_controller(cx,0xA8);
			if (b == -1) {
				_sti();
				return 0;
			}
			b &= ~0xB; /* clear mono/stereo and record monitor (bits 3, 1, and 0) */
			b |= (cx->buffer_stereo?1:2);	/* 10=mono 01=stereo */
			if (sndsb_ess_write_controller(cx,0xA8,b) == -1) {
				_sti();
				return 0;
			}

			/* NTS: The meaning of bits 1:0 in register 0xB9
			 *
			 *      00 single DMA transfer mode
			 *      01 demand DMA transfer mode, 2 bytes/request
			 *      10 demand DMA transfer mode, 4 bytes/request
			 *      11 reserved
			 *
			 * NOTES on what happens if you set bits 1:0 (DMA transfer type) to the "reserved" 11 value:
			 *
			 *      ESS 688 (Sharp laptop)          Nothing, apparently. Treated the same as 4 bytes/request
			 *
			 *      ESS 1887 (Compaq Presario)      Triggers a hardware bug where the chip appears to fetch
			 *                                      3 bytes per demand transfer but then only handle 1 byte,
			 *                                      which translates to audio playing at 3x the sample rate
			 *                                      it should be. NOT because the DAC is running any faster,
			 *                                      but because the chip is only playing back every 3rd sample!
			 *                                      This play only 3rds behavior is consistent across 8/16-bit
			 *                                      PCM and mono/stereo.
			 */

			/* TODO: This should be one of the options the user can tinker with for testing! */
			if (cx->goldplay_mode)
				b = cx->buffer_16bit ? 1 : 0;	/* demand transfer DMA 2 bytes (16-bit) or single transfer DMA (8-bit) */
			else
				b = 2;  /* demand transfer DMA 4 bytes per request */

			if (sndsb_ess_write_controller(cx,0xB9,b) == -1) {
				_sti();
				return 0;
			}

			if (cx->buffer_rate > 22050) {
				/* bit 7: = 1
				 * bit 6:0: = sample rate divider
				 *
				 * rate = 795.5KHz / (256 - x) */
				b = 256 - (795500UL / (unsigned long)cx->buffer_rate);
				if (b < 0x80) b = 0x80;
			}
			else {
				/* bit 7: = 0
				 * bit 6:0: = sample rate divider
				 *
				 * rate = 397.7KHz / (128 - x) */
				b = 128 - (397700UL / (unsigned long)cx->buffer_rate);
				if (b < 0) b = 0;
			}
			if (sndsb_ess_write_controller(cx,0xA1,b) == -1) {
				_sti();
				return 0;
			}

			b = 256 - (7160000UL / ((unsigned long)cx->buffer_rate * 32UL)); /* 80% of rate/2 times 82 I think... */
			if (sndsb_ess_write_controller(cx,0xA2,b) == -1) {
				_sti();
				return 0;
			}

			t16 = -(lv+1);
			if (sndsb_ess_write_controller(cx,0xA4,t16) == -1 || /* DMA transfer count low */
				sndsb_ess_write_controller(cx,0xA5,t16>>8) == -1) { /* DMA transfer count high */
				_sti();
				return 0;
			}

			b = sndsb_ess_read_controller(cx,0xB1);
			if (b == -1) {
				_sti();
				return 0;
			}
			b &= ~0xA0; /* clear compat game IRQ, fifo half-empty IRQs */
			b |= 0x50; /* set overflow IRQ, and "no function" */
			if (sndsb_ess_write_controller(cx,0xB1,b) == -1) {
				_sti();
				return 0;
			}

			b = sndsb_ess_read_controller(cx,0xB2);
			if (b == -1) {
				_sti();
				return 0;
			}
			b &= ~0xA0; /* clear compat */
			b |= 0x50; /* set DRQ/DACKB inputs for DMA */
			if (sndsb_ess_write_controller(cx,0xB2,b) == -1) {
				_sti();
				return 0;
			}

			b = 0x51; /* enable FIFO+DMA, reserved, load signal */
			b |= (cx->buffer_16bit ^ cx->audio_data_flipped_sign) ? 0x20 : 0x00; /* signed complement mode or not */
			if (sndsb_ess_write_controller(cx,0xB7,b) == -1) {
				_sti();
				return 0;
			}

			b = 0x90; /* enable FIFO+DMA, reserved, load signal */
			b |= (cx->buffer_16bit ^ cx->audio_data_flipped_sign) ? 0x20 : 0x00; /* signed complement mode or not */
			b |= (cx->buffer_stereo) ? 0x08 : 0x40; /* [3]=stereo [6]=!stereo */
			b |= (cx->buffer_16bit) ? 0x04 : 0x00; /* [2]=16bit */
			if (sndsb_ess_write_controller(cx,0xB7,b) == -1) {
				_sti();
				return 0;
			}

			b = sndsb_ess_read_controller(cx,0xB8);
			if (b == -1) {
				_sti();
				return 0;
			}
			if (sndsb_ess_write_controller(cx,0xB8,b | 1) == -1) { /* enable DMA */
				_sti();
				return 0;
			}
		}
		else {
			if (cx->chose_autoinit_dsp) {
				/* preparation function has already transmitted block length, use autoinit commands */
				if (cx->buffer_hispeed)
					sndsb_write_dsp(cx,cx->dsp_record ? 0x98 : 0x90);
				else
					sndsb_write_dsp(cx,cx->dsp_record ? 0x2C : 0x1C);
			}
			else {
				/* send single-cycle command, then transmit length */
				if (cx->buffer_hispeed)
					sndsb_write_dsp(cx,cx->dsp_record ? 0x99 : 0x91);
				else {
					sndsb_write_dsp(cx,cx->dsp_record ? 0x24 : 0x14);
					sndsb_write_dsp(cx,lv);
					sndsb_write_dsp(cx,lv >> 8);
				}
			}
		}
	}
	else if (cx->dsp_play_method == SNDSB_DSPOUTMETHOD_4xx) {
		unsigned long lv = (cx->buffer_irq_interval * (cx->buffer_stereo?2:1)) - 1;

		if (lv > 65535UL) lv = 65535UL;

		sndsb_write_dsp(cx,(cx->buffer_16bit ? 0xB0 : 0xC0) | (cx->chose_autoinit_dsp?0x04:0x00) |
			((!cx->chose_autoinit_dsp && cx->dsp_4xx_fifo_single_cycle) ? 0x02 : 0x00) |
			((cx->chose_autoinit_dsp && cx->dsp_4xx_fifo_autoinit) ? 0x02 : 0x00) |
			(cx->dsp_record ? 0x08 : 0x00));	/* bCommand FIFO on */
		sndsb_write_dsp(cx,(cx->audio_data_flipped_sign ? 0x10 : 0x00) ^
			((cx->buffer_stereo ? 0x20 : 0x00) | (cx->buffer_16bit ? 0x10 : 0x00))); /* bMode */
		sndsb_write_dsp(cx,lv);
		sndsb_write_dsp(cx,lv>>8);
	}

	cx->timer_tick_signal = 0;
	return 1;
}

int sndsb_stop_dsp_playback(struct sndsb_ctx *cx) {
	cx->gold_memcpy = 0;
	cx->dsp_stopping = 1;
	cx->windows_springwait = 0;
	cx->timer_tick_func = NULL;
	if (cx->direct_dac_sent_command) {
		if (cx->dsp_record)
			sndsb_read_dsp(cx);
		else
			sndsb_write_dsp(cx,0x80);

		cx->direct_dac_sent_command = 0;
	}

	/* NTS: As far as I can tell, the best way to stop the sound card is just reset the DSP.
	 *      The "Exit auto-init" commands don't seem to work */
	if (cx->dsp_play_method >= SNDSB_DSPOUTMETHOD_1xx)
		sndsb_reset_dsp(cx);
	if (cx->dsp_play_method >= SNDSB_DSPOUTMETHOD_3xx && cx->dsp_record)
		sndsb_write_dsp(cx,0xA0);

	if ((cx->buffer_16bit && cx->dma16 >= 0) || (!cx->buffer_16bit && cx->dma8 >= 0)) {
		uint16_t pr,cr;
		unsigned int nonmove = 0;
		/* wait for the DMA channel to stop moving */
		if (cx->buffer_16bit)	cr = d8237_read_count(cx->dma16);
		else			cr = d8237_read_count(cx->dma8);
		do {
			t8254_wait(t8254_us2ticks(10000)); /* 10ms */
			pr = cr;
			if (cx->buffer_16bit)	cr = d8237_read_count(cx->dma16);
			else			cr = d8237_read_count(cx->dma8);
			if (pr == cr) nonmove++;
			else nonmove = 0;
		} while (nonmove < 3);
	}

	if (cx->dsp_play_method > SNDSB_DSPOUTMETHOD_DIRECT) {
		sndsb_shutdown_dma(cx);
		sndsb_write_mixer(cx,0x0E,0);
	}

	cx->timer_tick_signal = 0;
	sndsb_write_dsp(cx,0xD3); /* turn off speaker */

	if (cx->ess_extensions && cx->dsp_play_method == SNDSB_DSPOUTMETHOD_3xx) {
		int b;

		b = sndsb_ess_read_controller(cx,0xB8);
		if (b != -1) {
			b &= ~0x01; /* stop DMA */
			sndsb_ess_write_controller(cx,0xB8,b);
		}
	}

	return 1;
}

void sndsb_send_buffer_again(struct sndsb_ctx *cx) {
	unsigned long lv;
	unsigned char ch;

	if (cx->dsp_stopping) return;
	if (cx->dsp_play_method == SNDSB_DSPOUTMETHOD_DIRECT) return;
	ch = cx->buffer_16bit ? cx->dma16 : cx->dma8;

	if (!cx->chose_autoinit_dma)
		outp(d8237_ioport(ch,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(ch) | D8237_MASK_SET); /* mask */

	/* ESS chipsets: I believe the reason non-auto-init DMA+DSP is halting is because
	 * we first needs to stop DMA on the chip THEN reprogram the DMA controller.
	 * Perhaps the FIFO is hardwired to refill at all times and reprogramming the
	 * DMA controller THEN twiddling the DMA enable opens a window of opportunity
	 * for refill to happen at the wrong time? */
	if (!cx->chose_autoinit_dsp) {
		if (cx->dsp_adpcm > 0) {
		}
		else if (cx->ess_extensions && cx->dsp_play_method == SNDSB_DSPOUTMETHOD_3xx) {
			unsigned char b;

			/* stop DMA */
			b = sndsb_ess_read_controller(cx,0xB8);
			sndsb_ess_write_controller(cx,0xB8,b & ~1);
		}
	}

	/* if we're doing it the non-autoinit method, then we
	   also need to update the DMA pointer */
	if (!cx->chose_autoinit_dma) {
		unsigned long npos = cx->buffer_dma_started;
		unsigned long rem = cx->buffer_dma_started;

		lv = cx->buffer_irq_interval;
		if (cx->dsp_adpcm == 0) {
			if (cx->buffer_16bit) lv <<= 1UL;
			if (cx->buffer_stereo) lv <<= 1UL;
		}

		if (cx->backwards) {
			if (rem == 0) {
				npos = cx->buffer_size - lv;
				rem = cx->buffer_size;
			}
			else {
				if (npos >= lv) npos -= lv;
				else npos = 0;
			}
		}
		else {
			npos += cx->buffer_dma_started_length;
			rem = npos + lv;
			if (npos >= cx->buffer_size) {
				npos = 0;
				rem = lv;
			}
			else if (rem > cx->buffer_size) {
				rem = cx->buffer_size;
			}
		}

		cx->buffer_dma_started = npos;
		cx->buffer_dma_started_length = lv = rem - npos;
		if (cx->backwards)
			d8237_write_base(ch,cx->buffer_phys+cx->buffer_dma_started+cx->buffer_dma_started_length-1); /* RAM location with not much around */
		else
			d8237_write_base(ch,cx->buffer_phys+cx->buffer_dma_started); /* RAM location with not much around */
		d8237_write_count(ch,cx->buffer_dma_started_length);
		outp(d8237_ioport(ch,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(ch)); /* unmask */
		if (lv != 0) lv--;
	}
	else {
		lv = cx->buffer_irq_interval;
		if (cx->dsp_adpcm == 0) {
			if (cx->buffer_16bit) lv <<= 1UL;
			if (cx->buffer_stereo) lv <<= 1UL;
		}
		if (lv != 0) lv--;
	}

	/* if we're doing the one-block-at-a-time 1.xx method, then start another right now */
	if (!cx->chose_autoinit_dsp) {
		if (cx->dsp_adpcm > 0) {
			if (cx->dsp_adpcm == ADPCM_4BIT)
				sndsb_write_dsp(cx,0x74); /* without ref. byte */
			else if (cx->dsp_adpcm == ADPCM_2_6BIT)
				sndsb_write_dsp(cx,0x76); /* without ref. byte */
			else if (cx->dsp_adpcm == ADPCM_2BIT)
				sndsb_write_dsp(cx,0x16); /* without ref. byte */
			sndsb_write_dsp(cx,lv);
			sndsb_write_dsp(cx,lv >> 8);
		}
		else if (cx->dsp_play_method >= SNDSB_DSPOUTMETHOD_1xx && cx->dsp_play_method <= SNDSB_DSPOUTMETHOD_3xx) {
			/* send single-cycle command, then transmit length */
			if (cx->ess_extensions && cx->dsp_play_method == SNDSB_DSPOUTMETHOD_3xx) {
				unsigned short t16;
				unsigned char b;

				t16 = -(lv+1);
				sndsb_ess_write_controller(cx,0xA4,t16); /* DMA transfer count low */
				sndsb_ess_write_controller(cx,0xA5,t16>>8); /* DMA transfer count high */

				/* start DMA again */
				b = sndsb_ess_read_controller(cx,0xB8);
				sndsb_ess_write_controller(cx,0xB8,b | 1);
			}
			else {
				if (cx->buffer_hispeed) {
					sndsb_write_dsp_blocksize(cx,lv+1);
					sndsb_write_dsp(cx,cx->dsp_record ? 0x99 : 0x91);
				}
				else {
					sndsb_write_dsp(cx,cx->dsp_record ? 0x24 : 0x14);
					sndsb_write_dsp(cx,lv);
					sndsb_write_dsp(cx,lv >> 8);
				}
			}
		}
		else if (cx->dsp_play_method == SNDSB_DSPOUTMETHOD_4xx) {
			lv++;
			if (cx->buffer_16bit) lv >>= 1UL;
			lv--;
			sndsb_write_dsp(cx,(cx->buffer_16bit ? 0xB0 : 0xC0) | (cx->chose_autoinit_dsp?0x04:0x00) |
				((!cx->chose_autoinit_dsp && cx->dsp_4xx_fifo_single_cycle) ? 0x02 : 0x00) |
				((cx->chose_autoinit_dsp && cx->dsp_4xx_fifo_autoinit) ? 0x02 : 0x00) |
				(cx->dsp_record ? 0x08 : 0x00));	/* bCommand FIFO on */
			sndsb_write_dsp(cx,(cx->audio_data_flipped_sign ? 0x10 : 0x00) ^
				((cx->buffer_stereo ? 0x20 : 0x00) | (cx->buffer_16bit ? 0x10 : 0x00))); /* bMode */
			sndsb_write_dsp(cx,lv);
			sndsb_write_dsp(cx,lv>>8);
		}
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////
//// FIXME FIXME FIXME This is MPU-401 support code. It does not belong in the Sound Blaster library   ////
//// because MPU-401 can exist independently of Sound Blaster. Move this code out to it's own library! ////
//// In the same manner that Adlib OPL-2/3 support exists in it's own library, MPU-401 belongs in it's ////
//// own library.                                                                                      ////
///////////////////////////////////////////////////////////////////////////////////////////////////////////
#if 0

int sndsb_mpu_command(struct sndsb_ctx *cx,uint8_t d) {
	unsigned int patience = 100;

	do {
		if (inp(cx->mpuio+SNDSB_MPUIO_STATUS) & 0x40) /* if not ready for cmd, wait and try again */
			t8254_wait(t8254_us2ticks(100));
		else {
			outp(cx->mpuio+SNDSB_MPUIO_COMMAND,d);
			return 1;
		}
	} while (--patience != 0);
	return 0;
}

int sndsb_mpu_write(struct sndsb_ctx *cx,uint8_t d) {
	unsigned int patience = 100;

	do {
		if (inp(cx->mpuio+SNDSB_MPUIO_STATUS) & 0x40) /* if not ready for cmd, wait and try again */
			t8254_wait(t8254_us2ticks(100));
		else {
			outp(cx->mpuio+SNDSB_MPUIO_DATA,d);
			return 1;
		}
	} while (--patience != 0);
	return 0;
}

int sndsb_mpu_read(struct sndsb_ctx *cx) {
	unsigned int patience = 100;

	do {
		if (inp(cx->mpuio+SNDSB_MPUIO_STATUS) & 0x80) /* if data ready not ready, wait and try again */
			t8254_wait(t8254_us2ticks(100));
		else {
			return inp(cx->mpuio+SNDSB_MPUIO_DATA);
		}
	} while (--patience != 0);

	return -1;
}

/* this code makes sure the MPU exists */
int sndsb_probe_mpu401(struct sndsb_ctx *cx) {
	unsigned int patience = 10;
	int c;

	if (cx->mpuio == 0) return 0;

	/* check the command register. note however that if neither data is available
	 * or a command can be written this can return 0xFF */
	if (inp(cx->mpuio+SNDSB_MPUIO_STATUS) == 0xFF) {
		/* hm, perhaps it's stuck returning data? */
		do { /* wait for it to signal no data and/or ability to write command */
			inp(cx->mpuio+SNDSB_MPUIO_DATA);
			if (inp(cx->mpuio+SNDSB_MPUIO_STATUS) != 0xFF)
				break;

			if (--patience == 0) return 0;
			t8254_wait(t8254_us2ticks(100)); /* 100us */
		} while(1);
	}

	patience=3;
	do {
		/* OK we got the status register to return something other than 0xFF.
		 * Issue a reset */
		if (sndsb_mpu_command(cx,0xFF)) {
			if ((c=sndsb_mpu_read(cx)) == 0xFE) {
				break;
			}
		}

		if (--patience == 0)
			return 0;

		t8254_wait(t8254_us2ticks(10)); /* 10us */
	} while (1);

	return 1;
}

/// detect fragment

	if (cx->mpuio == 0) { /* uh oh, we have to probe for it */
		if (sndsb_by_mpu(0x330) == NULL) {
			cx->mpuio = 0x330; /* more common */
			if (sndsb_probe_mpu401(cx))
				cx->mpu_ok = 1;
			else {
				if (sndsb_by_mpu(0x300) == NULL) {
					cx->mpuio = 0x300; /* less common */
					if (sndsb_probe_mpu401(cx))
						cx->mpu_ok = 1;
					else {
						cx->mpuio = 0;
					}
				}
			}
		}
	}
	else {
		if (sndsb_probe_mpu401(cx))
			cx->mpu_ok = 1;
	}

/// end fragment

#endif
