/*
 * fiwix/drivers/char/console.c
 *
 * Copyright 2018-2021, Jordi Sanfeliu. All rights reserved.
 * Distributed under the terms of the Fiwix License.
 */

#include <fiwix/asm.h>
#include <fiwix/kernel.h>
#include <fiwix/ctype.h>
#include <fiwix/console.h>
#include <fiwix/devices.h>
#include <fiwix/tty.h>
#include <fiwix/keyboard.h>
#include <fiwix/sleep.h>
#include <fiwix/pit.h>
#include <fiwix/timer.h>
#include <fiwix/process.h>
#include <fiwix/mm.h>
#include <fiwix/sched.h>
#include <fiwix/kd.h>
#include <fiwix/stdio.h>
#include <fiwix/string.h>
#include <fiwix/fbcon.h>

#define CSI_J_CUR2END	0	/* clear from cursor to end of screen */
#define CSI_J_STA2CUR	1	/* clear from start of screen to cursor */
#define CSI_J_SCREEN	2	/* clear entire screen */

#define CSI_K_CUR2END	0	/* clear from cursor to end of line */
#define CSI_K_STA2CUR	1	/* clear from start of line to cursor */
#define CSI_K_LINE	2	/* clear entire line */

#define CSE		vc->esc = 0	/* Code Set End */

/* VT100 ID string generated by <ESC>Z or <ESC>[c */
#define VT100ID		"\033[?1;2c"

/* VT100 report status generated by <ESC>[5n */
#define DEVICE_OK	"\033[0n"
#define DEVICE_NOT_OK	"\033[3n"

#define SCREEN_SIZE	(video.columns * video.lines)
#define VC_BUF_LINES	(video.lines * SCREENS_LOG)


short int current_cons;

struct video_parms video;
struct vconsole vc[NR_VCONSOLES + 1];

static struct fs_operations tty_driver_fsop = {
	0,
	0,

	tty_open,
	tty_close,
	tty_read,
	tty_write,
	tty_ioctl,
	tty_lseek,
	NULL,			/* readdir */
	NULL,			/* mmap */
	tty_select,

	NULL,			/* readlink */
	NULL,			/* followlink */
	NULL,			/* bmap */
	NULL,			/* lookup */
	NULL,			/* rmdir */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* mknod */
	NULL,			/* truncate */
	NULL,			/* create */
	NULL,			/* rename */

	NULL,			/* read_block */
	NULL,			/* write_block */

	NULL,			/* read_inode */
	NULL,			/* write_inode */
	NULL,			/* ialloc */
	NULL,			/* ifree */
	NULL,			/* statfs */
	NULL,			/* read_superblock */
	NULL,			/* remount_fs */
	NULL,			/* write_superblock */
	NULL			/* release_superblock */
};

static struct device tty_device = {
	"vconsole",
	VCONSOLES_MAJOR,
	{ 0, 0, 0, 0, 0, 0, 0, 0 },
	0,
	NULL,
	&tty_driver_fsop,
	NULL
};

static struct device console_device = {
	"console",
	SYSCON_MAJOR,
	{ 0, 0, 0, 0, 0, 0, 0, 0 },
	0,
	NULL,
	&tty_driver_fsop,
	NULL
};

unsigned short int ansi_color_table[] = {
	COLOR_BLACK,
	COLOR_RED,
	COLOR_GREEN,
	COLOR_BROWN,
	COLOR_BLUE,
	COLOR_MAGENTA,
	COLOR_CYAN,
	COLOR_WHITE
};

static int is_vconsole(__dev_t dev)
{
	if(MAJOR(dev) == VCONSOLES_MAJOR && MINOR(dev) <= NR_VCONSOLES) {
		return 1;
	}

	return 0;
}

static void adjust(struct vconsole *vc, int x, int y)
{
	if(x < 0) {
		x = 0;
	}
	if(x >= vc->columns) {
		x = vc->columns - 1;
	}
	if(y < 0) {
		y = 0;
	}
	if(y >= vc->lines) {
		y = vc->lines - 1;
	}
	vc->x = x;
	vc->y = y;
}

