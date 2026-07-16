/* fbdesktop -- startup (framebuffer, font, input devices, raw tty), hit testing,
 * icon launching and the event loop */
#include "fbdesktop.h"

static void do_hit_test(int x, int y)
{
	if (y >= yres - TASK_H) {
		if (x >= sd_x() && x < sd_x() + SD_W) {
			toggle_show_desktop();
			return;
		}
		int bx = 8;
		for (int zi = 0; zi < zcount; zi++) {
			int i = zorder[zi];
			if (!wins[i].used)
				continue;
			int bw = 130;
			if (bx + bw > task_limit())
				break;
			if (x >= bx && x < bx + bw) {
				wins[i].minimized = 0;
				raise_window(i);
				focused = i;
				return;
			}
			bx += bw + 6;
		}
		return;
	}

	for (int zi = zcount - 1; zi >= 0; zi--) {
		int i = zorder[zi];
		if (!wins[i].used || wins[i].minimized)
			continue;
		struct window *w = &wins[i];
		if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
			if (y < w->y + TITLE_H) {
				int closeX = w->x + w->w - 24;
				int maxX = closeX - 24;
				int minX = maxX - 24;
				if (x >= closeX) {
					close_window(i);
				} else if (x >= maxX) {
					raise_window(i);
					focused = i;
					toggle_maximize(i);
				} else if (x >= minX) {
					wins[i].minimized = 1;
				} else {
					raise_window(i);
					focused = i;
					drag_mode = 1;
					drag_win = i;
				}
				return;
			} else if (!w->maximized && x >= w->x + w->w - 10 && y >= w->y + w->h - 10) {
				raise_window(i);
				focused = i;
				drag_mode = 2;
				drag_win = i;
				return;
			} else {
				raise_window(i);
				focused = i;
				if (w->type == WIN_FILES) {
					fm_click(w, x, y);
				} else if (w->type == WIN_EDIT) {
					ed_click(w, x, y);
				} else if (w->type == WIN_SETTINGS) {
					int t = settings_click(w, x, y);
					if (t >= 0 && t < NUM_THEMES) {
						theme_idx = t;
					} else if (t == NUM_THEMES) {
						show_hidden = !show_hidden;
						/* Reload all open file windows and the desktop to show/hide .* files */
						for (int i = 0; i < MAX_WIN; i++)
							if (wins[i].used && wins[i].type == WIN_FILES && wins[i].fm)
								fm_load(&wins[i]);
						desk_scan();
					}
				} else if (w->type == WIN_TASKMGR) {
					taskmgr_click(w, x, y);
				}
				return;
			}
		}
	}

	/* Press on an icon only *selects* it. It launches on release if the
	 * pointer never moved; otherwise the motion turns into a drag.
	 * Indices >= NUM_ICONS are desktop files: same press/drag/release
	 * state machine, but their position is fixed (grid-computed), not
	 * draggable, so a "drag" on one just cancels the open. */
	int idx = icon_at(x, y);
	if (idx >= 0) {
		icon_press = idx;
		icon_dragged = 0;
		int ix, iy;
		if (idx < NUM_ICONS) { ix = icons[idx].x; iy = icons[idx].y; }
		else desk_item_pos(idx, &ix, &iy);
		icon_grab_dx = x - ix;
		icon_grab_dy = y - iy;
	}
}

static void launch_icon(int idx)
{
	struct icon *ic = &icons[idx];
	if (ic->action == 1) {
		run_and_show("echo Rebooting...");
		usleep(500000);
		system("reboot -f");
	} else if (ic->action == 2) {
		run_and_show("echo Powering off...");
		usleep(500000);
		system("poweroff -f");
	} else if (ic->action == 3) {
		spawn_terminal();
	} else if (ic->action == 7) {
		spawn_browser();
	} else if (ic->action == 4) {
		spawn_file_window();
	} else if (ic->action == 5) {
		spawn_taskmgr();
	} else if (ic->action == 6) {
		spawn_settings();
	}
}

/* Opening a desktop icon: a folder opens a File Manager rooted there, a
 * file opens the text editor -- same as double-clicking it in File Manager. */
