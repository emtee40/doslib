
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ctype.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/vga/vga.h>
#include <hw/dos/dos.h>
#include <hw/8237/8237.h>		/* DMA controller */
#include <hw/8254/8254.h>		/* 8254 timer */
#include <hw/8259/8259.h>		/* 8259 PIC interrupts */
#include <hw/vga/vgagui.h>
#include <hw/vga/vgatty.h>
#include <hw/dos/doswin.h>
#include <hw/floppy/floppy.h>

struct dma_8237_allocation*		floppy_dma = NULL; /* DMA buffer */

char	tmp[1024];

/* "current position" */
static int current_log_sect = 1;
static int current_log_head = 0;
static int current_phys_head = 0;
static int current_log_track = 0;
static int current_phys_rw_gap = 0x1B;		/* gap size for read/write commands */
static int current_phys_fmt_gap = 0x54;		/* gap size when formatting */
static int current_sectsize_p2 = 2;		/* NTS: 128 << N, 128 << 2 = 512 */
static int current_sectsize_smaller = 0xFF;	/* if sectsize_p2 == 0 */
static int current_sectcount = 2;		/* two sectors */
static unsigned char allow_multitrack = 1;	/* set MT bit */
static unsigned char mfm_mode = 1;		/* set bit to indicate MFM (not FM) */

static void (interrupt *my_irq0_old_irq)() = NULL;
static void (interrupt *my_floppy_old_irq)() = NULL;
static struct floppy_controller *my_floppy_irq_floppy = NULL;
static unsigned long floppy_irq_counter = 0;
static int my_floppy_irq_number = -1;

void do_floppy_controller_unhook_irq(struct floppy_controller *fdc);

static void interrupt my_irq0() {
    /* we need to manipulate the BIOS data area to prevent the BIOS from turning off the motor behind our backs */
    /* target values:
     *   40:3F  diskette motor status
     *   40:40  motor shutoff counter */
#if TARGET_MSDOS == 32
    *((unsigned char*)0x43F) = 0x00; /* motors are off */
    *((unsigned char*)0x440) = 0x00; /* keep motor shutoff counter zero */
#else
    *((unsigned char far*)MK_FP(0x40,0x3F)) = 0x00; /* motors are off */
    *((unsigned char far*)MK_FP(0x40,0x40)) = 0x00; /* keep motor shutoff counter zero */
#endif

    my_irq0_old_irq();
}

static void interrupt my_floppy_irq() {
	int i;

	i = vga_state.vga_width*(vga_state.vga_height-1);

	_cli();
	if (my_floppy_irq_floppy != NULL) {
		my_floppy_irq_floppy->irq_fired++;

		/* ack PIC */
		if (my_floppy_irq_floppy->irq >= 8) p8259_OCW2(8,P8259_OCW2_NON_SPECIFIC_EOI);
		p8259_OCW2(0,P8259_OCW2_NON_SPECIFIC_EOI);

		/* If too many IRQs fired, then unhook the IRQ and use polling from now on. */
		if (my_floppy_irq_floppy->irq_fired >= 0xFFFEU) {
			do_floppy_controller_unhook_irq(my_floppy_irq_floppy);
			vga_state.vga_alpha_ram[i+12] = 0x1C00 | '!';
			my_floppy_irq_floppy->irq_fired = ~0; /* make sure the IRQ counter is as large as possible */
		}
	}

	/* we CANNOT use sprintf() here. sprintf() doesn't work to well from within an interrupt handler,
	 * and can cause crashes in 16-bit realmode builds. */
	vga_state.vga_alpha_ram[i++] = 0x1F00 | 'I';
	vga_state.vga_alpha_ram[i++] = 0x1F00 | 'R';
	vga_state.vga_alpha_ram[i++] = 0x1F00 | 'Q';
	vga_state.vga_alpha_ram[i++] = 0x1F00 | ':';
	vga_state.vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter / 100000UL) % 10UL));
	vga_state.vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter /  10000UL) % 10UL));
	vga_state.vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter /   1000UL) % 10UL));
	vga_state.vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter /    100UL) % 10UL));
	vga_state.vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter /     10UL) % 10UL));
	vga_state.vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter /       1L) % 10UL));
	vga_state.vga_alpha_ram[i++] = 0x1F00 | ' ';

	floppy_irq_counter++;
}

int wait_for_enter_or_escape() {
	int c;

	do {
		c = getch();
		if (c == 0) c = getch() << 8;
	} while (!(c == 13 || c == 27));

	return c;
}

void do_floppy_controller_hook_irq(struct floppy_controller *fdc) {
	if (my_floppy_irq_number >= 0 || my_floppy_old_irq != NULL || fdc->irq < 0)
		return;

	/* let the IRQ know what floppy controller */
	my_floppy_irq_floppy = fdc;

	/* enable on floppy controller */
	p8259_mask(fdc->irq);
	floppy_controller_enable_irq(fdc,1);

	/* hook IRQ */
	my_floppy_old_irq = _dos_getvect(irq2int(fdc->irq));
	_dos_setvect(irq2int(fdc->irq),my_floppy_irq);
	my_floppy_irq_number = fdc->irq;

	/* enable at PIC */
	p8259_unmask(fdc->irq);
}

void do_floppy_controller_unhook_irq(struct floppy_controller *fdc) {
	if (my_floppy_irq_number < 0 || my_floppy_old_irq == NULL || fdc->irq < 0)
		return;

	/* disable on floppy controller, then mask at PIC */
	p8259_mask(fdc->irq);
	floppy_controller_enable_irq(fdc,0);

	/* restore the original vector */
	_dos_setvect(irq2int(fdc->irq),my_floppy_old_irq);
	my_floppy_irq_number = -1;
	my_floppy_old_irq = NULL;
}

void do_floppy_controller_enable_irq(struct floppy_controller *fdc,unsigned char en) {
	if (!en || fdc->irq < 0 || fdc->irq != my_floppy_irq_number)
		do_floppy_controller_unhook_irq(fdc);
	if (en && fdc->irq >= 0)
		do_floppy_controller_hook_irq(fdc);
}

int floppy_controller_wait_busy_in_instruction(struct floppy_controller *fdc,unsigned int timeout) {
	do {
		floppy_controller_read_status(fdc);
		if (!floppy_controller_busy_in_instruction(fdc)) return 1;
		t8254_wait(t8254_us2ticks(1000));
	} while (--timeout != 0);

	return 0;
}

int floppy_controller_wait_data_ready_ms(struct floppy_controller *fdc,unsigned int timeout) {
	do {
		floppy_controller_read_status(fdc);
		if (floppy_controller_data_io_ready(fdc)) return 1;
		t8254_wait(t8254_us2ticks(1000));
	} while (--timeout != 0);

	return 0;
}

int floppy_controller_wait_irq(struct floppy_controller *fdc,unsigned int timeout,unsigned int counter) {
	do {
		if (fdc->irq_fired >= counter) break;
		t8254_wait(t8254_us2ticks(1000));
	} while (--timeout != 0);

	return 0;
}

int floppy_controller_write_data(struct floppy_controller *fdc,const unsigned char *data,int len) {
	unsigned int oflags = get_cpu_flags();
	int ret = 0;

	_cli(); /* clear interrupts so we can focus on talking to the FDC */
	while (len > 0) {
		if (!floppy_controller_wait_data_ready(fdc,1000)) {
			if (ret == 0) ret = -1;
			break;
		}
		if (!floppy_controller_can_write_data(fdc)) {
			if (ret == 0) ret = -2;
			break;
		}

		floppy_controller_write_data_byte(fdc,*data++);
		len--; ret++;
	}

	if (oflags&0x200/*IF interrupt enable was on*/) _sti();
	return ret;
}