static void cr(struct vconsole *vc)
{
	vc->x = 0;
}

static void lf(struct vconsole *vc)
{
	if(vc->y == vc->lines) {
		video.scroll_screen(vc, 0, SCROLL_UP);
	} else {
		vc->y++;
	}
}

static void ri(struct vconsole *vc)
{
	if(vc->y == 0) {
		video.scroll_screen(vc, 0, SCROLL_DOWN);
	} else {
		vc->y--;
	}
}

static void csi_J(struct vconsole *vc, int mode)
{
	int from, count;

	switch(mode) {
		case CSI_J_CUR2END:	/* Erase Down <ESC>[J */
			from = (vc->y * vc->columns) + vc->x;
			count = vc->columns - vc->x;
			video.write_screen(vc, from, count, vc->color_attr);
			from = ((vc->y + 1) * vc->columns);
			count = SCREEN_SIZE - from;
			break;
		case CSI_J_STA2CUR:	/* Erase Up <ESC>[1J */
			from = vc->y * vc->columns;
			count = vc->x + 1;
			video.write_screen(vc, from, count, vc->color_attr);
			from = 0;
			count = vc->y * vc->columns;
			break;
		case CSI_J_SCREEN:	/* Erase Screen <ESC>[2J */
			from = 0;
			count = SCREEN_SIZE;
			break;
		default:
			return;
	}
	video.write_screen(vc, from, count, vc->color_attr);
}

static void csi_K(struct vconsole *vc, int mode)
{
	int from, count;

	switch(mode) {
		case CSI_K_CUR2END:	/* Erase End of Line <ESC>[K */
			from = (vc->y * vc->columns) + vc->x;
			count = vc->columns - vc->x;
			break;
		case CSI_K_STA2CUR:	/* Erase Start of Line <ESC>[1K */
			from = vc->y * vc->columns;
			count = vc->x + 1;
			break;
		case CSI_K_LINE:	/* Erase Line <ESC>[2K */
			from = vc->y * vc->columns;
			count = vc->columns;
			break;
		default:
			return;
	}
	video.write_screen(vc, from, count, vc->color_attr);
}

static void csi_X(struct vconsole *vc, int count)
{
	int from;

	from = (vc->y * vc->columns) + vc->x;
	count = count > (vc->columns - vc->x) ? vc->columns - vc->x : count;
	video.write_screen(vc, from, count, vc->color_attr);
}

static void csi_L(struct vconsole *vc, int count)
{
	if(count > (vc->lines - vc->top)) {
		count = vc->lines - vc->top;
	}
	while(count--) {
		video.scroll_screen(vc, vc->y, SCROLL_DOWN);
	}
}

static void csi_M(struct vconsole *vc, int count)
{
	if(count > (vc->lines - vc->top)) {
		count = vc->lines - vc->top;
	}
	while(count--) {
		video.scroll_screen(vc, vc->y, SCROLL_UP);
	}
}

static void csi_P(struct vconsole *vc, int count)
{
	if(count > vc->columns) {
		count = vc->columns;
	}
	while(count--) {
		video.delete_char(vc);
	}
}

static void csi_at(struct vconsole *vc, int count)
{
	if(count > vc->columns) {
		count = vc->columns;
	}
	while(count--) {
		video.insert_char(vc);
	}
}

static void default_color_attr(struct vconsole *vc)
{
	vc->color_attr = DEF_MODE;
	vc->bold = 0;
	vc->underline = 0;
	vc->blink = 0;
	vc->reverse = 0;
}