static void launch_deskfile(int i)
{
	if (i < 0 || i >= desk_count)
		return;
	struct deskfile *df = &desk_files[i];
	char path[FM_FULLLEN];
	snprintf(path, sizeof(path), "%s/%s", DESKTOP_DIR, df->name);
	if (df->isdir) {
		int slot = spawn_file_window();
		if (slot >= 0) {
			snprintf(wins[slot].fm->cwd, sizeof(wins[slot].fm->cwd), "%s", path);
			fm_load(&wins[slot]);
		}
	} else {
		spawn_editor(path);
	}
}

/* Shared pointer handler: nx,ny = new absolute cursor position, left = button.
 * Works for both absolute (evdev tablet) and relative (PS/2 mouse) sources. */
int process_pointer(int nx, int ny, int left, int right)
{
	if (nx < 0) nx = 0;
	if (ny < 0) ny = 0;
	if (nx >= xres) nx = xres - 1;
	if (ny >= yres) ny = yres - 1;
	int dx = nx - mx, dy = ny - my;
	mx = nx;
	my = ny;

	/* A focused browser window swallows the pointer over its content area, so
	 * Firefox gets the events -- but the titlebar and the resize grip stay
	 * ours, which is what keeps drag/minimise/maximise/close working. */
	if (browser_win >= 0 && wins[browser_win].used && focused == browser_win &&
	    !wins[browser_win].minimized && drag_mode == 0 && icon_press < 0) {
		struct window *bw = &wins[browser_win];
		int cy = bw->y + TITLE_H;
		int grip = bw->maximized ? 0 : 10;   /* matches do_hit_test's corner */
		if (mx >= bw->x && mx < bw->x + bw->w - grip &&
		    my >= cy && my < bw->y + bw->h - grip) {
			browser_pointer(mx - bw->x, my - cy, left, right);
			prev_left = left;
			prev_right = right;
			return 1;
		}
	}

	int changed = (dx || dy);
	if (left && drag_mode == 1 && drag_win >= 0 && wins[drag_win].used) {
		wins[drag_win].x += dx;
		wins[drag_win].y += dy;
		clamp_window(&wins[drag_win]);
		changed = 1;
	} else if (left && drag_mode == 2 && drag_win >= 0 && wins[drag_win].used) {
		wins[drag_win].w += dx;
		wins[drag_win].h += dy;
		if (wins[drag_win].w < WIN_MINW) wins[drag_win].w = WIN_MINW;
		if (wins[drag_win].h < WIN_MINH) wins[drag_win].h = WIN_MINH;
		resize_notify(&wins[drag_win]);
		changed = 1;
	} else if (left && prev_left && icon_press >= 0) {
		/* Past a few pixels of travel this is a drag, not a click.
		 * Desktop files (idx >= NUM_ICONS) have a fixed, grid-computed
		 * position -- dragging one just cancels the open, it doesn't move. */
		int ix, iy;
		if (icon_press < NUM_ICONS) { ix = icons[icon_press].x; iy = icons[icon_press].y; }
		else desk_item_pos(icon_press, &ix, &iy);
		if (!icon_dragged && (dx * dx + dy * dy) > 0) {
			int tx = mx - icon_grab_dx - ix;
			int ty = my - icon_grab_dy - iy;
			if (tx * tx + ty * ty > 9)
				icon_dragged = 1;
		}
		if (icon_dragged && icon_press < NUM_ICONS) {
			icons[icon_press].x = mx - icon_grab_dx;
			icons[icon_press].y = my - icon_grab_dy;
			clamp_icon(&icons[icon_press]);
			changed = 1;
		}
	} else if (left && prev_left && fmdrag_win >= 0 && !fmdrag_active) {
		/* Same drag-threshold pattern for a pressed file manager row. */
		if ((dx * dx + dy * dy) > 0) {
			int tx = mx - fmdrag_grab_x, ty = my - fmdrag_grab_y;
			if (tx * tx + ty * ty > 9)
				fmdrag_active = 1;
		}
		changed = 1;
	} else if (left && !prev_left) {
		/* An open context menu eats the next click: on it, run the item;
		 * off it, dismiss it and let the click through to the desktop. */
		if (ctxmenu_mode != CTXMODE_NONE) {
			int handled = ctxmenu_click(mx, my);
			ctxmenu_mode = CTXMODE_NONE;
			if (!handled)
				do_hit_test(mx, my);
		} else {
			do_hit_test(mx, my);
		}
		changed = 1;
	}

	if (!left && prev_left) {
		if (drag_mode == 2 && drag_win >= 0 && wins[drag_win].used)
			resize_notify(&wins[drag_win]);
		if (icon_press >= 0) {
			int idx = icon_press;
			int was_drag = icon_dragged;
			icon_press = -1;
			icon_dragged = 0;
			if (!was_drag) { /* a click that never moved */
				if (idx < NUM_ICONS)
					launch_icon(idx);
				else
					launch_deskfile(idx - NUM_ICONS);
			}
		}
		if (fmdrag_win >= 0) {
			if (fmdrag_active)
				fm_drop(mx, my);
			else if (fmdrag_was_preselected)
				fm_open_selected(fmdrag_win, fmdrag_entidx);
			fmdrag_win = -1;
			fmdrag_entidx = -1;
			fmdrag_active = 0;
		}
		changed = 1;
	}
	if (left != prev_left)
		changed = 1;
	if (!left) {
		drag_mode = 0;
		drag_win = -1;
	}
	prev_left = left;

	if (right && !prev_right) {
		ctxmenu_open(mx, my);
		changed = 1;
	}
	prev_right = right;

	return changed;
}

