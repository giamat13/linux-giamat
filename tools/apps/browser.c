/* fbdesktop -- browser: Firefox on a headless Xvfb, captured over MIT-SHM and fed
 * input with XTEST */
#include "fbdesktop.h"

/* ---- browser: Firefox on a headless Xvfb, shown inside one of our windows ---- */

#define BR_DISPLAY ":1"

static Display *xdpy;
static XImage *xshm_img;
static XShmSegmentInfo xshm;
static pid_t xvfb_pid, ff_pid;
int browser_win = -1;
static time_t browser_started;
/* The Xvfb screen is the whole display: that is the largest the window can ever
 * get (maximised), and Firefox itself is resized to fit whatever we are showing,
 * so the capture is always exactly the window's content -- never letterboxed. */
static int br_sw, br_sh;
static int br_fitw, br_fith;   /* size Firefox was last stretched to */


void browser_teardown(void)
{
	if (xshm_img) {
		XShmDetach(xdpy, &xshm);
		XDestroyImage(xshm_img);
		shmdt(xshm.shmaddr);
		shmctl(xshm.shmid, IPC_RMID, NULL);
		xshm_img = NULL;
	}
	if (xdpy) {
		XCloseDisplay(xdpy);
		xdpy = NULL;
	}
	if (ff_pid > 0) {
		kill(ff_pid, SIGTERM);
		waitpid(ff_pid, NULL, 0);
		ff_pid = 0;
	}
	if (xvfb_pid > 0) {
		kill(xvfb_pid, SIGTERM);
		waitpid(xvfb_pid, NULL, 0);
		xvfb_pid = 0;
	}
	browser_win = -1;
}

/* Nothing inside Xvfb manages windows, so Firefox maps at whatever size it likes
 * and never learns that our frame was resized. We are the closest thing to a WM
 * in there: size its top level to the window's content area.
 * Override-redirect children are menus, popups and tooltips -- resizing those to
 * full size would wreck them, so they are left alone. */
static void browser_fit_windows(int w, int h)
{
	Window root = DefaultRootWindow(xdpy), par, *kids = NULL;
	unsigned int nkids = 0;
	if (w <= 0 || h <= 0 || !XQueryTree(xdpy, root, &root, &par, &kids, &nkids))
		return;
	for (unsigned int i = 0; i < nkids; i++) {
		XWindowAttributes wa;
		if (XGetWindowAttributes(xdpy, kids[i], &wa) && !wa.override_redirect)
			XMoveResizeWindow(xdpy, kids[i], 0, 0, w, h);
	}
	if (kids)
		XFree(kids);
	XFlush(xdpy);
	br_fitw = w;
	br_fith = h;
}

static int browser_start_x(void)
{
	br_sw = xres;
	br_sh = yres;

	xvfb_pid = fork();
	if (xvfb_pid == 0) {
		/* SIG_IGN on SIGCHLD survives exec, and Xvfb waits on the xkbcomp it
		 * forks -- inherit our setting and that wait fails with ECHILD, which
		 * Xvfb reports as "failed to compile keymap" and dies. */
		signal(SIGCHLD, SIG_DFL);
		/* serial, not /dev/null: the framebuffer is ours, so this is the only
		 * place an X or Firefox failure can actually be read. */
		int log = open("/dev/ttyS0", O_WRONLY);
		if (log >= 0) { dup2(log, 1); dup2(log, 2); }
		char geom[32];
		snprintf(geom, sizeof(geom), "%dx%dx24", br_sw, br_sh);
		execlp("Xvfb", "Xvfb", BR_DISPLAY, "-screen", "0", geom,
		       "-nolisten", "tcp", "-ac", NULL);
		_exit(1);
	}
	if (xvfb_pid < 0)
		return -1;

	for (int try = 0; try < 100 && !xdpy; try++) {   /* Xvfb needs a moment */
		usleep(100000);
		xdpy = XOpenDisplay(BR_DISPLAY);
	}
	if (!xdpy) {
		DBG("[browser] XOpenDisplay(%s) failed\n", BR_DISPLAY);
		return -1;
	}

	int major, minor, pixmaps;
	if (!XShmQueryVersion(xdpy, &major, &minor, &pixmaps)) {
		DBG("[browser] no MIT-SHM\n");
		return -1;
	}
	xshm_img = XShmCreateImage(xdpy, DefaultVisual(xdpy, 0), DefaultDepth(xdpy, 0),
				   ZPixmap, NULL, &xshm, br_sw, br_sh);
	if (!xshm_img) {
		DBG("[browser] XShmCreateImage failed\n");
		return -1;
	}
	xshm.shmid = shmget(IPC_PRIVATE,
			    (size_t)xshm_img->bytes_per_line * xshm_img->height,
			    IPC_CREAT | 0600);
	if (xshm.shmid < 0) {
		DBG("[browser] shmget failed: %s\n", strerror(errno));
		return -1;
	}
	xshm.shmaddr = xshm_img->data = shmat(xshm.shmid, NULL, 0);
	xshm.readOnly = False;
	if (xshm.shmaddr == (char *)-1 || !XShmAttach(xdpy, &xshm)) {
		DBG("[browser] XShmAttach failed\n");
		return -1;
	}
	XSync(xdpy, False);
	DBG("[browser] X ready, depth=%d\n", DefaultDepth(xdpy, 0));
	return 0;
}