/* Select Graphic Rendition */
static void csi_m(struct vconsole *vc)
{
	int n;

	if(vc->reverse) {
		vc->color_attr = ((vc->color_attr & 0x7000) >> 4) | ((vc->color_attr & 0x0700) << 4) | (vc->color_attr & 0x8800); 
	}

	for(n = 0; n < vc->nparms; n++) {
		switch(vc->parms[n]) {
			case SGR_DEFAULT:
				default_color_attr(vc);
				break;
			case SGR_BOLD:
				vc->bold = 1;
				break;
			case SGR_BLINK:
				vc->blink = 1;
				break;
			case SGR_REVERSE:
				vc->reverse = 1;
				break;
			/* normal intensity */
			case 21:
			case 22:
				vc->bold = 0;
				break;
			case SGR_BLINK_OFF:
				vc->blink = 0;
				break;
			case SGR_REVERSE_OFF:
				vc->reverse = 0;
				break;
			case SGR_BLACK_FG:
			case SGR_RED_FG:
			case SGR_GREEN_FG:
			case SGR_BROWN_FG:
			case SGR_BLUE_FG:
			case SGR_MAGENTA_FG:
			case SGR_CYAN_FG:
			case SGR_WHITE_FG:
				vc->color_attr = (vc->color_attr & 0xF8FF) | (ansi_color_table[vc->parms[n] - 30]);
				break;
			case SGR_DEFAULT_FG_U_ON:
			case SGR_DEFAULT_FG_U_OFF:
				/* not supported yet */
				break;
			case SGR_BLACK_BG:
			case SGR_RED_BG:
			case SGR_GREEN_BG:
			case SGR_BROWN_BG:
			case SGR_BLUE_BG:
			case SGR_MAGENTA_BG:
			case SGR_CYAN_BG:
			case SGR_WHITE_BG:
				vc->color_attr = (vc->color_attr & 0x8FFF) | ((ansi_color_table[vc->parms[n] - 40]) << 4);
				break;
			case SGR_DEFAULT_BG:
				/* not supported yet */
				break;
		}
	}
	if(vc->bold) {
		vc->color_attr |= 0x0800;
	} else {
		vc->color_attr &= ~0x0800;
	}
	if(vc->blink) {
		vc->color_attr |= 0x8000;
	} else {
		vc->color_attr &= ~0x8000;
	}
	if(vc->reverse) {
		vc->color_attr = ((vc->color_attr & 0x7000) >> 4) | ((vc->color_attr & 0x0700) << 4) | (vc->color_attr & 0x8800); 
	}
}

static void init_vt(struct vconsole *vc)
{
	vc->vt_mode.mode = VT_AUTO;
	vc->vt_mode.waitv = 0;
	vc->vt_mode.relsig = 0;
	vc->vt_mode.acqsig = 0;
	vc->vt_mode.frsig = 0;
	vc->vc_mode = KD_TEXT;
	vc->tty->pid = 0;
	vc->switchto_tty = -1;
}

static void insert_seq(struct tty *tty, char *buf, int count)
{
	while(count--) {
		tty_queue_putchar(tty, &tty->read_q, *(buf++));
	}
	tty->input(tty);
}

static void vcbuf_scroll_up(void)
{
	memcpy_w(vcbuf, vcbuf + video.columns, (VC_BUF_SIZE - video.columns) * 2);
}

static void vcbuf_refresh(struct vconsole *vc)
{
	short int *screen;

	screen = (short int *)vc->screen;
	memset_w(vcbuf, BLANK_MEM, VC_BUF_SIZE);
	memcpy_w(vcbuf, screen, SCREEN_SIZE);
}