/* PS/2 relative fallback (real mouse / touchpad, no absolute device). */
static int handle_mouse_packet(unsigned char *pkt)
{
	int left = pkt[0] & 0x1;
	int right = pkt[0] & 0x2;
	int dx = pkt[1];
	int dy = pkt[2];
	if (pkt[0] & 0x10) dx -= 256;
	if (pkt[0] & 0x20) dy -= 256;
	dy = -dy;
	return process_pointer(mx + dx, my + dy, left, right);
}

/* Read all pending events from the absolute pointer; map to screen coords. */
static int read_abs_pointer(void)
{
	struct input_event ev;
	int changed = 0;
	while (read(absptr_fd, &ev, sizeof(ev)) == (int)sizeof(ev)) {
		if (ev.type == EV_ABS) {
			if (ev.code == ABS_X) abs_curx = ev.value;
			else if (ev.code == ABS_Y) abs_cury = ev.value;
		} else if (ev.type == EV_KEY) {
			if (ev.code == BTN_LEFT || ev.code == BTN_TOUCH)
				abs_btn = ev.value ? 1 : 0;
			else if (ev.code == BTN_RIGHT)
				abs_rbtn = ev.value ? 1 : 0;
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			int rx = abs_maxx - abs_minx; if (rx <= 0) rx = 1;
			int ry = abs_maxy - abs_miny; if (ry <= 0) ry = 1;
			int nx = (int)((long)(abs_curx - abs_minx) * (xres - 1) / rx);
			int ny = (int)((long)(abs_cury - abs_miny) * (yres - 1) / ry);
			if (process_pointer(nx, ny, abs_btn, abs_rbtn))
				changed = 1;
		}
	}
	return changed;
}

/* Read evdev keyboard just to catch Alt+Tab (keymap-independent). Text input
 * still flows through stdin. */
static int read_kbd_evdev(void)
{
	struct input_event ev;
	int changed = 0;
	while (read(kbd_evdev_fd, &ev, sizeof(ev)) == (int)sizeof(ev)) {
		if (ev.type != EV_KEY)
			continue;
		if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) {
			alt_held = (ev.value != 0);
		} else if (ev.code == KEY_TAB && ev.value == 1 && alt_held) {
			cycle_window_focus();
			changed = 1;
		}
	}
	return changed;
}