#if TARGET_MSDOS == 32
int floppy_controller_write_data_ndma(struct floppy_controller *fdc,const unsigned char *data,int len) {
#else
int floppy_controller_write_data_ndma(struct floppy_controller *fdc,const unsigned char far *data,int len) {
#endif
	unsigned int oflags = get_cpu_flags();
	int ret = 0;

	_cli();
	while (len > 0) {
		if (!floppy_controller_wait_data_ready(fdc,1000)) {
			if (ret == 0) ret = -1;
			break;
		}
		if (!floppy_controller_wait_data_write_non_dma_ready(fdc,1000)) {
			if (ret == 0) ret = -2;
			break;
		}

		floppy_controller_write_data_byte(fdc,*data++);
		len--; ret++;
	}

	if (oflags&0x200/*IF interrupt enable was on*/) _sti();
	return ret;
}

int floppy_controller_read_data(struct floppy_controller *fdc,unsigned char *data,int len) {
	unsigned int oflags = get_cpu_flags();
	int ret = 0;

	_cli();
	while (len > 0) {
		if (!floppy_controller_wait_data_ready(fdc,1000)) {
			if (ret == 0) ret = -1;
			break;
		}
		if (!floppy_controller_can_read_data(fdc)) {
			if (ret == 0) ret = -2;
			break;
		}

		*data++ = floppy_controller_read_data_byte(fdc);
		len--; ret++;
	}

	if (oflags&0x200/*IF interrupt enable was on*/) _sti();
	return ret;
}

#if TARGET_MSDOS == 32
int floppy_controller_read_data_ndma(struct floppy_controller *fdc,unsigned char *data,int len) {
#else
int floppy_controller_read_data_ndma(struct floppy_controller *fdc,unsigned char far *data,int len) {
#endif
	unsigned int oflags = get_cpu_flags();
	int ret = 0;

	_cli();
	while (len > 0) {
		if (!floppy_controller_wait_data_ready(fdc,1000)) {
			if (ret == 0) ret = -1;
			break;
		}
		if (!floppy_controller_wait_data_read_non_dma_ready(fdc,1000)) {
			if (ret == 0) ret = -2;
			break;
		}

		*data++ = floppy_controller_read_data_byte(fdc);
		len--; ret++;
	}

	if (oflags&0x200/*IF interrupt enable was on*/) _sti();
	return ret;
}

void do_floppy_controller_reset(struct floppy_controller *fdc) {
	struct vga_msg_box vgabox;

	vga_msg_box_create(&vgabox,"FDC reset in progress",0,0);

	floppy_controller_set_reset(fdc,1);
	t8254_wait(t8254_us2ticks(1000000));
	floppy_controller_set_reset(fdc,0);
	floppy_controller_wait_data_ready_ms(fdc,1000);
	floppy_controller_read_status(fdc);

	vga_msg_box_destroy(&vgabox);
}

void do_check_interrupt_status(struct floppy_controller *fdc) {
	struct vga_msg_box vgabox;
	char cmd[10],resp[10];
	int rd,wd,rdo,wdo;

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	/* Check Interrupt Status (x8h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0   0   0   0   1   0   0   0
	 *
	 */

	wdo = 1;
	cmd[0] = 0x08;	/* Check interrupt status */
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 1) {
		sprintf(tmp,"Failed to write data to FDC, %u/%u",wd,wdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* wait for data ready. does not fire an IRQ (because you use this to clear IRQ status!) */
	floppy_controller_wait_data_ready_ms(fdc,1000);

	/* NTS: It's not specified whether this returns 2 bytes if success and 1 if no IRQ pending.. or...? */
	rdo = 2;
	resp[1] = 0;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		sprintf(tmp,"Failed to read data from the FDC, %u/%u",rd,rdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* Check Interrupt Status (x8h) response
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |              ST0
	 *      1 |        Current Cylinder
	 */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* return value is ST0 and the current cylinder */
	fdc->st[0] = resp[0];
	fdc->cylinder = resp[1];
}

void do_spin_up_motor(struct floppy_controller *fdc,unsigned char drv) {
	if (drv > 3) return;

	if (!(fdc->digital_out & (0x10 << drv))) {
		struct vga_msg_box vgabox;

		vga_msg_box_create(&vgabox,"Spinning up motor",0,0);

		/* if the motor isn't on, then turn it on, and then wait for motor to stabilize */
		floppy_controller_set_motor_state(fdc,drv,1);
		t8254_wait(t8254_us2ticks(500000)); /* 500ms */

		vga_msg_box_destroy(&vgabox);
	}
}

void do_seek_drive_rel(struct floppy_controller *fdc,int track) {
	struct vga_msg_box vgabox;
	char cmd[10];
	int wd,wdo;

	do_spin_up_motor(fdc,fdc->digital_out&3);

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	floppy_controller_reset_irq_counter(fdc);

	/* Seek Relative (1xFh)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  1 DIR   0   0   1   1   1   1
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *      2 |         Cylinder Step
	 *
	 *         HD = Head select (on PC platform, doesn't matter)
	 *    DR1,DR0 = Drive select
	 *        DIR = Step direction (1=inward increasing, 0=outward decreasing)
	 *  Cyl. step = How many tracks to step */

	wdo = 3;
	cmd[0] = 0x8F + (track < 0 ? 0x40 : 0x00); /* Seek rel [6:6] = DIR */
	cmd[1] = (fdc->digital_out&3)+(current_phys_head?0x04:0x00)/* [1:0] = DR1,DR0 [2:2] HD (doesn't matter) */;
	cmd[2] = abs(track);
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 3) {
		sprintf(tmp,"Failed to write data to FDC, %u/%u",wd,wdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* fires an IRQ. doesn't return state */
	if (fdc->use_irq) floppy_controller_wait_irq(fdc,1000,1);
	floppy_controller_wait_data_ready_ms(fdc,1000);

	/* Seek Relative (1xFh) response
	 *
	 * (none)
	 */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* use Check Interrupt Status */
	do_check_interrupt_status(fdc);
}

unsigned long prompt_track_number();
void do_seek_drive(struct floppy_controller *fdc,uint8_t track);
void do_calibrate_drive(struct floppy_controller *fdc);
int do_read_sector_id(unsigned char resp[7],struct floppy_controller *fdc,unsigned char head);

void do_step_tracks(struct floppy_controller *fdc) {
	unsigned long start,end,track;
	struct vga_msg_box vgabox;
	unsigned char resp[7];
	char tmp[64];
	int del,c;

	start = prompt_track_number();
	if (start == (~0UL)) return;

	end = prompt_track_number();
	if (start == (~0UL)) return;
	if (start > end) return;

	do_spin_up_motor(fdc,fdc->digital_out&3);

	vga_msg_box_create(&vgabox,"Seeking to track 0",0,0);
	do_calibrate_drive(fdc);
	do_calibrate_drive(fdc);
	do_calibrate_drive(fdc);
	vga_msg_box_destroy(&vgabox);

	/* step! */
	for (track=start;track <= end;track++) {
		if (kbhit()) {
			c = getch();
			if (c == 0) c = getch() << 8;
			if (c == 27) break;
		}

		sprintf(tmp,"Seeking track %lu",track);
		vga_msg_box_create(&vgabox,tmp,0,0);

		/* move head */
		do_seek_drive(fdc,(uint8_t)track);

		/* read sector ID to try and force head select if FDC doesn't do it from seek command */
		do_read_sector_id(resp,fdc,current_phys_head);

		/* delay 1 second */
		for (del=0;del < 1000;del++)
			t8254_wait(t8254_us2ticks(1000));

		/* un-draw the box */
		vga_msg_box_destroy(&vgabox);
	}
}

void do_seek_drive(struct floppy_controller *fdc,uint8_t track) {
	struct vga_msg_box vgabox;
	char cmd[10];
	int wd,wdo;

	do_spin_up_motor(fdc,fdc->digital_out&3);

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc)) {
        /* Fun fact, on real hardware, resetting the floppy controller also causes the
         * controller to forget which track it's on (reset to zero).
         *
         * So, issuing a command to seek to track 10, resetting the controller, then issuing another seek to track 10
         * will leave the head on track 20. */
		do_floppy_controller_reset(fdc);
        do_calibrate_drive(fdc);
    }

	floppy_controller_reset_irq_counter(fdc);

	/* Seek (xFh)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0   0   0   0   1   1   1   1
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *      2 |            Cylinder
	 *
	 *         HD = Head select (on PC platform, doesn't matter)
	 *    DR1,DR0 = Drive select
	 *   Cylinder = Track to move to */

	wdo = 3;
	cmd[0] = 0x0F;	/* Seek */
	cmd[1] = (fdc->digital_out&3)+(current_phys_head?0x04:0x00)/* [1:0] = DR1,DR0 [2:2] HD (doesn't matter) */;
	cmd[2] = track;
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 3) {
		sprintf(tmp,"Failed to write data to FDC, %u/%u",wd,wdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* fires an IRQ. doesn't return state */
	if (fdc->use_irq) floppy_controller_wait_irq(fdc,1000,1);
	floppy_controller_wait_data_ready_ms(fdc,1000);

	/* Seek (xFh) response
	 *
	 * (none)
	 */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* use Check Interrupt Status */
	do_check_interrupt_status(fdc);
}

void do_check_drive_status(struct floppy_controller *fdc) {
	struct vga_msg_box vgabox;
	char cmd[10],resp[10];
	int rd,wd,rdo,wdo;

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	/* Check Drive Status (x4h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0   0   0   0   0   1   0   0
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *
	 *       HD = Head select (on PC platform, doesn't matter)
	 *  DR1,DR0 = Drive select */

	wdo = 2;
	cmd[0] = 0x04;	/* Check drive status */
	cmd[1] = (fdc->digital_out&3)+(current_phys_head?0x04:0x00)/* [1:0] = DR1,DR0 [2:2] = HD = 0 */;
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 2) {
		sprintf(tmp,"Failed to write data to FDC, %u/%u",wd,wdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* wait for data ready. does not fire an IRQ */
	floppy_controller_wait_data_ready_ms(fdc,1000);

	/* Check Drive Status (x4h) response
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |              ST3
         */

	rdo = 1;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		sprintf(tmp,"Failed to read data from the FDC, %u/%u",rd,rdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* return value is ST3 */
	fdc->st[3] = resp[0];
}

void do_calibrate_drive(struct floppy_controller *fdc) {
	struct vga_msg_box vgabox;
	char cmd[10];
	int wd,wdo;

	do_spin_up_motor(fdc,fdc->digital_out&3);

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	floppy_controller_reset_irq_counter(fdc);

	/* Calibrate Drive (x7h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0   0   0   0   0   1   1   1
	 *      1 |  x   x   x   x   x   0 DR1 DR0
	 *
	 *  DR1,DR0 = Drive select */

	wdo = 2;
	cmd[0] = 0x07;	/* Calibrate */
	cmd[1] = (fdc->digital_out&3)/* [1:0] = DR1,DR0 */;
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 2) {
		sprintf(tmp,"Failed to write data to FDC, %u/%u",wd,wdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* fires an IRQ. doesn't return state */
	if (fdc->use_irq) floppy_controller_wait_irq(fdc,1000,1);
	floppy_controller_wait_data_ready_ms(fdc,1000);

	/* Calibrate Drive (x7h) response
	 *
	 * (none)
	 */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* use Check Interrupt Status */
	do_check_interrupt_status(fdc);
}

int do_floppy_get_version(struct floppy_controller *fdc) {
	int rd,wd,rdo,wdo;
	char cmd[10],resp[10];

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	/* Version (1x0h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0   0   0   1   0   0   0   0
	 */

	wdo = 1;
	cmd[0] = 0x10;
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 1) {
		do_floppy_controller_reset(fdc);
		return 0;
	}

	/* wait for data ready. does not fire an IRQ */
	floppy_controller_wait_data_ready_ms(fdc,1000);

	rdo = 1;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		do_floppy_controller_reset(fdc);
		return 0;
	}

	/* Version (1x0h) response
	 *
	 *   Byte |  7    6    5    4    3    2    1    0
	 *   -----+--------------------------------------
	 *      0 |            Version or error
	 *
	 * Version/Error values:
	 *     0x80: Not an enhanced controller (we got an invalid opcode)
	 *     0x90: Enhanced controller
         */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	fdc->version = resp[0];
	return 1;

}

int do_floppy_controller_specify(struct floppy_controller *fdc) {
	int retry_count=0;
	char cmd[10];
	int wd,wdo;
	int drv;

retry:	drv = fdc->digital_out & 3;

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	/* Specify (x3h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0   0   0   0   0   0   1   1
	 *      1 |  <--- SRT --->   <----HUT---->
	 *      2 |  <-----------HLT--------->  ND
	 */

	wdo = 3;
	cmd[0] = 0x03;
	cmd[1] = (fdc->current_srt << 4) | (fdc->current_hut & 0xF);
	cmd[2] = (fdc->current_hlt << 1) | (fdc->non_dma_mode & 1);
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 3) {
		do_floppy_controller_reset(fdc);
		return 0;
	}

	/* wait for data ready. does not fire an IRQ */
	floppy_controller_wait_data_ready_ms(fdc,1000);

	/* Specify (x3h) response
	 *
	 * (none)
	 */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* use Check Interrupt Status */
	do_check_interrupt_status(fdc);

	/* if it failed, try again */
	if ((fdc->st[0]&0xC0) == 0xC0 || (fdc->st[0]&0xC0) == 0x40) {
		if ((++retry_count) < 8) { /* what the hell VirtualBox? */
			floppy_controller_drive_select(fdc,drv);
			goto retry;
		}

		return 0;
	}

	return 1;
}

int do_floppy_dumpreg(struct floppy_controller *fdc) {
	int rd,wd,rdo,wdo;
	char cmd[10],resp[10];

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	/* Dump Registers (xEh)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0   0   0   0   1   1   1   0
	 */

	wdo = 1;
	cmd[0] = 0x0E;
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 1) {
		do_floppy_controller_reset(fdc);
		return 0;
	}

	/* wait for data ready. does not fire an IRQ */
	floppy_controller_wait_data_ready_ms(fdc,1000);

	rdo = 10;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		do_floppy_controller_reset(fdc);
		return 0;
	}

	/* Dump Registers (xEh) response
	 *
	 *   Byte |  7    6    5    4    3    2    1    0
	 *   -----+--------------------------------------
	 *      0 |       Current Cylinder (drive 0)             <- Also known as PCN0
	 *      1 |       Current Cylinder (drive 1)             <- Also known as PCN1
	 *      2 |       Current Cylinder (drive 2)             <- Also known as PCN2
	 *      3 |       Current Cylinder (drive 3)             <- Also known as PCN3
	 *      4 |  <------SRT----->    <------HUT----->
	 *      5 |  <------------HLT-------------->   ND
	 *      6 |  <--------------SC/EOT-------------->
	 *      7 |LOCK   0   D3   D2   D1   D0   GAP WGATE
	 *      8 |  0   EIS EFIFO POLL  <---FIFOTHR---->
	 *      9 |  <--------------PRETRK-------------->
	 *
	 *          SRT = Step rate interval (0.5 to 8ms in 0.5 steps at 1mbit/sec, affected by data rate)
	 *          HUT = Head unload time (affected by data rate)
	 *          HLT = Head load time (affected by data rate)
	 *           ND = Non-DMA mode flag. If set, the controller should operate in the non-DMA mode.
	 *       SC/EOT = Sector count/End of track
	 *      D3...D0 = Drive Select 3..0, which ones are in Perpendicular Mode
	 *          GAP = Alter Gap 2 length in Perpendicular Mode
	 *        WGATE = Write gate alters timing of WE, to allow for pre-erase loads in perpendicular drives.
	 *          EIS = Enable Implied Seek
	 *        EFIFO = Enable FIFO. When 0, FIFO is enabled. When 1, FIFO is disabled.
	 *         POLL = Polling disable. When set, internal polling routine is disabled.
	 *      FIFOTHR = FIFO threshold - 1 in the execution phase of read/write commands. 0h to Fh maps to 1...16 bytes.
	 *       PRETRK = Precompensation start track number.
	 *
	 *
	 *      82077AA Drive Control Delays (ms) Table 5-10
	 *
	 *        |                HUT                 |                 SRT                |
	 *        |   1M     500K     300K     250K    |   1M     500K     300K     250K    |
	 *     ---+------------------------------------+------------------------------------+
	 *      0 |  128      256      426      512    |  8.0       16     26.7       32    |
	 *      1 |    8       16     26.7       32    |  7.5       15       25       30    |
	 *     ...............................................................................
	 *     Eh |  112      224      373      448    |  1.0        2     3.33        4    |
	 *     Fh |  120      240      400      480    |  0.5        1     1.67        2    |
	 *        +------------------------------------+------------------------------------+
	 *
	 *        |                HLT                 |
	 *        |   1M     500K     300K     250K    |
	 *     ---+------------------------------------+
	 *      0 |  128      256      426      512    |
	 *      1 |    1        2      3.3        4    |
	 *     ..........................................
	 *    7Eh |  126      252      420      504    |
	 *    7Fh |  127      254      423      508    |
	 *        +------------------------------------+
         */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* copy down info we want to keep track of */
	fdc->current_srt = resp[4] >> 4;
	fdc->current_hut = resp[4] & 0x0F;
	fdc->current_hlt = resp[5] >> 1;
	fdc->non_dma_mode = resp[5] & 0x01;
	fdc->implied_seek = (resp[8] >> 6) & 1;

	{
		struct vga_msg_box vgabox;
		char *w=tmp;
		int i;

		for (i=0;i < rd;i++) w += sprintf(w,"%02x ",resp[i]);
		*w++ = '\n';
		*w++ = '\n';
		*w = 0;

		w += sprintf(w,"PCN[0-3](physical track) = %u,%u,%u,%u\n",
			resp[0],resp[1],resp[2],resp[3]);
		w += sprintf(w,"SRT=%u  HUT=%u  HLT=%u  ND=%u\n",
			resp[4]>>4,resp[4]&0xF,
			resp[5]>>1,resp[5]&1);
		w += sprintf(w,"SC/EOT=%u  LOCK=%u  D[0-3](perpendicular)=%u,%u,%u,%u\n",
			resp[6],
			(resp[7]>>7)&1,
			(resp[7]>>5)&1,
			(resp[7]>>4)&1,
			(resp[7]>>3)&1,
			(resp[7]>>2)&1);
		w += sprintf(w,"GAP=%u  WGATE=%u  EIS=%u  EFIFO=%u  POLL=%u\n",
			(resp[7]>>1)&1,
			(resp[7]>>0)&1,
			(resp[8]>>6)&1,
			(resp[8]>>5)&1,
			(resp[8]>>4)&1);
		w += sprintf(w,"FIFOTHR=%u  PRETRK=%u",
			(resp[8]>>0)&0xF,
			resp[9]);

		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
	}

	return 1;
}

int do_read_sector_id(unsigned char resp[7],struct floppy_controller *fdc,unsigned char head) {
	int rd,wd,rdo,wdo;
	char cmd[10];

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	/* Read ID (xAh)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0  MF   0   0   1   0   1   0
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *
	 *         MF = MFM/FM
	 *         HD = Head select (on PC platform, doesn't matter)
	 *    DR1,DR0 = Drive select */

	wdo = 2;
	cmd[0] = 0x0A + (mfm_mode?0x40:0x00); /* Read sector ID [6:6] MFM */
	cmd[1] = (fdc->digital_out&3)+(head<<2)/* [1:0] = DR1,DR0 [2:2] = HD */;
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 2) {
		do_floppy_controller_reset(fdc);
		return 0;
	}

	/* wait for data ready. does not fire an IRQ */
	floppy_controller_wait_data_ready_ms(fdc,1000);

	rdo = 7;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		do_floppy_controller_reset(fdc);
		return 0;
	}

	/* Read ID (xAh) response
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |              ST0
	 *      1 |              ST1
	 *      2 |              ST2
	 *      3 |           Cylinder
	 *      4 |             Head
	 *      5 |        Sector Number
	 *      6 |         Sector Size
         */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* accept ST0..ST2 from response and update */
	if (rd >= 3) {
		fdc->st[0] = resp[0];
		fdc->st[1] = resp[1];
		fdc->st[2] = resp[2];
	}
	if (rd >= 4) {
		fdc->cylinder = resp[3];
	}

	return rd;
}