static void echo_char(struct vconsole *vc, unsigned char *buf, unsigned int count)
{
	unsigned char ch;
	unsigned long int flags;

	SAVE_FLAGS(flags); CLI();
	if(vc->flags & CONSOLE_HAS_FOCUS) {
		if(video.buf_top) {
			video.restore_screen(vc);
			video.show_cursor(vc, ON);
			video.buf_top = 0;
		}
	}

	while(count--) {
		ch = *buf++;
		if(ch == NULL) {
			continue;

		} else if(ch == '\b') {
			if(vc->x) {
				vc->x--;
			}

		} else if(ch == '\a') {
			vconsole_beep();

		} else if(ch == '\r') {
			cr(vc);

		} else if(ch == '\n') {
			cr(vc);
			vc->y++;
			if(vc->flags & CONSOLE_HAS_FOCUS) {
				video.buf_y++;
			}

		} else if(ch == '\t') {
			while(vc->x < (vc->columns - 1)) {
				if(vc->tty->tab_stop[++vc->x]) {
					break;
				}
			}
/*			vc->x += TAB_SIZE - (vc->x % TAB_SIZE); */
			vc->check_x = 1;

		} else {
			if((vc->x == vc->columns - 1) && vc->check_x) {
				vc->x = 0;
				vc->y++;
				if(vc->flags & CONSOLE_HAS_FOCUS) {
					video.buf_y++;
				}
			}
			if(vc->y >= vc->lines) {
				video.scroll_screen(vc, 0, SCROLL_UP);
				vc->y--;
			}
			video.put_char(vc, ch);
			if(vc->x < vc->columns - 1) {
				vc->check_x = 0;
				vc->x++;
			} else {
				vc->check_x = 1;
			}
		}
		if(vc->y >= vc->lines) {
			video.scroll_screen(vc, 0, SCROLL_UP);
			vc->y--;
		}
		if(vc->flags & CONSOLE_HAS_FOCUS) {
			if(video.buf_y >= VC_BUF_LINES) {
				vcbuf_scroll_up();
				video.buf_y--;
			}
		}
	}
	video.update_curpos(vc);
	RESTORE_FLAGS(flags);
}

void vconsole_reset(struct tty *tty)
{
	int n;
	struct vconsole *vc;

	vc = (struct vconsole *)tty->driver_data;

	vc->top = 0;
	vc->lines = video.lines;
	vc->columns = video.columns;
	vc->check_x = 0;
	vc->led_status = 0;
	set_leds(vc->led_status);
	vc->scrlock = vc->numlock = vc->capslock = 0;
	vc->esc = vc->sbracket = vc->semicolon = vc->question = 0;
	vc->parmv1 = vc->parmv2 = 0;
	vc->nparms = 0;
	memset_b(vc->parms, 0, sizeof(vc->parms));
	default_color_attr(vc);
	vc->insert_mode = 0;
	vc->saved_x = vc->saved_y = 0;

	for(n = 0; n < MAX_TAB_COLS; n++) {
		if(!(n % TAB_SIZE)) {
			vc->tty->tab_stop[n] = 1;
		} else {
			vc->tty->tab_stop[n] = 0;
		}
	}

	termios_reset(tty);
	vc->tty->winsize.ws_row = vc->lines - vc->top;
	vc->tty->winsize.ws_col = vc->columns;
	vc->tty->winsize.ws_xpixel = 0;
	vc->tty->winsize.ws_ypixel = 0;
	vc->tty->lnext = 0;

	init_vt(vc);
	vc->flags &= ~CONSOLE_BLANKED;
	video.update_curpos(vc);
}