static void scan_input_devices(void)
{
	for (int i = 0; i < 32; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		unsigned char evbit[(EV_MAX + 7) / 8] = {0};
		unsigned char absbit[(ABS_MAX + 7) / 8] = {0};
		unsigned char keybit[(KEY_MAX + 7) / 8] = {0};
		ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit);
		int is_abs = 0, is_kbd = 0;
		if (test_bit(EV_ABS, evbit)) {
			ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit);
			if (test_bit(ABS_X, absbit) && test_bit(ABS_Y, absbit))
				is_abs = 1;
		}
		if (test_bit(EV_KEY, evbit)) {
			ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
			if (test_bit(KEY_A, keybit) && test_bit(KEY_ENTER, keybit))
				is_kbd = 1;
		}
		if (is_abs && absptr_fd < 0) {
			absptr_fd = fd;
			struct input_absinfo ai;
			if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) { abs_minx = ai.minimum; abs_maxx = ai.maximum; }
			if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) { abs_miny = ai.minimum; abs_maxy = ai.maximum; }
			DBG("[input] abs pointer %s x[%d..%d] y[%d..%d]\n", path, abs_minx, abs_maxx, abs_miny, abs_maxy);
			continue;
		}
		if (is_kbd && kbd_evdev_fd < 0) {
			kbd_evdev_fd = fd;
			DBG("[input] keyboard %s\n", path);
			continue;
		}
		close(fd);
	}
}

/* USB tablets enumerate a few hundred ms after boot, often after we first run.
 * Retry briefly until an absolute pointer appears, then give up and let the
 * caller fall back to the relative PS/2 mouse.
 * ponytail: fixed ~2s cap; only real hardware with no tablet ever waits the
 * full time. Switch to a udev/inotify watch if that delay matters. */
static void open_input_devices(void)
{
	for (int attempt = 0; attempt < 20; attempt++) {
		scan_input_devices();
		if (absptr_fd >= 0)
			return;
		usleep(100000);
	}
}

static void setup_raw_stdin(void)
{
	if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
		have_orig_termios = 1;
		struct termios raw = orig_termios;
		raw.c_lflag &= ~(ICANON | ECHO | ISIG);
		raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	}
}