static unsigned char found_sectors[256];

typedef struct scanid_t {
    unsigned char       c,h,s,sz;
} scanid_t;

int scanlist_cmp(const void *a,const void *b) {
    const scanid_t *sa = (const scanid_t*)a;
    const scanid_t *sb = (const scanid_t*)b;

    if (sa->c > sb->c) return 1;
    else if (sa->c < sb->c) return -1;

    if (sa->h > sb->h) return 1;
    else if (sa->h < sb->h) return -1;

    if (sa->s > sb->s) return 1;
    else if (sa->s < sb->s) return -1;

    if (sa->sz > sb->sz) return 1;
    else if (sa->sz < sb->sz) return -1;

    return 0;
}

void do_readid_scan(struct floppy_controller *fdc,unsigned char headsel) {
    t8254_time_t pt,ct;
    unsigned int i,scancount;
    scanid_t *scanlist,*ps,psc;
    unsigned long countdown = (unsigned long)T8254_REF_CLOCK_HZ * (unsigned long)3; // 3 seconds max
	char resp[10];
	int c,rc;

    scancount = 500;
    scanlist = malloc(scancount * sizeof(scanid_t));
    if (scanlist == NULL) return;

    write_8254_system_timer(0);

    vga_moveto(0,0);
    vga_write_color(0x0E);
    vga_clear();

    vga_moveto(0,0);
    vga_write("Scanning... ");

    pt = ct = read_8254(T8254_TIMER_INTERRUPT_TICK);
    memset(&psc,0,sizeof(psc));
    for (i=0;i < scancount;i++) {
        if ((signed long)countdown < 0L) {
            scancount = i;
            break;
        }

        if (kbhit()) {
            c = getch();
            if (c == 27) {
                scancount = i;
                break;
            }
        }

        ps = scanlist + i;
		if ((c=do_read_sector_id(resp,fdc,headsel)) == 7 && (fdc->st[0] & 0xC0) == 0/*no error*/) {
            ps->c = resp[3];
            ps->h = resp[4];
            ps->s = resp[5];
            ps->sz= resp[6];

            if (ps->c == psc.c &&
                ps->h == psc.h &&
                ps->s == psc.s &&
                ps->sz== psc.sz)
                i--; /* step back, same data */

            psc = *ps;
        }
        else {
            memset(ps,0,sizeof(*ps));
            i--;
        }

        pt = ct;
        ct = read_8254(T8254_TIMER_INTERRUPT_TICK);

        /* timer counts DOWN */
        countdown -= ((unsigned long)(pt - ct)) & 0xFFFFUL;
    }
    vga_write(" C/H/S/SZ list follows\n");

    /* yes, I'm lazy */
    qsort(scanlist,scancount,sizeof(psc),scanlist_cmp);

    /* show it */
    rc=0;
    memset(&psc,0,sizeof(psc));
    for (i=0;i < scancount;i++) {
        ps = scanlist + i;

        if (ps->c != 0 || ps->h != 0 || ps->s != 0 || ps->sz != 0) {
            if (psc.c != ps->c ||
                psc.h != ps->h ||
                psc.s != ps->s ||
                psc.sz!= ps->sz) {
                sprintf(tmp,"%3u/%3u/%3u/%3u |",ps->c,ps->h,ps->s,ps->sz);
                vga_write(tmp);

                if (rc >= (4 * 20)) break;

                if (((++rc)&3) == 0) {
                    vga_write("\n");
                    rc = 0;
                }
            }

            psc = *ps;
        }
    }
    vga_write("\n");

    while (getch() != 13);

    free(scanlist);
}

void do_read_sector_id_demo(struct floppy_controller *fdc) {
    unsigned char tracksel = current_log_track;
	unsigned char headsel = current_phys_head;
    unsigned char fsup = 0;
	char resp[10];
	int c;

	do_spin_up_motor(fdc,fdc->digital_out&3);

	vga_moveto(0,0);
	vga_write_color(0x0E);
	vga_clear();

    memset(found_sectors,0,sizeof(found_sectors));
    fsup = 1;

	vga_write("Read Sector ID demo. Space to switch heads.\n\n");

	do {
		if (kbhit()) {
			c = getch();
			if (c == 0) c = getch() << 8;

			if (c == 27)
				break;
			else if (c == ' ') {
				headsel ^= 1;

                memset(found_sectors,0,sizeof(found_sectors));
                fsup = 1;
            }
            else if (c == '-') {
                if (tracksel > 0) tracksel--;
                do_seek_drive(fdc,tracksel);

                memset(found_sectors,0,sizeof(found_sectors));
                fsup = 1;
            }
            else if (c == '+') {
                if (tracksel < 127) tracksel++;
                do_seek_drive(fdc,tracksel);

                memset(found_sectors,0,sizeof(found_sectors));
                fsup = 1;
            }
            else if (c == 'c') {
                tracksel = 0;
                do_calibrate_drive(fdc);

                memset(found_sectors,0,sizeof(found_sectors));
                fsup = 1;
            }
            else if (c == 's') {
                do_readid_scan(fdc,headsel);

                vga_moveto(0,0);
                vga_write_color(0x0E);
                vga_clear();

                fsup = 1;
            }
		}

		if ((c=do_read_sector_id(resp,fdc,headsel)) == 7 && (fdc->st[0] & 0xC0) == 0/*no error*/) {
			vga_moveto(0,2);
			vga_write_color(0x0F);

			sprintf(tmp,"ST: %02xh %02xh %02xh %02xh\n",fdc->st[0],fdc->st[1],fdc->st[2],fdc->st[3]);
			vga_write(tmp);

			sprintf(tmp,"C/H/S/sz: %-3u/%-3u/%-3u/%-3u       \n",resp[3],resp[4],resp[5],resp[6]);
			vga_write(tmp);

            if (found_sectors[(unsigned char)resp[5]] == 0) {
                found_sectors[(unsigned char)resp[5]] = 1;
                fsup = 1;
            }
		}
		else {
			vga_moveto(0,2);
			vga_write_color(0x0F);

			sprintf(tmp,"ST: --h --h --h --h (ret=%d)\n",c);
			vga_write(tmp);

			sprintf(tmp,"C/H/S/sz: ---/---/---/---       \n");
			vga_write(tmp);
		}

        if (fsup) {
            unsigned int i,mn=255,ma=0;

            vga_moveto(0,8);
            vga_write_color(0x0E);

            for (i=0;i < 256;i++) {
                if (found_sectors[i]) {
                    if (mn > i) mn = i;
                    if (ma < i) ma = i;
                }
            }

            for (i=0;i < 20;i++)
                vga_writec(found_sectors[i]?'*':' ');

            sprintf(tmp," %u-%u     ",mn,ma);
            vga_write(tmp);

            fsup = 0;
        }
	} while (1);
}

