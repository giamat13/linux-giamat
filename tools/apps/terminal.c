/* fbdesktop -- terminal app: a live pty-backed VT100-ish window, plus the
 * one-shot command-output view. The grid model itself lives in grid.c. */
#include "fbdesktop.h"

void run_and_show(const char *cmd)
{
	fill_rect(0, 0, xres, yres, 0x11111b);
	FILE *p = popen(cmd, "r");
	int y = 10, max_rows = (yres - 40) / font_h;
	int row = 0;
	if (p) {
		char line[512];
		while (row < max_rows && fgets(line, sizeof(line), p)) {
			line[strcspn(line, "\n")] = 0;
			draw_text(10, y, line, 0xcdd6f4);
			y += font_h;
			row++;
		}
		pclose(p);
	}
	if (backbuf)
		memcpy(fbp, backbuf, (size_t)line_length * yres);
}

int spawn_terminal(void)
{
	int slot = alloc_window_slot();
	if (slot < 0)
		return -1;

	int master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0)
		return -1;
	if (grantpt(master) < 0 || unlockpt(master) < 0) {
		close(master);
		return -1;
	}
	char *slavename = ptsname(master);
	if (!slavename) {
		close(master);
		return -1;
	}
	char slavebuf[64];
	strncpy(slavebuf, slavename, sizeof(slavebuf) - 1);
	slavebuf[sizeof(slavebuf) - 1] = 0;

	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int slave = open(slavebuf, O_RDWR);
		if (slave < 0)
			_exit(1);
		ioctl(slave, TIOCSCTTY, 0);
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);
		if (slave > 2)
			close(slave);
		close(master);
		for (int i = 0; i < MAX_WIN; i++)
			if (i != slot && wins[i].used && wins[i].type == WIN_TERM)
				close(wins[i].pty_fd);
		setenv("TERM", "linux", 1);
		execl("/bin/sh", "sh", NULL);
		_exit(1);
	} else if (pid < 0) {
		close(master);
		return -1;
	}

	memset(&wins[slot], 0, sizeof(wins[slot]));
	wins[slot].used = 1;
	wins[slot].type = WIN_TERM;
	wins[slot].pty_fd = master;
	wins[slot].pid = pid;
	wins[slot].x = 200 + slot * 24;
	wins[slot].y = 120 + slot * 24;
	wins[slot].w = 560;
	wins[slot].h = 360;
	wins[slot].attr_fg = COL_FG_DEFAULT;
	wins[slot].attr_bg = COL_BG_DEFAULT;
	snprintf(wins[slot].title, sizeof(wins[slot].title), "Terminal %d", slot + 1);
	update_grid_dims(&wins[slot]);
	for (int r = 0; r < wins[slot].rows; r++)
		clear_row_range(&wins[slot], r, 0, wins[slot].cols - 1);
	resize_notify(&wins[slot]);
	zorder[zcount++] = slot;
	focused = slot;
	return slot;
}