void vconsole_write(struct tty *tty)
{
	int n;
	unsigned char ch;
	int numeric;
	struct vconsole *vc;

	vc = (struct vconsole *)tty->driver_data;

	if(vc->flags & CONSOLE_HAS_FOCUS) {
		if(video.buf_top) {
			video.restore_screen(vc);
			video.buf_top = 0;
			video.show_cursor(vc, ON);
			video.update_curpos(vc);
		}
	}

	ch = numeric = 0;

	while(!vc->scrlock && tty->write_q.count > 0) {
		ch = tty_queue_getchar(&tty->write_q);

		if(vc->esc) {
			if(vc->sbracket) {
				if(IS_NUMERIC(ch)) {
					numeric = 1;
					if(vc->semicolon) {
						vc->parmv2 *= 10;
						vc->parmv2 += ch - '0';
					} else {
						vc->parmv1 *= 10;
						vc->parmv1 += ch - '0';
					}
					vc->parms[vc->nparms] *= 10;
					vc->parms[vc->nparms] += ch - '0';
					continue;
				}
				switch(ch) {
					case ';':
						vc->semicolon = 1;
						vc->parmv2 = 0;
						vc->nparms++;
						continue;
					case '?':
						vc->question = 1;
						continue;
					case '@':	/* Insert Character(s) <ESC>[ n @ */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						csi_at(vc, vc->parmv1);
						CSE;
						continue;
					case 'A':	/* Cursor Up <ESC>[ n A */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						adjust(vc, vc->x, vc->y - vc->parmv1);
						CSE;
						continue;
					case 'B':	/* Cursor Down <ESC>[ n B */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						adjust(vc, vc->x, vc->y + vc->parmv1);
						CSE;
						continue;
					case 'C':	/* Cursor Forward <ESC>[ n C */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						adjust(vc, vc->x + vc->parmv1, vc->y);
						CSE;
						continue;
					case 'D':	/* Cursor Backward <ESC>[ n D */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						adjust(vc, vc->x - vc->parmv1, vc->y);
						CSE;
						continue;
					case 'E':	/* Cursor Next Line(s) <ESC>[ n E */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						adjust(vc, 0, vc->y + vc->parmv1);
						CSE;
						continue;
					case 'F':	/* Cursor Previous Line(s) <ESC>[ n F */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						adjust(vc, 0, vc->y - vc->parmv1);
						CSE;
						continue;
					case 'G':	/* Cursor Horizontal Position <ESC>[ n G */
					case '`':
						vc->parmv1 = vc->parmv1 ? vc->parmv1 - 1 : vc->parmv1;
						adjust(vc, vc->parmv1, vc->y);
						CSE;
						continue;
					case 'H':	/* Cursor Home <ESC>[ ROW ; COLUMN H */
					case 'f':	/* Horizontal Vertical Position <ESC>[ ROW ; COLUMN f */
						vc->parmv1 = vc->parmv1 ? vc->parmv1 - 1 : vc->parmv1;
						vc->parmv2 = vc->parmv2 ? vc->parmv2 - 1 : vc->parmv2;
						adjust(vc, vc->parmv2, vc->parmv1);
						CSE;
						continue;
					case 'I':	/* Cursor Forward Tabulation <ESC>[ n I */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						while(vc->parmv1--){
							while(vc->x < (vc->columns - 1)) {
								if(vc->tty->tab_stop[++vc->x]) {
									break;
								}
							}
						}
						adjust(vc, vc->x, vc->y);
						CSE;
						continue;
					case 'J':	/* Erase (Down/Up/Screen) <ESC>[J */
						csi_J(vc, vc->parmv1);
						CSE;
						continue;
					case 'K':	/* Erase (End of/Start of/) Line <ESC>[K */
						csi_K(vc, vc->parmv1);
						CSE;
						continue;
					case 'L':	/* Insert Line(s) <ESC>[ n L */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						csi_L(vc, vc->parmv1);
						CSE;
						continue;
					case 'M':	/* Delete Line(s) <ESC>[ n M */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						csi_M(vc, vc->parmv1);
						CSE;
						continue;
					case 'P':	/* Delete Character(s) <ESC>[ n P */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						csi_P(vc, vc->parmv1);
						CSE;
						continue;
					case 'S':	/* Scroll Up <ESC>[ n S */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						while(vc->parmv1--) {
							video.scroll_screen(vc, 0, SCROLL_UP);
						}
						CSE;
						continue;
					case 'T':	/* Scroll Down <ESC>[ n T */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						while(vc->parmv1--) {
							video.scroll_screen(vc, 0, SCROLL_DOWN);
						}
						CSE;
						continue;
					case 'X':	/* Erase Character(s) <ESC>[ n X */
						vc->parmv1 = !vc->parmv1 ? 1 : vc->parmv1;
						csi_X(vc, vc->parmv1);
						CSE;
						continue;
					case 'c':	/* Query Device Code <ESC>[c */
						if(!numeric) {
							insert_seq(tty, VT100ID, 7);
						}
						CSE;
						continue;
					case 'd':	/* Cursor Vertical Position <ESC>[ n d */
						vc->parmv1 = vc->parmv1 ? vc->parmv1 - 1 : vc->parmv1;
						adjust(vc, vc->x, vc->parmv1);
						CSE;
						continue;
					case 'g':
						switch(vc->parmv1) {
							case 0:	/* Clear Tab <ESC>[g */
								vc->tty->tab_stop[vc->x] = 0;
								break;
							case 3:	/* Clear All Tabs <ESC>[3g */
							case 5:	/* Clear All Tabs <ESC>[5g */
								for(n = 0; n < MAX_TAB_COLS; n++)
									vc->tty->tab_stop[n] = 0;
								break;
						}
						CSE;
						continue;
					case 'h':
						if(vc->question) {
							switch(vc->parmv1) {
								/* DEC modes */
								case 25: /* Switch Cursor Visible <ESC>[?25h */
									video.show_cursor(vc, ON);
									break;
								case 4:
									vc->insert_mode = ON; /* not used */
									break;
							}
						}
						CSE;
						continue;
					case 'l':
						if(vc->question) {
							switch(vc->parmv1) {
								/* DEC modes */
								case 25: /* Switch Cursor Invisible <ESC>[?25l */
									video.show_cursor(vc, OFF);
									break;
								case 4:
									vc->insert_mode = OFF; /* not used */
									break;
							}
						}
						CSE;
						continue;
					case 'm':	/* Select Graphic Rendition <ESC> n ... m */
						vc->nparms++;
						csi_m(vc);
						CSE;
						continue;
					case 'n':
						if(!vc->question) {
							switch(vc->parmv1) {
								case 5:	/* Query Device Status <ESC>[5n */
									insert_seq(tty, DEVICE_OK, 4);
									break;
								case 6:	/* Query Cursor Position <ESC>[6n */
									{
										char curpos[8];
										char len;
										len = sprintk(curpos, "\033[%d;%dR", vc->y, vc->x);
										insert_seq(tty, curpos, len);
									}
									break;
							}
						}
						CSE;
						continue;
					case 'r':	/* Top and Bottom Margins <ESC>[r  / <ESC>[{start};{end}r */
						if(!vc->parmv1) {
							vc->parmv1++;
						}
						if(!vc->parmv2) {
							vc->parmv2 = video.lines;
						}
						if(vc->parmv1 < vc->parmv2 && vc->parmv2 <= video.lines) {
							vc->top = vc->parmv1 - 1;
							vc->lines = vc->parmv2;
							adjust(vc, 0, 0);
						}
						CSE;
						continue;
					case 's':	/* Save Cursor <ESC>[s */
						vc->saved_x = vc->x;
						vc->saved_y = vc->y;
						CSE;
						continue;
					case 'u':	/* Restore Cursor <ESC>[u */
						vc->x = vc->saved_x;
						vc->y = vc->saved_y;
						CSE;
						continue;
					default:
						CSE;
						break;
				}
			} else {
				switch(ch) {
					case '[':
						vc->sbracket = 1;
						vc->semicolon = 0;
						vc->question = 0;
						vc->parmv1 = vc->parmv2 = 0;
						vc->nparms = 0;
						memset_b(vc->parms, 0, sizeof(vc->parms));
						continue;
					case '7':	/* Save Cursor & Attrs <ESC>7 */
						vc->saved_x = vc->x;
						vc->saved_y = vc->y;
						CSE;
						continue;
					case '8':	/* Restore Cursor & Attrs <ESC>8 */
						vc->x = vc->saved_x;
						vc->y = vc->saved_y;
						CSE;
						continue;
					case 'D':	/* Scroll Up <ESC>D */
						lf(vc);
						CSE;
						continue;
					case 'E':	/* Move To Next Line <ESC>E */
						cr(vc);
						lf(vc);
						CSE;
						continue;
					case 'H':	/* Set Tab <ESC>H */
						vc->tty->tab_stop[vc->x] = 1;
						CSE;
						continue;
					case 'M':	/* Scroll Down <ESC>M */
						ri(vc);
						CSE;
						continue;
					case 'Z':	/* Identify Terminal <ESC>Z */
						insert_seq(tty, VT100ID, 7);
						CSE;
						continue;
					case 'c':	/* Reset Device <ESC>c */
						vconsole_reset(vc->tty);
						vc->x = vc->y = 0;
						csi_J(vc, CSI_J_SCREEN);
						CSE;
						continue;
					default:
						CSE;
						break;
				}
			}
		}
		switch(ch) {
			case '\033':
				vc->esc = 1;
				vc->sbracket = 0;
				vc->semicolon = 0;
				vc->question = 0;
				vc->parmv1 = vc->parmv2 = 0;
				continue;
			default:
				echo_char(vc, &ch, 1);
				continue;
		}
	}
	if(ch) {
		if(vc->vc_mode != KD_GRAPHICS) {
			video.update_curpos(vc);
		}
		wakeup(&tty_write);
	}
}