static void restore_stdin(void)
{
	if (have_orig_termios)
		tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

int main(void)
{
	int fbfd = open("/dev/fb0", O_RDWR);
	if (fbfd < 0) {
		perror("open /dev/fb0");
		return 1;
	}
	ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);
	ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo);
	xres = vinfo.xres;
	yres = vinfo.yres;
	bpp = vinfo.bits_per_pixel;
	line_length = finfo.line_length;
	long screensize = (long)line_length * yres;
	fbp = mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fbp == MAP_FAILED) {
		perror("mmap fb");
		return 1;
	}
	backbuf = malloc(screensize);  /* NULL is fine: put_pixel falls back to fbp */

	dbg = fopen("/dev/ttyS0", "w");
	if (dbg)
		setvbuf(dbg, NULL, _IONBF, 0);

	confd = open("/dev/tty1", O_RDWR);
	if (confd >= 0) {
		struct console_font_op op;
		memset(&op, 0, sizeof(op));
		op.op = KD_FONT_OP_GET;
		op.width = 32;
		op.height = 32;
		op.charcount = 512;
		op.data = font;
		int r = ioctl(confd, KDFONTOP, &op);
		have_font = r == 0;
		if (have_font) {
			font_w = op.width;
			font_h = op.height;
			font_bpr = (font_w + 7) / 8;
		}
		ioctl(confd, KDSETMODE, KD_GRAPHICS);
	}
	setup_raw_stdin();
	DBG("[fbdesktop] xres=%d yres=%d bpp=%d have_font=%d font_w=%d font_h=%d\n",
		xres, yres, bpp, have_font, font_w, font_h);

	init_icon_positions(); /* needs xres/yres; without it every icon sits at 0,0 */
	mkdir(DESKTOP_DIR, 0755); /* ignore EEXIST -- it's fine if it's already there */
	desk_scan();

	open_input_devices();
	/* Only fall back to relative PS/2 mouse when no absolute tablet exists,
	 * otherwise mousedev would relay the tablet as relative and cause drift. */
	int mousefd = (absptr_fd < 0) ? open("/dev/input/mice", O_RDONLY) : -1;

	mx = xres / 2;
	my = yres / 2;

	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	redraw_all();

	for (;;) {
		struct pollfd fds[4 + MAX_WIN];
		int n = 0;
		int mouse_i = -1, abs_i = -1, kev_i = -1, kbd_i;
		if (mousefd >= 0) {
			mouse_i = n;
			fds[n].fd = mousefd;
			fds[n].events = POLLIN;
			n++;
		}
		if (absptr_fd >= 0) {
			abs_i = n;
			fds[n].fd = absptr_fd;
			fds[n].events = POLLIN;
			n++;
		}
		if (kbd_evdev_fd >= 0) {
			kev_i = n;
			fds[n].fd = kbd_evdev_fd;
			fds[n].events = POLLIN;
			n++;
		}
		kbd_i = n;
		fds[n].fd = STDIN_FILENO;
		fds[n].events = POLLIN;
		n++;
		int win_i[MAX_WIN];
		for (int i = 0; i < MAX_WIN; i++) {
			win_i[i] = -1;
			if (wins[i].used && wins[i].type == WIN_TERM) {
				win_i[i] = n;
				fds[n].fd = wins[i].pty_fd;
				fds[n].events = POLLIN;
				n++;
			}
		}

		/* Wake at least once a second so the taskbar clock ticks and any
		 * open Task Manager tab refreshes without needing input. A visible
		 * browser has no fd to poll -- Firefox just paints into the X screen --
		 * so it needs us to come back and re-copy it, hence the faster tick. */
		int browser_live = (browser_win >= 0 && wins[browser_win].used &&
				    !wins[browser_win].minimized);
		int pr = poll(fds, n, browser_live ? 40 : 1000);
		if (pr == 0 && browser_live) {
			redraw_all();
			continue;
		}
		if (pr == 0) {
			sample_stats();
			for (int i = 0; i < MAX_WIN; i++)
				if (wins[i].used && wins[i].type == WIN_TASKMGR && !wins[i].minimized)
					taskmgr_refresh(&wins[i]);
			/* Pick up files added/removed in DESKTOP_DIR from outside this
			 * app (a terminal, another program) -- skip mid-interaction so
			 * indices an active press/menu is holding don't shift under it. */
			if (icon_press < 0 && ctxmenu_mode != CTXMODE_DESKTOP)
				desk_scan();
			redraw_all();
			continue;
		}
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		int need_redraw = 0;

		if (mouse_i >= 0 && (fds[mouse_i].revents & POLLIN)) {
			unsigned char pkt[3];
			if (read(mousefd, pkt, 3) == 3) {
				if (handle_mouse_packet(pkt))
					need_redraw = 1;
			}
		}
		if (abs_i >= 0 && (fds[abs_i].revents & POLLIN)) {
			if (read_abs_pointer())
				need_redraw = 1;
		}
		if (kev_i >= 0 && (fds[kev_i].revents & POLLIN)) {
			if (read_kbd_evdev())
				need_redraw = 1;
		}
		if (fds[kbd_i].revents & POLLIN) {
			char buf[64];
			int r = read(STDIN_FILENO, buf, sizeof(buf));
			/* Swallow keystrokes while Alt is held so Alt+Tab's ESC/Tab bytes
			 * don't leak into the focused window. */
			if (r > 0 && !alt_held && focused >= 0 && wins[focused].used) {
				if (wins[focused].type == WIN_BROWSER)
					browser_keys(buf, r);
				else if (wins[focused].type == WIN_TERM)
					write(wins[focused].pty_fd, buf, r);
				else if (wins[focused].type == WIN_EDIT &&
					 ed_keys(&wins[focused], buf, r))
					need_redraw = 1;
				else if (wins[focused].type == WIN_FILES &&
					 fm_keys(&wins[focused], buf, r))
					need_redraw = 1;
			}
		}
		for (int i = 0; i < MAX_WIN; i++) {
			if (win_i[i] >= 0 && (fds[win_i[i]].revents & (POLLIN | POLLHUP))) {
				char buf[1024];
				int r = read(wins[i].pty_fd, buf, sizeof(buf));
				if (r > 0) {
					process_bytes(&wins[i], (unsigned char *)buf, r);
					need_redraw = 1;
				} else {
					close_window(i);
					need_redraw = 1;
				}
			}
		}

		if (need_redraw)
			redraw_all();
	}

	restore_stdin();
	if (confd >= 0)
		ioctl(confd, KDSETMODE, KD_TEXT);
	return 0;
}