signed long prompt_signed_track_number() {
	signed long track = 0;
	struct vga_msg_box box;
	VGA_ALPHA_PTR sco;
	char temp_str[16];
	int c,i=0;

	vga_msg_box_create(&box,"Enter relative track number:",2,0);
	sco = vga_state.vga_alpha_ram + ((box.y+2) * vga_state.vga_width) + box.x + 2;
	while (1) {
		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			track = 0;
			break;
		}
		else if (c == 13) {
			if (i == 0) break;
			temp_str[i] = 0;
			if (isdigit(temp_str[0]))
				track = strtol(temp_str,NULL,0);
			else
				track = 0;

			break;
		}
		else if (isdigit(c) || c == '-') {
			if (i < 15) {
				sco[i] = c | 0x1E00;
				temp_str[i++] = c;
			}
		}
		else if (c == 8) {
			if (i > 0) i--;
			sco[i] = ' ' | 0x1E00;
		}
	}
	vga_msg_box_destroy(&box);

	return track;
}

unsigned long prompt_track_number() {
	unsigned long track = 0;
	struct vga_msg_box box;
	VGA_ALPHA_PTR sco;
	char temp_str[16];
	int c,i=0;

	vga_msg_box_create(&box,"Enter track number:",2,0);
	sco = vga_state.vga_alpha_ram + ((box.y+2) * vga_state.vga_width) + box.x + 2;
	while (1) {
		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			track = (int)(~0UL);
			break;
		}
		else if (c == 13) {
			if (i == 0) break;
			temp_str[i] = 0;
			if (isdigit(temp_str[0]))
				track = strtol(temp_str,NULL,0);
			else
				track = 0;

			break;
		}
		else if (isdigit(c)) {
			if (i < 15) {
				sco[i] = c | 0x1E00;
				temp_str[i++] = c;
			}
		}
		else if (c == 8) {
			if (i > 0) i--;
			sco[i] = ' ' | 0x1E00;
		}
	}
	vga_msg_box_destroy(&box);

	return track;
}

void do_floppy_format_track(struct floppy_controller *fdc) {
	unsigned int data_length,unit_length;
	unsigned int returned_length = 0;
	char cmd[10],resp[10];
	int rd,rdo,wd,wdo,p;
	uint8_t nsect;

	nsect = current_sectcount;

	unit_length = 4;
	data_length = nsect * unit_length;

	vga_moveto(0,0);
	vga_write_color(0x0E);
	vga_clear();

	{
		struct vga_msg_box vgabox;
		char *w=tmp;
		int i;

		w += sprintf(w,"I will format the track as having %u sectors/track,\n",
			nsect);
		w += sprintf(w,"first sector %u, logical track %u, %u bytes/sector.\n",
			current_log_sect,current_log_track,128 << current_sectsize_p2);
		w += sprintf(w,"I am using the sector number count for sectors/track.\n");
		w += sprintf(w,"Since (last I checked) the head is currently located at\n");
		w += sprintf(w,"track %u, physical track %u will be formatted logical\n",
			fdc->cylinder,fdc->cylinder);
		w += sprintf(w,"track %u. The track will be marked as having logical head %u\n",
			current_log_track,
			current_log_head);
		w += sprintf(w,"and formatted on physical head %u\n",
			current_phys_head);
		w += sprintf(w,"\n");
		w += sprintf(w,"Hit ENTER to continue if this is what you want,\nor ESC to stop now.");

		vga_msg_box_create(&vgabox,tmp,0,0);
		i = wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		if (i == 27) return;
	}

	sprintf(tmp,"Formatting C/H/S/sz/num %u/%u/%u/%u/%u\n",current_log_track,current_log_head,current_log_sect,128 << current_sectsize_p2,nsect);
	vga_write(tmp);

	do_spin_up_motor(fdc,fdc->digital_out&3);

	if (floppy_dma == NULL) return;
	if (data_length > floppy_dma->length) {
		nsect = floppy_dma->length / unit_length;
		data_length = nsect * unit_length;
	}
	if (data_length > floppy_dma->length) return;

	/* sector pattern. 4 bytes for each sector to write */
	for (rd=0;rd < nsect;rd++) {
		wd = rd * unit_length;

		/*   |
		 * --+---------------------------
		 * 0 | Cylinder
		 * 1 | Head
		 * 2 | Sector address
		 * 3 | Sector size code
		 */
		floppy_dma->lin[wd++] = current_log_track;
		floppy_dma->lin[wd++] = current_log_head;
		floppy_dma->lin[wd++] = current_log_sect + rd;
		floppy_dma->lin[wd++] = current_sectsize_p2;
	}

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	if (fdc->dma >= 0 && fdc->use_dma) {
		outp(d8237_ioport(fdc->dma,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(fdc->dma) | D8237_MASK_SET); /* mask */

		outp(d8237_ioport(fdc->dma,D8237_REG_W_WRITE_MODE),
			D8237_MODER_CHANNEL(fdc->dma) |
			D8237_MODER_TRANSFER(D8237_MODER_XFER_READ) |
			D8237_MODER_MODESEL(D8237_MODER_MODESEL_SINGLE));

		d8237_write_count(fdc->dma,data_length);
		d8237_write_base(fdc->dma,floppy_dma->phys);

		outp(d8237_ioport(fdc->dma,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(fdc->dma)); /* unmask */

		inp(d8237_ioport(fdc->dma,D8237_REG_R_STATUS)); /* read status port to clear TC bits */
	}

	/* Format Track (xDh)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0  MF   0   0   1   1   0   1
	 *      1 |  0   0   0   0   0  HD DR1 DR0
	 *      2 |  <------ bytes/sector ------->
	 *      3 |  <----- sectors/track ------->
	 *      4 |  <--------- GAP3 ------------>
	 *      5 |  <------ Fill byte ---------->
	 *
	 *         MF = MFM/FM
	 *         HD = Head select (on PC platform, doesn't matter)
	 *    DR1,DR0 = Drive select */

	wdo = 6;
	cmd[0] = (mfm_mode?0x40:0x00)/* MFM */ + 0x0D/* FORMAT TRACK */;
	cmd[1] = (fdc->digital_out&3)+(current_phys_head&1?0x04:0x00)/* [1:0] = DR1,DR0 [2:2] = HD */;
	cmd[2] = current_sectsize_p2;
	cmd[3] = nsect;
	cmd[4] = current_phys_fmt_gap;
	cmd[5] = 0xAA;			/* fill byte */
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 2) {
		vga_write("Write data port failed\n");
		do_floppy_controller_reset(fdc);
		return;
	}

	vga_write("Format in progress\n");

	/* fires an IRQ */
	if (fdc->use_dma) {
		if (fdc->use_irq) floppy_controller_wait_irq(fdc,10000,1); /* 10 seconds */
		floppy_controller_wait_data_ready_ms(fdc,1000);
	}
	else {
		/* NOTES:
		 *
		 * Sun/Oracle VirtualBox floppy emulation bug: PIO mode doesn't seem to be supported properly on write
		 * for more than one sector. It will SEEM like it works, but when you read the data back only the first
		 * sector was ever written. On the same configuration, READ works fine using the same PIO mode. */
		while (returned_length < data_length) {
			if ((returned_length+unit_length) > data_length) break;
			floppy_controller_wait_data_ready_ms(fdc,10000);

			p = floppy_controller_write_data_ndma(fdc,floppy_dma->lin + returned_length,unit_length);
			if (p < 0 || p > unit_length) {
				sprintf(tmp,"NDMA write failed %d (%02xh)\n",p,fdc->main_status);
				vga_write(tmp);
				p = 0;
			}

			returned_length += p;
			/* stop on incomplete transfer */
			if (p != unit_length) break;
		}
	}

	rdo = 7;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		vga_write("Write data port failed\n");
		do_floppy_controller_reset(fdc);
		return;
	}

	/* Format Track (xDh) response
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |              ST0
	 *      1 |              ST1
	 *      2 |              ST2
	 *      3 |               ?
	 *      4 |               ?
	 *      5 |               ?
	 *      6 |               ?
         */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* accept ST0..ST2 from response and update */
	if (rd >= 3) {
		fdc->st[0] = resp[0];
		fdc->st[1] = resp[1];
		fdc->st[2] = resp[2];
	}
	if (rd >= 4) {
		fdc->cylinder = resp[3];
	}

	/* (if DMA) did we get any data? */
	if (fdc->dma >= 0 && fdc->use_dma) {
		uint32_t dma_len = d8237_read_count(fdc->dma);
		uint8_t status = inp(d8237_ioport(fdc->dma,D8237_REG_R_STATUS));

		/* some DMA controllers reset the count back to the original value on terminal count.
		 * so if the DMA controller says terminal count, the true count is zero */
		if (status&D8237_STATUS_TC(fdc->dma)) dma_len = 0;

		if (dma_len > (uint32_t)data_length) dma_len = (uint32_t)data_length;
		returned_length = (unsigned int)((uint32_t)data_length - dma_len);
	}

	sprintf(tmp,"%lu bytes written\n",(unsigned long)returned_length);
	vga_write(tmp);
	vga_write("\n");

	wait_for_enter_or_escape();
}