void vconsole_select(int new_cons)
{
	new_cons++;
	if(current_cons != new_cons) {
		if(vc[current_cons].vt_mode.mode == VT_PROCESS) {
			if(!kill_pid(vc[current_cons].tty->pid, vc[current_cons].vt_mode.acqsig)) {
				vc[current_cons].switchto_tty = new_cons;
				return;
			}
			init_vt(&vc[current_cons]);
		}
		if(vc[current_cons].vc_mode == KD_GRAPHICS) {
			return;
		}
		vconsole_select_final(new_cons);
	}
}

void vconsole_select_final(int new_cons)
{
	if(current_cons != new_cons) {
		if(vc[new_cons].vt_mode.mode == VT_PROCESS) {
			if(kill_pid(vc[new_cons].tty->pid, vc[new_cons].vt_mode.acqsig)) {
				init_vt(&vc[new_cons]);
			}
		}
		if(video.buf_top) {
			video.buf_top = 0;
			video.show_cursor(&vc[current_cons], ON);
			video.update_curpos(&vc[current_cons]);
		}
		vc[current_cons].vidmem = NULL;
		vc[current_cons].flags &= ~CONSOLE_HAS_FOCUS;
		vc[new_cons].vidmem = (unsigned char *)video.address;
		vc[new_cons].flags |= CONSOLE_HAS_FOCUS;
		video.restore_screen(&vc[new_cons]);
		current_cons = new_cons;
		set_leds(vc[current_cons].led_status);
		video.update_curpos(&vc[current_cons]);

		video.buf_y = vc[current_cons].y;
		video.buf_top = 0;
		vcbuf_refresh(&vc[current_cons]);
		video.show_cursor(&vc[current_cons], COND);
		video.cursor_blink((unsigned int)&vc[current_cons]);
	}
}