int spawn_browser(void)
{
	if (browser_win >= 0 && wins[browser_win].used) {   /* only ever one */
		focused = browser_win;
		wins[browser_win].minimized = 0;
		raise_window(browser_win);
		return browser_win;
	}
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;
	if (browser_start_x() < 0) {
		browser_teardown();
		return -1;
	}

	ff_pid = fork();
	if (ff_pid == 0) {
		signal(SIGCHLD, SIG_DFL);   /* Firefox reaps its own content processes */
		int log = open("/dev/ttyS0", O_WRONLY);
		if (log >= 0) { dup2(log, 1); dup2(log, 2); }
		setenv("DISPLAY", BR_DISPLAY, 1);
		setenv("HOME", "/root", 1);
		execlp("firefox-esr", "firefox-esr", "--no-remote", NULL);
		_exit(1);
	}
	if (ff_pid < 0) {
		browser_teardown();
		return -1;
	}

	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_BROWSER;
	wins[slot].pty_fd = -1;
	wins[slot].x = 40;
	wins[slot].y = 40;
	wins[slot].w = 1100;
	wins[slot].h = 680 + TITLE_H;
	if (wins[slot].w > xres - 80) wins[slot].w = xres - 80;
	if (wins[slot].h > yres - TASK_H - 80) wins[slot].h = yres - TASK_H - 80;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Firefox");
	clamp_window(&wins[slot]);
	zorder[zcount++] = slot;
	focused = slot;
	browser_win = slot;
	browser_started = time(NULL);
	return slot;
}

/* Pull the X screen and blit it into the window, clipped to whatever size the
 * user has dragged the frame to. No scaling: cheaper, and text stays sharp. */
void draw_browser(struct window *w)
{
	int cy = w->y + TITLE_H;
	int vw = w->w, vh = w->h - TITLE_H;
	if (vw > br_sw) vw = br_sw;
	if (vh > br_sh) vh = br_sh;
	if (vw <= 0 || vh <= 0)
		return;

	fill_rect(w->x, cy, w->w, w->h - TITLE_H, 0x101018);
	if (!xdpy || !xshm_img) {
		draw_text(w->x + 16, cy + 16, "starting browser...", 0x9399b2);
		return;
	}

	/* Keep Firefox exactly the size of the area we are about to show. Also keep
	 * re-fitting for the first few seconds: its window does not exist yet when
	 * the first frames are drawn. */
	if (vw != br_fitw || vh != br_fith || time(NULL) - browser_started < 8)
		browser_fit_windows(vw, vh);

	XShmGetImage(xdpy, DefaultRootWindow(xdpy), xshm_img, 0, 0, AllPlanes);

	int stride = xshm_img->bytes_per_line;
	for (int row = 0; row < vh; row++) {
		uint32_t *src = (uint32_t *)(xshm_img->data + (size_t)row * stride);
		for (int col = 0; col < vw; col++)
			put_pixel(w->x + col, cy + row, src[col] & 0xffffff);
	}
}

void browser_pointer(int lx, int ly, int left, int right)
{
	if (!xdpy)
		return;
	if (lx < 0) lx = 0;
	if (ly < 0) ly = 0;
	if (lx >= br_sw) lx = br_sw - 1;
	if (ly >= br_sh) ly = br_sh - 1;
	XTestFakeMotionEvent(xdpy, 0, lx, ly, 0);
	if (left != prev_left)
		XTestFakeButtonEvent(xdpy, 1, left ? True : False, 0);
	if (right != prev_right)
		XTestFakeButtonEvent(xdpy, 3, right ? True : False, 0);
	XFlush(xdpy);
}

static void browser_tap(KeySym ks)
{
	KeyCode kc = XKeysymToKeycode(xdpy, ks);
	if (!kc)
		return;
	/* If the key only produces this symbol with Shift down, hold Shift. */
	int shift = (XkbKeycodeToKeysym(xdpy, kc, 0, 0) != ks &&
		     XkbKeycodeToKeysym(xdpy, kc, 0, 1) == ks);
	KeyCode shiftkc = XKeysymToKeycode(xdpy, XK_Shift_L);
	if (shift)
		XTestFakeKeyEvent(xdpy, shiftkc, True, 0);
	XTestFakeKeyEvent(xdpy, kc, True, 0);
	XTestFakeKeyEvent(xdpy, kc, False, 0);
	if (shift)
		XTestFakeKeyEvent(xdpy, shiftkc, False, 0);
	XFlush(xdpy);
}

/* stdin is a raw tty, so this is bytes, not keysyms: translate the handful that
 * matter and pass printable ASCII straight through (keysym == codepoint). */
void browser_keys(const char *buf, int n)
{
	if (!xdpy)
		return;
	for (int i = 0; i < n; i++) {
		unsigned char c = (unsigned char)buf[i];
		if (c == 0x1b && i + 2 < n && buf[i + 1] == '[') {
			switch (buf[i + 2]) {
			case 'A': browser_tap(XK_Up); break;
			case 'B': browser_tap(XK_Down); break;
			case 'C': browser_tap(XK_Right); break;
			case 'D': browser_tap(XK_Left); break;
			case 'H': browser_tap(XK_Home); break;
			case 'F': browser_tap(XK_End); break;
			default: break;
			}
			i += 2;
		} else if (c == '\r' || c == '\n') {
			browser_tap(XK_Return);
		} else if (c == 0x7f || c == 0x08) {
			browser_tap(XK_BackSpace);
		} else if (c == '\t') {
			browser_tap(XK_Tab);
		} else if (c == 0x1b) {
			browser_tap(XK_Escape);
		} else if (c >= 0x20 && c < 0x7f) {
			browser_tap((KeySym)c);
		}
	}
}