void do_floppy_write_test(struct floppy_controller *fdc) {
	unsigned int data_length,unit_length;
	unsigned int returned_length = 0;
	char cmd[10],resp[10];
	uint8_t cyl,head,sect,ssz,nsect;
	int rd,rdo,wd,wdo,x,p;

	cyl = current_log_track;
	head = current_log_head;
	sect = current_log_sect;
	nsect = current_sectcount;
	ssz = current_sectsize_p2;

	if (current_sectsize_p2 > 0)
		unit_length = (128 << ssz);
	else
		unit_length = current_sectsize_smaller;

	data_length = nsect * unit_length;

	vga_moveto(0,0);
	vga_write_color(0x0E);
	vga_clear();

	sprintf(tmp,"Writing C/H/S/sz/num %u/%u/%u/%u/%u\n",cyl,head,sect,unit_length,nsect);
	vga_write(tmp);

	do_spin_up_motor(fdc,fdc->digital_out&3);

	if (floppy_dma == NULL) return;
	if (data_length > floppy_dma->length) {
		nsect = floppy_dma->length / unit_length;
		data_length = nsect * unit_length;
	}
	if (data_length > floppy_dma->length) return;

	for (x=0;(unsigned int)x < data_length;x++) floppy_dma->lin[x] = x;

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	if (fdc->dma >= 0 && fdc->use_dma) {
		outp(d8237_ioport(fdc->dma,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(fdc->dma) | D8237_MASK_SET); /* mask */

		outp(d8237_ioport(fdc->dma,D8237_REG_W_WRITE_MODE),
			D8237_MODER_CHANNEL(fdc->dma) |
			D8237_MODER_TRANSFER(D8237_MODER_XFER_READ) |
			D8237_MODER_MODESEL(D8237_MODER_MODESEL_SINGLE));

		d8237_write_count(fdc->dma,data_length);
		d8237_write_base(fdc->dma,floppy_dma->phys);

		outp(d8237_ioport(fdc->dma,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(fdc->dma)); /* unmask */

		inp(d8237_ioport(fdc->dma,D8237_REG_R_STATUS)); /* read status port to clear TC bits */
	}

	/* Write Sector (x5h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 | MT  MF   0   0   0   1   0   1
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *      2 |            Cylinder
	 *      3 |              Head
	 *      4 |         Sector Number
	 *      5 |          Sector Size
	 *      6 |    Track length/last sector
	 *      7 |        Length of GAP3
	 *      8 |          Data Length
	 *
	 *         MT = Multi-track
	 *         MF = MFM/FM
	 *         HD = Head select (on PC platform, doesn't matter)
	 *    DR1,DR0 = Drive select */

	/* NTS: To ensure we read only one sector, Track length/last sector must equal
	 * Sector Number field. The floppy controller stops reading on error or when
	 * sector number matches. This is very important for the PIO mode of this code,
	 * else we'll never complete reading properly and never get to reading back
	 * the status bytes. */

	wdo = 9;
	cmd[0] = (allow_multitrack?0x80:0x00)/*Multitrack*/ + (mfm_mode?0x40:0x00)/* MFM */ + 0x05/* WRITE DATA */;
	cmd[1] = (fdc->digital_out&3)+(current_phys_head&1?0x04:0x00)/* [1:0] = DR1,DR0 [2:2] = HD */;
	cmd[2] = cyl;	/* cyl=0 */
	cmd[3] = head;	/* head=0 */
	cmd[4] = sect;	/* sector=1 */
	cmd[5] = ssz;	/* sector size=2 (512 bytes) */
	cmd[6] = sect+nsect-1; /* last sector of the track (what sector to stop at). */
	cmd[7] = current_phys_rw_gap;
	cmd[8] = current_sectsize_smaller; /* DTL (not used if 256 or larger) */
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 2) {
		vga_write("Write data port failed\n");
		do_floppy_controller_reset(fdc);
		return;
	}

	vga_write("Write in progress\n");

	/* fires an IRQ */
	if (fdc->use_dma) {
		if (fdc->use_irq) floppy_controller_wait_irq(fdc,10000,1); /* 10 seconds */
		floppy_controller_wait_data_ready_ms(fdc,1000);
	}
	else {
		/* NOTES:
		 *
		 * Sun/Oracle VirtualBox floppy emulation bug: PIO mode doesn't seem to be supported properly on write
		 * for more than one sector. It will SEEM like it works, but when you read the data back only the first
		 * sector was ever written. On the same configuration, READ works fine using the same PIO mode. */
		while (returned_length < data_length) {
			if ((returned_length+unit_length) > data_length) break;
			floppy_controller_wait_data_ready_ms(fdc,10000);

			p = floppy_controller_write_data_ndma(fdc,floppy_dma->lin + returned_length,unit_length);
			if (p < 0 || p > unit_length) {
				sprintf(tmp,"NDMA write failed %d (%02xh)\n",p,fdc->main_status);
				vga_write(tmp);
				p = 0;
			}

			returned_length += p;
			/* stop on incomplete transfer */
			if (p != unit_length) break;
		}
	}

	rdo = 7;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		vga_write("Write data port failed\n");
		do_floppy_controller_reset(fdc);
		return;
	}

	/* Write Sector (x5h) response
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |              ST0
	 *      1 |              ST1
	 *      2 |              ST2
	 *      3 |           Cylinder
	 *      4 |             Head
	 *      5 |        Sector Number
	 *      6 |         Sector Size
         */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* accept ST0..ST2 from response and update */
	if (rd >= 3) {
		fdc->st[0] = resp[0];
		fdc->st[1] = resp[1];
		fdc->st[2] = resp[2];
	}
	if (rd >= 4) {
		fdc->cylinder = resp[3];
	}

	/* (if DMA) did we get any data? */
	if (fdc->dma >= 0 && fdc->use_dma) {
		uint32_t dma_len = d8237_read_count(fdc->dma);
		uint8_t status = inp(d8237_ioport(fdc->dma,D8237_REG_R_STATUS));

		/* some DMA controllers reset the count back to the original value on terminal count.
		 * so if the DMA controller says terminal count, the true count is zero */
		if (status&D8237_STATUS_TC(fdc->dma)) dma_len = 0;

		if (dma_len > (uint32_t)data_length) dma_len = (uint32_t)data_length;
		returned_length = (unsigned int)((uint32_t)data_length - dma_len);
	}

	sprintf(tmp,"%lu bytes written\n",(unsigned long)returned_length);
	vga_write(tmp);
	vga_write("\n");

	wait_for_enter_or_escape();
}

void do_floppy_read_test(struct floppy_controller *fdc) {
	unsigned int data_length,unit_length;
	unsigned int returned_length = 0;
	char cmd[10],resp[10];
	uint8_t cyl,head,sect,ssz,nsect;
	int rd,rdo,wd,wdo,x,y,p;

	cyl = current_log_track;
	head = current_log_head;
	sect = current_log_sect;
	nsect = current_sectcount;
	ssz = current_sectsize_p2;

	if (current_sectsize_p2 > 0)
		unit_length = (128 << ssz);
	else
		unit_length = current_sectsize_smaller;

	data_length = nsect * unit_length;

	vga_moveto(0,0);
	vga_write_color(0x0E);
	vga_clear();

	sprintf(tmp,"Reading C/H/S/sz/num %u/%u/%u/%u/%u\n",cyl,head,sect,unit_length,nsect);
	vga_write(tmp);

	do_spin_up_motor(fdc,fdc->digital_out&3);

	if (floppy_dma == NULL) return;
	if (data_length > floppy_dma->length) {
		nsect = floppy_dma->length / unit_length;
		data_length = nsect * unit_length;
	}
	if (data_length > floppy_dma->length) return;

#if TARGET_MSDOS == 32
	memset(floppy_dma->lin,0,data_length);
#else
	_fmemset(floppy_dma->lin,0,data_length);
#endif

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	if (fdc->dma >= 0 && fdc->use_dma) {
		outp(d8237_ioport(fdc->dma,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(fdc->dma) | D8237_MASK_SET); /* mask */

		outp(d8237_ioport(fdc->dma,D8237_REG_W_WRITE_MODE),
			D8237_MODER_CHANNEL(fdc->dma) |
			D8237_MODER_TRANSFER(D8237_MODER_XFER_WRITE) |
			D8237_MODER_MODESEL(D8237_MODER_MODESEL_SINGLE));

		d8237_write_count(fdc->dma,data_length);
		d8237_write_base(fdc->dma,floppy_dma->phys);

		outp(d8237_ioport(fdc->dma,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(fdc->dma)); /* unmask */

		inp(d8237_ioport(fdc->dma,D8237_REG_R_STATUS)); /* read status port to clear TC bits */
	}

	/* Read Sector (x6h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 | MT  MF  SK   0   0   1   1   0
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *      2 |            Cylinder
	 *      3 |              Head
	 *      4 |         Sector Number
	 *      5 |          Sector Size
	 *      6 |    Track length/last sector
	 *      7 |        Length of GAP3
	 *      8 |          Data Length
	 *
	 *         MT = Multi-track
	 *         MF = MFM/FM
	 *         SK = Skip deleted address mark
	 *         HD = Head select (on PC platform, doesn't matter)
	 *    DR1,DR0 = Drive select */

	/* NTS: To ensure we read only one sector, Track length/last sector must equal
	 * Sector Number field. The floppy controller stops reading on error or when
	 * sector number matches. This is very important for the PIO mode of this code,
	 * else we'll never complete reading properly and never get to reading back
	 * the status bytes. */

	wdo = 9;
	cmd[0] = (allow_multitrack?0x80:0x00)/*Multitrack*/ + (mfm_mode?0x40:0x00)/* MFM */ + 0x06/* READ DATA */;
	cmd[1] = (fdc->digital_out&3)+(current_phys_head&1?0x04:0x00)/* [1:0] = DR1,DR0 [2:2] = HD */;
	cmd[2] = cyl;	/* cyl=0 */
	cmd[3] = head;	/* head=0 */
	cmd[4] = sect;	/* sector=1 */
	cmd[5] = ssz;	/* sector size=2 (512 bytes) */
	cmd[6] = sect+nsect-1; /* last sector of the track (what sector to stop at). */
	cmd[7] = current_phys_rw_gap;
	cmd[8] = current_sectsize_smaller; /* DTL (not used if 256 or larger) */
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 2) {
		vga_write("Write data port failed\n");
		do_floppy_controller_reset(fdc);
		return;
	}

	vga_write("Read in progress\n");

	/* fires an IRQ */
	if (fdc->use_dma) {
		if (fdc->use_irq) floppy_controller_wait_irq(fdc,10000,1); /* 10 seconds */
		floppy_controller_wait_data_ready_ms(fdc,1000);
	}
	else {
		while (returned_length < data_length) {
			if ((returned_length+unit_length) > data_length) break;
			floppy_controller_wait_data_ready_ms(fdc,10000);

			p = floppy_controller_read_data_ndma(fdc,floppy_dma->lin + returned_length,unit_length);
			if (p < 0 || p > unit_length) {
				sprintf(tmp,"NDMA read failed %d (%02xh)\n",p,fdc->main_status);
				vga_write(tmp);
				p = 0;
			}

			returned_length += p;
			/* stop on incomplete transfer */
			if (p != unit_length) break;
		}
	}

	rdo = 7;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		vga_write("Read data port failed\n");
		do_floppy_controller_reset(fdc);
		return;
	}

	/* Read Sector (x6h) response
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |              ST0
	 *      1 |              ST1
	 *      2 |              ST2
	 *      3 |           Cylinder
	 *      4 |             Head
	 *      5 |        Sector Number
	 *      6 |         Sector Size
         */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* accept ST0..ST2 from response and update */
	if (rd >= 3) {
		fdc->st[0] = resp[0];
		fdc->st[1] = resp[1];
		fdc->st[2] = resp[2];
	}
	if (rd >= 4) {
		fdc->cylinder = resp[3];
	}

	/* (if DMA) did we get any data? */
	if (fdc->dma >= 0 && fdc->use_dma) {
		uint32_t dma_len = d8237_read_count(fdc->dma);
		uint8_t status = inp(d8237_ioport(fdc->dma,D8237_REG_R_STATUS));

		/* some DMA controllers reset the count back to the original value on terminal count.
		 * so if the DMA controller says terminal count, the true count is zero */
		if (status&D8237_STATUS_TC(fdc->dma)) dma_len = 0;

		if (dma_len > (uint32_t)data_length) dma_len = (uint32_t)data_length;
		returned_length = (unsigned int)((uint32_t)data_length - dma_len);
	}

	sprintf(tmp,"%lu bytes received\n",(unsigned long)returned_length);
	vga_write(tmp);
	vga_write("\n");

	vga_write_color(0x0F);
	for (p=0;p == 0 || p < ((returned_length+255)/256);p++) {
		for (y=0;y < 16;y++) {
			vga_moveto(0,6+y);
			for (x=0;x < 16;x++) {
				sprintf(tmp,"%02x ",floppy_dma->lin[((y+(p*16))*16)+x]);
				vga_write(tmp);
			}

			vga_moveto(50,6+y);
			for (x=0;x < 16;x++) {
				tmp[0] = floppy_dma->lin[((y+(p*16))*16)+x];
				if (tmp[0] < 32) tmp[0] = '.';
				tmp[1] = 0;
				vga_write(tmp);
			}
		}

		if (wait_for_enter_or_escape() == 27)
			break;
	}
}