void unblank_screen(struct vconsole *vc)
{
	if(!(vc->flags & CONSOLE_BLANKED)) {
		return;
	}
	video.restore_screen(vc);
	vc->flags &= ~CONSOLE_BLANKED;
	video.show_cursor(vc, ON);
}

void vconsole_start(struct tty *tty)
{
	struct vconsole *vc;

	vc = (struct vconsole *)tty->driver_data;
	if(!vc->scrlock) {
		return;
	}
	vc->led_status &= ~SCRLBIT;
	vc->scrlock = 0;
	set_leds(vc->led_status);
}

void vconsole_stop(struct tty *tty)
{
	struct vconsole *vc;

	vc = (struct vconsole *)tty->driver_data;
	if(vc->scrlock) {
		return;
	}
	vc->led_status |= SCRLBIT;
	vc->scrlock = 1;
	set_leds(vc->led_status);
}

void vconsole_beep(void)
{
	struct callout_req creq;

	pit_beep_on();
	creq.fn = pit_beep_off;
	creq.arg = 0;
	add_callout(&creq, HZ / 8);
}

void vconsole_deltab(struct tty *tty)
{
	int col, n;
	unsigned char count;
	struct vconsole *vc;
	struct cblock *cb;
	unsigned char ch;

	vc = (struct vconsole *)tty->driver_data;
	cb = tty->cooked_q.head;
	col = count = 0;

	while(cb) {
		for(n = 0; n < cb->end_off; n++) {
			if(n >= cb->start_off) {
				ch = cb->data[n];
				if(ch == '\t') {
					while(!vc->tty->tab_stop[++col]);
				} else {
					col++;
					if(ISCNTRL(ch) && !ISSPACE(ch) && tty->termios.c_lflag & ECHOCTL) {
						col++;
					}
				}
				col %= vc->columns;
			}
		}
		cb = cb->next;
	}
	count = vc->x - col;

	while(count--) {
		tty_queue_putchar(tty, &tty->write_q, '\b');
	}
}