void do_floppy_controller_read_tests(struct floppy_controller *fdc) {
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	char redraw=1;
	int select=-1;
	int c;

	while (1) {
		if (backredraw) {
			vga = vga_state.vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_state.vga_height;y++) {
				for (x=0;x < vga_state.vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        Read tests ");
			sprintf(tmp,"@%X",fdc->base_io);
			vga_write(tmp);
			if (fdc->irq >= 0) {
				sprintf(tmp," IRQ %d",fdc->irq);
				vga_write(tmp);
			}
			if (fdc->dma >= 0) {
				sprintf(tmp," DMA %d",fdc->dma);
				vga_write(tmp);
			}
			if (floppy_dma != NULL) {
				sprintf(tmp," phys=%08lxh len=%04lxh",(unsigned long)floppy_dma->phys,(unsigned long)floppy_dma->length);
				vga_write(tmp);
			}
			while (vga_state.vga_pos_x < vga_state.vga_width && vga_state.vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			redraw = 0;

			floppy_controller_read_status(fdc);
			floppy_controller_read_DIR(fdc);
			floppy_controller_read_ps2_status(fdc);

			y = 2;
			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"DOR %02xh DIR %02xh Stat %02xh CCR %02xh cyl=%-3u",
				fdc->digital_out,fdc->digital_in,
				fdc->main_status,fdc->control_cfg,
				fdc->cylinder);
			vga_write(tmp);

			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"ST0..3: %02x %02x %02x %02x PS/2 %02x %02x",
				fdc->st[0],fdc->st[1],fdc->st[2],fdc->st[3],
				fdc->ps2_status[0],fdc->ps2_status[1]);
			vga_write(tmp);

			y = 5;
			vga_moveto(8,y++);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("FDC menu");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');
			y++;

			vga_moveto(8,y++);
			vga_write_color((select == 0) ? 0x70 : 0x0F);
			vga_write("Read sector");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			if (select == -1) {
				break;
			}
			else if (select == 0) { /* Read Sector */
				do_floppy_read_test(fdc);
				backredraw = 1;
			}
		}
		else if (c == 0x4800) {
			if (--select < -1)
				select = 0;

			redraw = 1;
		}
		else if (c == 0x5000) {
			if (++select > 0)
				select = -1;

			redraw = 1;
		}
	}
}

void do_floppy_controller_write_tests(struct floppy_controller *fdc) {
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	char redraw=1;
	int select=-1;
	int c;

	while (1) {
		if (backredraw) {
			vga = vga_state.vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_state.vga_height;y++) {
				for (x=0;x < vga_state.vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        Write tests ");
			sprintf(tmp,"@%X",fdc->base_io);
			vga_write(tmp);
			if (fdc->irq >= 0) {
				sprintf(tmp," IRQ %d",fdc->irq);
				vga_write(tmp);
			}
			if (fdc->dma >= 0) {
				sprintf(tmp," DMA %d",fdc->dma);
				vga_write(tmp);
			}
			if (floppy_dma != NULL) {
				sprintf(tmp," phys=%08lxh len=%04lxh",(unsigned long)floppy_dma->phys,(unsigned long)floppy_dma->length);
				vga_write(tmp);
			}
			while (vga_state.vga_pos_x < vga_state.vga_width && vga_state.vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			redraw = 0;

			floppy_controller_read_status(fdc);
			floppy_controller_read_DIR(fdc);
			floppy_controller_read_ps2_status(fdc);

			y = 2;
			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"DOR %02xh DIR %02xh Stat %02xh CCR %02xh cyl=%-3u",
				fdc->digital_out,fdc->digital_in,
				fdc->main_status,fdc->control_cfg,
				fdc->cylinder);
			vga_write(tmp);

			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"ST0..3: %02x %02x %02x %02x PS/2 %02x %02x",
				fdc->st[0],fdc->st[1],fdc->st[2],fdc->st[3],
				fdc->ps2_status[0],fdc->ps2_status[1]);
			vga_write(tmp);

			y = 5;
			vga_moveto(8,y++);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("FDC menu");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');
			y++;

			vga_moveto(8,y++);
			vga_write_color((select == 0) ? 0x70 : 0x0F);
			vga_write("Write sector");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			if (select == -1) {
				break;
			}
			else if (select == 0) { /* Write Sector */
				do_floppy_write_test(fdc);
				backredraw = 1;
			}
		}
		else if (c == 0x4800) {
			if (--select < -1)
				select = 0;

			redraw = 1;
		}
		else if (c == 0x5000) {
			if (++select > 0)
				select = -1;

			redraw = 1;
		}
	}
}