void console_flush_log_buf(char *buffer, unsigned int count)
{
	char *b;
	struct tty *tty;

	if(!(tty = get_tty(_syscondev))) {
		_syscondev = MKDEV(VCONSOLES_MAJOR, 0);
		tty = get_tty(_syscondev);
	}
	b = buffer;

	while(count) {
		if(tty_queue_putchar(tty, &tty->write_q, *b) < 0) {
			tty->output(tty);
			continue;
		}
		count--;
		b++;
	}
	tty->output(tty);
}

void console_init(void)
{
	int n;
	struct tty *tty;

	if(video.flags & VPF_VGA) {
		printk("console   0x%04X-0x%04X    -    %s (%d virtual consoles)\n", video.port, video.port + 1, video.signature, NR_VCONSOLES);
	}
	if(video.flags & VPF_VESAFB) {
		printk("console                    -    color frame buffer, screen=%dx%d, font=%dx%d\n", video.columns, video.lines, video.fb_char_width, video.fb_char_height);
		printk("\t\t\t\t(%d virtual consoles)\n", NR_VCONSOLES);
	}

	for(n = 1; n <= NR_VCONSOLES; n++) {
		if(!register_tty(MKDEV(VCONSOLES_MAJOR, n))) {
			tty = get_tty(MKDEV(VCONSOLES_MAJOR, n));
			tty->driver_data = (void *)&vc[n];
			tty->stop = vconsole_stop;
			tty->start = vconsole_start;
			tty->deltab = vconsole_deltab;
			tty->reset = vconsole_reset;
			tty->input = do_cook;
			tty->output = vconsole_write;
			vc[n].tty = tty;
			if(video.flags & VPF_VGA) {
				vc[n].screen = (short int *)kmalloc();
			}
			if(video.flags & VPF_VESAFB) {
				vc[n].screen = vc_screen[n];
			}
			vc[n].vidmem = NULL;
			memset_w(vc[n].screen, BLANK_MEM, SCREEN_SIZE);
			vconsole_reset(tty);
		}
	}

	current_cons = 1;
	video.show_cursor(&vc[current_cons], ON);
	vc[current_cons].vidmem = (unsigned char *)video.address;
	vc[current_cons].flags |= CONSOLE_HAS_FOCUS;

	if(video.flags & VPF_VGA) {
		memcpy_w(vc[current_cons].screen, video.address, SCREEN_SIZE);
	}

	video.get_curpos(&vc[current_cons]);
	video.update_curpos(&vc[current_cons]);
	video.buf_y = vc[current_cons].y;
	video.buf_top = 0;

	SET_MINOR(console_device.minors, 0);
	SET_MINOR(console_device.minors, 1);
	for(n = 0; n <= NR_VCONSOLES; n++) {
		SET_MINOR(tty_device.minors, n);
	}

	register_device(CHR_DEV, &console_device);
	register_device(CHR_DEV, &tty_device);

	if(is_vconsole(_syscondev)) {
		register_console(console_flush_log_buf);
	}
}