void prompt_position() {
	uint64_t tmp;
	int en_mfm,en_mt;
	int cyl,head,phead,sect,ssz,sszm,scount,gaprw,gapfmt;
	struct vga_msg_box box;
	unsigned char redraw=1;
	unsigned char ok=1;
	char temp_str[64];
	int select=0;
	int c,i=0;

	en_mfm = mfm_mode;
	en_mt = allow_multitrack;
	sszm = current_sectsize_smaller;
	gapfmt = current_phys_fmt_gap;
	gaprw = current_phys_rw_gap;
	scount = current_sectcount;
	phead = current_phys_head;
	ssz = current_sectsize_p2;
	cyl = current_log_track;
	sect = current_log_sect;
	head = current_log_head;

	vga_msg_box_create(&box,
		"Edit position:                  ",
		2+10,0);
	while (1) {
		char recalc = 0;
		char rekey = 0;

		if (redraw) {
			redraw = 0;

			vga_moveto(box.x+2,box.y+2+1 + 0);
			vga_write_color(0x1E);
			vga_write("Cylinder:  ");
			vga_write_color(select == 0 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%u",cyl);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 1);
			vga_write_color(0x1E);
			vga_write("Head:      ");
			vga_write_color(select == 1 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%u",head);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 2);
			vga_write_color(0x1E);
			vga_write("Sector:    ");
			vga_write_color(select == 2 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%u",sect);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 3);
			vga_write_color(0x1E);
			vga_write("Sect. size:");
			vga_write_color(select == 3 ? 0x70 : 0x1E);
			if (ssz > 0)
				i=sprintf(temp_str,"%u",128 << ssz);
			else
				i=sprintf(temp_str,"%u",sszm);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 4);
			vga_write_color(0x1E);
			vga_write("Phys. Head:");
			vga_write_color(select == 4 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%u",phead);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 5);
			vga_write_color(0x1E);
			vga_write("Sect count:");
			vga_write_color(select == 5 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%u",scount);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 6);
			vga_write_color(0x1E);
			vga_write("Multitrack:");
			vga_write_color(select == 6 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%s",en_mt?"On":"Off");
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 7);
			vga_write_color(0x1E);
			vga_write("FM/MFM:    ");
			vga_write_color(select == 7 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%s",en_mfm?"MFM":"FM");
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 8);
			vga_write_color(0x1E);
			vga_write("R/W gap:   ");
			vga_write_color(select == 8 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%u",gaprw);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 9);
			vga_write_color(0x1E);
			vga_write("Fmt gap:   ");
			vga_write_color(select == 9 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%u",gapfmt);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);
		}

		c = getch();
		if (c == 0) c = getch() << 8;

nextkey:	if (c == 27) {
			ok = 0;
			break;
		}
		else if (c == 13) {
			ok = 1;
			break;
		}
		else if (c == 0x4800) {
			if (--select < 0) select = 9;
			redraw = 1;
		}
		else if (c == 0x5000 || c == 9/*tab*/) {
			if (++select > 9) select = 0;
			redraw = 1;
		}

		else if (c == 0x4B00) { /* left */
			switch (select) {
				case 0:
					if (cyl == 0) cyl = 255;
					else cyl--;
					break;
				case 1:
					if (head == 0) head = 255;
					else head--;
					break;
				case 2:
					if (sect == 0) sect = 255;
					else sect--;
					break;
				case 3:
					if (ssz == 0) {
						if (sszm <= 1) {
							sszm = 0xFF;
							ssz = 7;
						}
						else {
							sszm--;
						}
					}
					else {
						ssz--;
					}
					break;
				case 4:
					if (phead == 0) phead = 1;
					else phead--;
					break;
				case 5:
					if (scount <= 1) scount = 256;
					else scount--;
					break;
				case 6:
					en_mt = !en_mt;
					break;
				case 7:
					en_mfm = !en_mfm;
					break;
				case 8:
					if (gaprw == 0) gaprw = 255;
					else gaprw--;
					break;
				case 9:
					if (gapfmt == 0) gapfmt = 255;
					else gapfmt--;
					break;
			};

			recalc = 1;
			redraw = 1;
		}
		else if (c == 0x4D00) { /* right */
			switch (select) {
				case 0:
					if ((++cyl) >= 256) cyl = 0;
					break;
				case 1:
					if ((++head) >= 256) head = 0;
					break;
				case 2:
					if ((++sect) >= 256) sect = 0;
					break;
				case 3:
					if (ssz == 0) {
						if ((++sszm) > 255) {
							sszm = 0xFF;
							ssz = 1;
						}
					}
					else {
						if ((++ssz) > 7) {
							ssz = 0;
							sszm = 1;
						}
					}
					break;
				case 4:
					if ((++phead) >= 2) phead = 0;
					break;
				case 5:
					if ((++scount) >= 257) scount = 1;
					break;
				case 6:
					en_mt = !en_mt;
					break;
				case 7:
					en_mfm = !en_mfm;
					break;
				case 8:
					if (gaprw == 255) gaprw = 0;
					else gaprw++;
					break;
				case 9:
					if (gapfmt == 255) gapfmt = 0;
					else gapfmt++;
					break;
			};

			recalc = 1;
			redraw = 1;
		}

		else if ((c == 8 || isdigit(c)) && (select != 6 && select != 7)) {
			unsigned int sy = box.y+2+1 + select;
			unsigned int sx = box.x+2+11;

			switch (select) {
				case 0:	sprintf(temp_str,"%u",cyl); break;
				case 1:	sprintf(temp_str,"%u",head); break;
				case 2:	sprintf(temp_str,"%u",sect); break;
				case 3: sprintf(temp_str,"%u",ssz != 0 ? (128 << ssz) : sszm); break;
				case 4:	sprintf(temp_str,"%u",phead); break;
				case 5:	sprintf(temp_str,"%u",scount); break;
				case 8:	sprintf(temp_str,"%u",gaprw); break;
				case 9:	sprintf(temp_str,"%u",gapfmt); break;
			}

			if (c == 8) {
				i = strlen(temp_str) - 1;
				if (i < 0) i = 0;
				temp_str[i] = 0;
			}
			else {
				i = strlen(temp_str);
				if (i == 1 && temp_str[0] == '0') i--;
				if ((i+2) < sizeof(temp_str)) {
					temp_str[i++] = (char)c;
					temp_str[i] = 0;
				}
			}

			redraw = 1;
			while (1) {
				if (redraw) {
					redraw = 0;
					vga_moveto(sx,sy);
					vga_write_color(0x70);
					vga_write(temp_str);
					while (vga_state.vga_pos_x < (box.x+box.w-4) && vga_state.vga_pos_x != 0) vga_writec(' ');
				}

				c = getch();
				if (c == 0) c = getch() << 8;

				if (c == 8) {
					if (i > 0) {
						temp_str[--i] = 0;
						redraw = 1;
					}
				}
				else if (isdigit(c)) {
					if ((i+2) < sizeof(temp_str)) {
						temp_str[i++] = (char)c;
						temp_str[i] = 0;
						redraw = 1;
					}
				}
				else {
					break;
				}
			}

			switch (select) {
				case 0:	tmp=strtoull(temp_str,NULL,0);  cyl=(tmp > 255ULL ? 255ULL : tmp); break;
				case 1:	tmp=strtoull(temp_str,NULL,0); head=(tmp > 255ULL ? 255ULL : tmp); break;
				case 2:	tmp=strtoull(temp_str,NULL,0); sect=(tmp > 255ULL ? 255ULL : tmp); break;
				case 3:
					tmp=strtoull(temp_str,NULL,0);
					if (tmp > 16384ULL) tmp = 16384ULL;
					if (tmp >= 256UL) {
						ssz=1; /* 256 */
						while ((128<<ssz) < (unsigned int)tmp) ssz++;
					}
					else {
						ssz=0;
						sszm=(unsigned int)tmp;
					}
					break;
				case 4:	tmp=strtoull(temp_str,NULL,0); phead=(tmp >  1ULL ?   1ULL : tmp); break;
				case 5:	tmp=strtoull(temp_str,NULL,0); scount=(tmp > 256ULL ? 256ULL : tmp);
					if (scount == 0) scount = 1;
					break;
				case 8:	tmp=strtoull(temp_str,NULL,0); gaprw=(tmp > 255ULL ? 255ULL : tmp); break;
				case 9:	tmp=strtoull(temp_str,NULL,0); gapfmt=(tmp > 255ULL ? 255ULL : tmp); break;
			}

			rekey = 1;
			recalc = 1;
		}

		if (rekey) {
			rekey = 0;
			goto nextkey;
		}
	}
	vga_msg_box_destroy(&box);

	if (ok) {
		mfm_mode = en_mfm;
		allow_multitrack = en_mt;
		current_sectsize_smaller = sszm;
		current_phys_fmt_gap = gapfmt;
		current_phys_rw_gap = gaprw;
		current_sectcount = scount;
		current_phys_head = phead;
		current_sectsize_p2 = ssz;
		current_log_track = cyl;
		current_log_sect = sect;
		current_log_head = head;
	}
}

void do_presets(struct floppy_controller *fdc) {
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	char redraw=1;
	int select=-1;
	int c;

	while (1) {
		if (backredraw) {
			vga = vga_state.vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_state.vga_height;y++) {
				for (x=0;x < vga_state.vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        Floppy format presets ");
			sprintf(tmp,"@%X",fdc->base_io);
			vga_write(tmp);
			if (fdc->irq >= 0) {
				sprintf(tmp," IRQ %d",fdc->irq);
				vga_write(tmp);
			}
			if (fdc->dma >= 0) {
				sprintf(tmp," DMA %d",fdc->dma);
				vga_write(tmp);
			}
			if (floppy_dma != NULL) {
				sprintf(tmp," phys=%08lxh len=%04lxh",(unsigned long)floppy_dma->phys,(unsigned long)floppy_dma->length);
				vga_write(tmp);
			}
			while (vga_state.vga_pos_x < vga_state.vga_width && vga_state.vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			redraw = 0;

			y = 5;
			vga_moveto(8,y++);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("Back");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');
			y++;

			vga_moveto(8,y++);
			vga_write_color((select == 0) ? 0x70 : 0x0F);
			vga_write("1.44MB 3.5\" high density");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 1) ? 0x70 : 0x0F);
			vga_write("720KB 3.5\" high density");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 2) ? 0x70 : 0x0F);
			vga_write("1.2MB 5.25\" high density");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 3) ? 0x70 : 0x0F);
			vga_write("360KB 5.25\" double density");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 4) ? 0x70 : 0x0F);
			vga_write("180KB 5.25\" double density");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			if (select == -1) {
				break;
			}
			else if (select == 0) { /* 1.44MB 3.5" */
				floppy_controller_set_data_transfer_rate(fdc,0/*500kb*/);
				break;
			}
			else if (select == 1) { /* 720KB 3.5" */
				floppy_controller_set_data_transfer_rate(fdc,2/*250kb*/);
				break;
			}
			else if (select == 2) { /* 1.2MB 5.25" */
				floppy_controller_set_data_transfer_rate(fdc,0/*500kb*/);
				break;
			}
			else if (select == 3) { /* 360KB 5.25" */
				floppy_controller_set_data_transfer_rate(fdc,1/*300kb*/);
				break;
			}
			else if (select == 4) { /* 180KB 5.25" */
				floppy_controller_set_data_transfer_rate(fdc,1/*300kb*/);
				break;
			}
		}
		else if (c == 0x4800) {
			if (--select < -1)
				select = 4;

			redraw = 1;
		}
		else if (c == 0x5000) {
			if (++select > 4)
				select = -1;

			redraw = 1;
		}
	}
}

void do_floppy_controller(struct floppy_controller *fdc) {
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	char redraw=1;
	int select=-1;
	int c;

	/* and allocate DMA too */
	if (fdc->dma >= 0 && floppy_dma == NULL) {
#if TARGET_MSDOS == 32
		uint32_t choice = 65536;
#else
		uint32_t choice = 32768;
#endif

		do {
			floppy_dma = dma_8237_alloc_buffer(choice);
			if (floppy_dma == NULL) choice -= 4096UL;
		} while (floppy_dma == NULL && choice > 4096UL);

		if (floppy_dma == NULL) return;
	}

	/* if the floppy struct says to use interrupts, then do it */
	do_floppy_controller_enable_irq(fdc,fdc->use_irq);
	floppy_controller_enable_dma(fdc,fdc->use_dma);

	while (1) {
		if (backredraw) {
			vga = vga_state.vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_state.vga_height;y++) {
				for (x=0;x < vga_state.vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        Floppy controller ");
			sprintf(tmp,"@%X",fdc->base_io);
			vga_write(tmp);
			if (fdc->irq >= 0) {
				sprintf(tmp," IRQ %d",fdc->irq);
				vga_write(tmp);
			}
			if (fdc->dma >= 0) {
				sprintf(tmp," DMA %d",fdc->dma);
				vga_write(tmp);
			}
			if (floppy_dma != NULL) {
				sprintf(tmp," phys=%08lxh len=%04lxh",(unsigned long)floppy_dma->phys,(unsigned long)floppy_dma->length);
				vga_write(tmp);
			}
			while (vga_state.vga_pos_x < vga_state.vga_width && vga_state.vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			int cx = vga_state.vga_width / 2;

			redraw = 0;

			floppy_controller_read_status(fdc);
			floppy_controller_read_DIR(fdc);
			floppy_controller_read_ps2_status(fdc);

			y = 2;
			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"DOR %02xh DIR %02xh Stat %02xh CCR %02xh cyl=%-3u ver=%02xh",
				fdc->digital_out,fdc->digital_in,
				fdc->main_status,fdc->control_cfg,
				fdc->cylinder,   fdc->version);
			vga_write(tmp);

			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"ST0..3: %02x %02x %02x %02x PS/2 %02x %02x",
				fdc->st[0],fdc->st[1],fdc->st[2],fdc->st[3],
				fdc->ps2_status[0],fdc->ps2_status[1]);
			vga_write(tmp);

			y = 5;
			vga_moveto(8,y++);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("Main menu");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');
			y++;

			vga_moveto(8,y++);
			vga_write_color((select == 0) ? 0x70 : 0x0F);
			vga_write("DMA: ");
			vga_write(fdc->use_dma ? "Enabled" : "Disabled");
			while (vga_state.vga_pos_x < cx && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_write_color((select == 1) ? 0x70 : 0x0F);
			vga_write("IRQ: ");
			vga_write(fdc->use_irq ? "Enabled" : "Disabled");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 2) ? 0x70 : 0x0F);
			vga_write("Drive A motor: ");
			vga_write((fdc->digital_out&0x10) ? "On" : "Off");
			while (vga_state.vga_pos_x < cx && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_write_color((select == 3) ? 0x70 : 0x0F);
			vga_write("Drive B motor: ");
			vga_write((fdc->digital_out&0x20) ? "On" : "Off");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 4) ? 0x70 : 0x0F);
			vga_write("Drive C motor: ");
			vga_write((fdc->digital_out&0x40) ? "On" : "Off");
			while (vga_state.vga_pos_x < cx && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_write_color((select == 5) ? 0x70 : 0x0F);
			vga_write("Drive D motor: ");
			vga_write((fdc->digital_out&0x80) ? "On" : "Off");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 6) ? 0x70 : 0x0F);
			vga_write("Drive select: ");
			sprintf(tmp,"%c(%u)",(fdc->digital_out&3)+'A',fdc->digital_out&3);
			vga_write(tmp);
			while (vga_state.vga_pos_x < cx && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_write_color((select == 7) ? 0x70 : 0x0F);
			vga_write("Data rate: ");
			switch (fdc->control_cfg&3) {
				case 0:	vga_write("500kbit/sec"); break;
				case 1:	vga_write("300kbit/sec"); break;
				case 2:	vga_write("250kbit/sec"); break;
				case 3:	vga_write("1mbit/sec"); break;
			};
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 8) ? 0x70 : 0x0F);
			vga_write("Reset signal: ");
			vga_write((fdc->digital_out&0x04) ? "Off" : "On");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 9) ? 0x70 : 0x0F);
			vga_write("Pos log C/H/S/sz/cnt: ");
			sprintf(tmp,"%u/%u/%u/%u/%u  phyH=%u  MT=%u %s",
				current_log_track,
				current_log_head,
				current_log_sect,
				current_sectsize_p2 > 0 ? (128 << current_sectsize_p2) : current_sectsize_smaller,
				current_sectcount,
				current_phys_head,
				allow_multitrack,
				mfm_mode?"MFM":"FM");

			vga_write(tmp);
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');
			y++;

			vga_moveto(8,y++);
			vga_write_color((select == 10) ? 0x70 : 0x0F);
			vga_write("Check drive status (x4h)");
			while (vga_state.vga_pos_x < cx && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_write_color((select == 11) ? 0x70 : 0x0F);
			vga_write("Calibrate (x7h)");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 12) ? 0x70 : 0x0F);
			vga_write("Seek (xFh)");
			while (vga_state.vga_pos_x < cx && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_write_color((select == 13) ? 0x70 : 0x0F);
			vga_write("Seek relative (1xFh)");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 14) ? 0x70 : 0x0F);
			vga_write("Read sector ID (xAh)");
			while (vga_state.vga_pos_x < cx && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_write_color((select == 15) ? 0x70 : 0x0F);
			vga_write("Read testing >>");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 16) ? 0x70 : 0x0F);
			vga_write("Write testing >>");
			while (vga_state.vga_pos_x < cx && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_write_color((select == 17) ? 0x70 : 0x0F);
			vga_write("Dump Registers (xEh)");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 18) ? 0x70 : 0x0F);
			vga_write("Get Version (1x0h)");
			while (vga_state.vga_pos_x < cx && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_write_color((select == 19) ? 0x70 : 0x0F);
			vga_write("Format Track (xDh)");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 20) ? 0x70 : 0x0F);
			vga_write("Step through tracks");
			while (vga_state.vga_pos_x < cx && vga_state.vga_pos_x != 0) vga_writec(' ');

			vga_write_color((select == 21) ? 0x70 : 0x0F);
			vga_write("Preset...");
			while (vga_state.vga_pos_x < (vga_state.vga_width-8) && vga_state.vga_pos_x != 0) vga_writec(' ');
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
        else if (c == 'm') {
            const unsigned char drv = (fdc->digital_out&3);

            floppy_controller_set_motor_state(fdc,drv,!(fdc->digital_out&(0x10<<drv)));
            redraw = 1;
        }
        else if (c == 'p') {
            /* presets */
            do_presets(fdc);
            backredraw = 1;
        }
		else if (c == 13) {
			if (select == -1) {
				break;
			}
			else if (select == 0) { /* DMA enable */
				fdc->non_dma_mode = !fdc->non_dma_mode;
				if (do_floppy_controller_specify(fdc)) /* if specify fails, don't change DMA flag */
					floppy_controller_enable_dma(fdc,!fdc->non_dma_mode); /* enable DMA = !(non-dma-mode) */
				else
					fdc->non_dma_mode = !fdc->non_dma_mode;

				redraw = 1;
			}
			else if (select == 1) { /* IRQ enable */
				do_floppy_controller_enable_irq(fdc,!fdc->use_irq);
				redraw = 1;
			}
			else if (select == 2) { /* Drive A motor */
				floppy_controller_set_motor_state(fdc,0/*A*/,!(fdc->digital_out&0x10));
				redraw = 1;
			}
			else if (select == 3) { /* Drive B motor */
				floppy_controller_set_motor_state(fdc,1/*B*/,!(fdc->digital_out&0x20));
				redraw = 1;
			}
			else if (select == 4) { /* Drive C motor */
				floppy_controller_set_motor_state(fdc,2/*C*/,!(fdc->digital_out&0x40));
				redraw = 1;
			}
			else if (select == 5) { /* Drive D motor */
				floppy_controller_set_motor_state(fdc,3/*D*/,!(fdc->digital_out&0x80));
				redraw = 1;
			}
			else if (select == 6) { /* Drive select */
				floppy_controller_drive_select(fdc,((fdc->digital_out&3)+1)&3);
				redraw = 1;
			}
			else if (select == 7) { /* Transfer rate */
				floppy_controller_set_data_transfer_rate(fdc,((fdc->control_cfg&3)+1)&3);
				redraw = 1;
			}
			else if (select == 8) { /* reset */
				floppy_controller_set_reset(fdc,!!(fdc->digital_out&0x04)); /* bit is INVERTED 1=normal 0=reset */
				redraw = 1;
			}
			else if (select == 9) { /* Position */
				prompt_position();
				redraw = 1;
			}
			else if (select == 10) { /* check drive status */
				do_check_drive_status(fdc);
				redraw = 1;
			}
			else if (select == 11) { /* calibrate drive */
				do_calibrate_drive(fdc);
				redraw = 1;
			}
			else if (select == 12) { /* seek */
				unsigned long track = prompt_track_number();
				if (track != (~0UL) && track < 256) {
					do_seek_drive(fdc,(uint8_t)track);
					redraw = 1;
				}
			}
			else if (select == 13) { /* seek relative */
				signed long track = prompt_signed_track_number();
				if (track >= -255L && track <= 255L) {
					do_seek_drive_rel(fdc,(int)track);
					redraw = 1;
				}
			}
			else if (select == 14) { /* read sector ID */
				do_read_sector_id_demo(fdc);
				backredraw = 1;
			}
			else if (select == 15) { /* read testing */
				do_floppy_controller_read_tests(fdc);
				backredraw = 1;
			}
			else if (select == 16) { /* write testing */
				do_floppy_controller_write_tests(fdc);
				backredraw = 1;
			}
			else if (select == 17) { /* dump registers */
				do_floppy_dumpreg(fdc);
			}
			else if (select == 18) { /* get version */
				do_floppy_get_version(fdc);
				redraw = 1;
			}
			else if (select == 19) { /* format track */
				do_floppy_format_track(fdc);
				backredraw = 1;
			}
			else if (select == 20) { /* step through tracks */
				do_step_tracks(fdc);
				backredraw = 1;
			}
            else if (select == 21) { /* presets menu */
                do_presets(fdc);
                backredraw = 1;
            }
		}
		else if (c == 0x4800) {
			if (--select < -1)
				select = 21;

			redraw = 1;
		}
		else if (c == 0x5000) {
			if (++select > 21)
				select = -1;

			redraw = 1;
		}
	}

	if (floppy_dma != NULL) {
		dma_8237_free_buffer(floppy_dma);
		floppy_dma = NULL;
	}

    /* motor off */
    for (y=0;y < 4;y++) floppy_controller_set_motor_state(fdc,y,0);

	do_floppy_controller_enable_irq(fdc,0);
	floppy_controller_enable_irqdma_gate_otr(fdc,1); /* because BIOSes probably won't */
	p8259_unmask(fdc->irq);
}

void do_main_menu() {
	char redraw=1;
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	struct floppy_controller *floppy;
	unsigned int x,y,i;
	int select=-1;
	int c;

    my_irq0_old_irq = _dos_getvect(irq2int(0));
    _dos_setvect(irq2int(0),my_irq0);

	while (1) {
		if (backredraw) {
			vga = vga_state.vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_state.vga_height;y++) {
				for (x=0;x < vga_state.vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        Floppy controller test program");
			while (vga_state.vga_pos_x < vga_state.vga_width && vga_state.vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			redraw = 0;

			y = 5;
			vga_moveto(8,y++);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("Exit program");
			y++;

			for (i=0;i < MAX_FLOPPY_CONTROLLER;i++) {
				floppy = floppy_get_controller(i);
				if (floppy != NULL) {
					vga_moveto(8,y++);
					vga_write_color((select == (int)i) ? 0x70 : 0x0F);

					sprintf(tmp,"Controller @ %04X",floppy->base_io);
					vga_write(tmp);

					if (floppy->irq >= 0) {
						sprintf(tmp," IRQ %2d",floppy->irq);
						vga_write(tmp);
					}

					if (floppy->dma >= 0) {
						sprintf(tmp," DMA %2d",floppy->dma);
						vga_write(tmp);
					}
				}
			}
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			if (select == -1) {
				break;
			}
			else if (select >= 0 && select < MAX_FLOPPY_CONTROLLER) {
				floppy = floppy_get_controller(select);
				if (floppy != NULL) do_floppy_controller(floppy);
				backredraw = redraw = 1;
			}
		}
		else if (c == 0x4800) {
			if (select <= -1)
				select = MAX_FLOPPY_CONTROLLER - 1;
			else
				select--;

			while (select >= 0 && floppy_get_controller(select) == NULL)
				select--;

			redraw = 1;
		}
		else if (c == 0x5000) {
			select++;
			while (select >= 0 && select < MAX_FLOPPY_CONTROLLER && floppy_get_controller(select) == NULL)
				select++;
			if (select >= MAX_FLOPPY_CONTROLLER)
				select = -1;

			redraw = 1;
		}
	}

    /* restore IRQ */
    _dos_setvect(irq2int(0),my_irq0_old_irq);
}

static void help(void) {
    fprintf(stderr,"test [options]\n");
    fprintf(stderr," -h --help     Show this help\n");
    fprintf(stderr," -1            First controller only\n");
    fprintf(stderr," -2            Look for 2nd controller\n");
}

static int parse_argv(int argc,char **argv) {
    char *a;
    int i;

    for (i=1;i < argc;) {
        a = argv[i++];

        if (*a == '-') {
            do { a++; } while (*a == '-');

            if (!strcmp(a,"h") || !strcmp(a,"help")) {
                help();
                return 1;
            }
            else if (!strcmp(a,"1")) {
                floppy_controllers_enable_2nd = 0;
            }
            else if (!strcmp(a,"2")) {
                floppy_controllers_enable_2nd = 1;
            }
            else {
                fprintf(stderr,"Unknown switch '%s'\n",a);
                return 1;
            }
        }
        else {
            fprintf(stderr,"Unknown switch '%s'\n",a);
            return 1;
        }
    }

    return 0;
}

int main(int argc,char **argv) {
	struct floppy_controller *reffdc;
	struct floppy_controller *newfdc;
	int i;

    if (parse_argv(argc,argv))
        return 1;

	/* we take a GUI-based approach (kind of) */
	if (!probe_vga()) {
		printf("Cannot init VGA\n");
		return 1;
	}
	if (!probe_8237())
		printf("WARNING: Cannot init 8237 DMA\n");
	/* the floppy code has some timing requirements and we'll use the 8254 to do it */
	/* newer motherboards don't even have a floppy controller and it's probable they'll stop implementing the 8254 at some point too */
	if (!probe_8254()) {
		printf("8254 chip not detected\n");
		return 1;
	}
	if (!probe_8259()) {
		printf("8259 chip not detected\n");
		return 1;
	}
	if (!init_floppy_controller_lib()) {
		printf("Failed to init floppy controller\n");
		return 1;
	}

	printf("Probing standard FDC ports...\n");
	for (i=0;(reffdc = (struct floppy_controller*)floppy_get_standard_isa_port(i)) != NULL;i++) {
		printf("   %3X IRQ %d DMA %d: ",reffdc->base_io,reffdc->irq,reffdc->dma); fflush(stdout);

		if ((newfdc = floppy_controller_probe(reffdc)) != NULL) {
			printf("FOUND. PS/2=%u AT=%u dma=%u DOR/RW=%u DOR=%02xh DIR=%02xh mst=%02xh\n",
				newfdc->ps2_mode,
				newfdc->at_mode,
				newfdc->use_dma,
				newfdc->digital_out_rw,
				newfdc->digital_out,
				newfdc->digital_in,
				newfdc->main_status);
		}
		else {
			printf("\x0D                             \x0D"); fflush(stdout);
		}
	}

	printf("Hit ENTER to continue, ESC to cancel\n");
	i = wait_for_enter_or_escape();
	if (i == 27) {
		free_floppy_controller_lib();
		return 0;
	}

	do_main_menu();
	free_floppy_controller_lib();
	return 0;
}

