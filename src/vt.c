/*
 * Copyright © 2004 Bruno T. C. de Oliveira
 * Copyright © 2006 Pierre Habouzit
 * Copyright © 2008-2016 Marc André Tanner
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <wchar.h>
#if defined(__linux__) || defined(__CYGWIN__)
# include <pty.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
# include <libutil.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
# include <util.h>
#endif

#include "vt.h"

#ifdef _AIX
# include "forkpty-aix.c"
#elif defined __sun
# include "forkpty-sunos.c"
#endif

#ifndef NCURSES_ATTR_SHIFT
# define NCURSES_ATTR_SHIFT 8
#endif

#ifndef NCURSES_ACS
# ifdef PDCURSES
#  define NCURSES_ACS(c) (acs_map[(unsigned char)(c)])
# else /* BSD curses */
#  define NCURSES_ACS(c) (_acs_map[(unsigned char)(c)])
# endif
#endif

#ifdef NCURSES_VERSION
# ifndef NCURSES_EXT_COLORS
#  define NCURSES_EXT_COLORS 0
# endif
# if !NCURSES_EXT_COLORS
#  define MAX_COLOR_PAIRS MIN(COLOR_PAIRS, 256)
# endif
#endif
#ifndef MAX_COLOR_PAIRS
# define MAX_COLOR_PAIRS COLOR_PAIRS
#endif

#if defined _AIX && defined CTRL
# undef CTRL
#endif
#ifndef CTRL
# define CTRL(k)   ((k) & 0x1F)
#endif

#define IS_CONTROL(ch) !((ch) & 0xffffff60UL)
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define LENGTH(arr) (sizeof(arr) / sizeof((arr)[0]))

static bool is_utf8, has_default_colors;
static short color_pairs_reserved, color_pairs_max, color_pair_current;
static short *color2palette, default_fg, default_bg;
static char vt_term[32];
static bool has_truecolor;
static bool has_direct_color; /* host terminal accepts raw 38;2/48;2 SGR
                                * directly -- skip the indexed-color
                                * registration path (has_truecolor) entirely
                                * when this is true, since it's strictly
                                * faster: no OSC4 round trip per new color. */

/* truecolor (24-bit RGB) extended-pair cache, separate from the palette
 * cache above: keyed by packed RGB rather than an 8-bit palette index,
 * since 24-bit values can't be enumerated into MAX_COLOR_PAIRS slots. */
#define TRUECOLOR_PAIRS_MAX 512
typedef struct {
	int32_t fg_rgb, bg_rgb; /* packed 0xRRGGBB, -1 = unset/use palette */
	short pair;
	bool used;
} TruecolorPair;
static TruecolorPair truecolor_cache[TRUECOLOR_PAIRS_MAX];
static int truecolor_cache_next;

/*
 * Open-addressing hash index over truecolor_cache, keyed by (fg_rgb,
 * bg_rgb), so vt_color_get_truecolor() can look up an existing pair in
 * O(1) average instead of linearly scanning up to TRUECOLOR_PAIRS_MAX
 * entries per cell on every redraw -- that scan was the main cost of
 * truecolor support on a busy, colorful screen (a full-screen redraw
 * with many distinct colors could run it thousands of times per
 * frame). Sized well above TRUECOLOR_PAIRS_MAX to keep the load factor
 * low; -1 marks an empty slot (a valid index into truecolor_cache is
 * always >= 0).
 */
#define TRUECOLOR_HASH_SIZE (TRUECOLOR_PAIRS_MAX * 4)
static int truecolor_hash_index[TRUECOLOR_HASH_SIZE];

static unsigned int truecolor_hash(int32_t fg_rgb, int32_t bg_rgb)
{
	/* simple, fast mix -- these are already well-distributed 24-bit
	 * values (or -1), collisions are handled by linear probing below
	 * regardless, so this doesn't need to be cryptographically sound. */
	unsigned int h = (unsigned int)fg_rgb * 2654435761u;
	h ^= (unsigned int)bg_rgb * 40503u;
	return h % TRUECOLOR_HASH_SIZE;
}

/* look up a cached pair by (fg_rgb, bg_rgb); returns the cache slot
 * index, or -1 if not found. */
static int truecolor_hash_find(int32_t fg_rgb, int32_t bg_rgb)
{
	unsigned int h = truecolor_hash(fg_rgb, bg_rgb);
	for (unsigned int i = 0; i < TRUECOLOR_HASH_SIZE; i++) {
		unsigned int idx = (h + i) % TRUECOLOR_HASH_SIZE;
		int slot = truecolor_hash_index[idx];
		if (slot == -1)
			return -1; /* empty slot: not present */
		if (truecolor_cache[slot].used && truecolor_cache[slot].fg_rgb == fg_rgb
		    && truecolor_cache[slot].bg_rgb == bg_rgb)
			return slot;
	}
	return -1;
}

/* insert (fg_rgb, bg_rgb) -> slot into the hash index. */
static void truecolor_hash_insert(int32_t fg_rgb, int32_t bg_rgb, int slot)
{
	unsigned int h = truecolor_hash(fg_rgb, bg_rgb);
	for (unsigned int i = 0; i < TRUECOLOR_HASH_SIZE; i++) {
		unsigned int idx = (h + i) % TRUECOLOR_HASH_SIZE;
		if (truecolor_hash_index[idx] == -1) {
			truecolor_hash_index[idx] = slot;
			return;
		}
	}
	/* hash table somehow full (shouldn't happen, it's 4x oversized
	 * relative to the cache it indexes) -- silently drop the index
	 * entry; the cache slot itself still works, just falls back to a
	 * fresh allocation next time instead of a hit. */
}

/* remove whatever (fg_rgb, bg_rgb) pair currently occupies this slot
 * from the hash index, before the slot is evicted/reused for a
 * different color. */
static void truecolor_hash_remove(int slot)
{
	if (!truecolor_cache[slot].used)
		return;
	int32_t fg_rgb = truecolor_cache[slot].fg_rgb;
	int32_t bg_rgb = truecolor_cache[slot].bg_rgb;
	unsigned int h = truecolor_hash(fg_rgb, bg_rgb);
	for (unsigned int i = 0; i < TRUECOLOR_HASH_SIZE; i++) {
		unsigned int idx = (h + i) % TRUECOLOR_HASH_SIZE;
		if (truecolor_hash_index[idx] == slot) {
			truecolor_hash_index[idx] = -1;
			/*
			 * Linear-probe deletion hole: re-insert every entry
			 * that follows in this probe run, since later lookups
			 * may have skipped past idx assuming it was occupied.
			 * The cache is small (<=512 real entries) and evictions
			 * are rare relative to lookups, so this O(run length)
			 * fixup is cheap in practice.
			 */
			unsigned int j = (idx + 1) % TRUECOLOR_HASH_SIZE;
			while (truecolor_hash_index[j] != -1) {
				int moved = truecolor_hash_index[j];
				truecolor_hash_index[j] = -1;
				truecolor_hash_insert(truecolor_cache[moved].fg_rgb,
				                       truecolor_cache[moved].bg_rgb, moved);
				j = (j + 1) % TRUECOLOR_HASH_SIZE;
			}
			return;
		}
	}
}
static int truecolor_cache_size;  /* actual usable cache entries this run, <= TRUECOLOR_PAIRS_MAX */
static int truecolor_cache_count; /* entries currently in use */
static short truecolor_pair_base;  /* first pair index truecolor may use */
static int truecolor_color_base;   /* first extended color index truecolor may use (ext-colors builds only) */

typedef struct {
	wchar_t text;
	attr_t attr;
	short fg;
	short bg;
	int32_t fg_rgb; /* packed 0xRRGGBB, -1 if this cell has no truecolor fg */
	int32_t bg_rgb; /* packed 0xRRGGBB, -1 if this cell has no truecolor bg */
} Cell;

typedef struct {
	Cell *cells;
	unsigned dirty:1;
} Row;

/* Buffer holding the current terminal window content (as an array) as well
 * as the scroll back buffer content (as a circular/ring buffer).
 *
 * If new content is added to terminal the view port slides down and the
 * previously top most line is moved into the scroll back buffer at postion
 * scroll_index. This index will eventually wrap around and thus overwrite
 * the oldest lines.
 *
 * In the scenerio below a scroll up has been performed. That is 'scroll_above'
 * lines still lie above the current view port. Further scrolling up will show
 * them. Similarly 'scroll_below' is the amount of lines below the current
 * viewport.
 *
 * The function buffer_boundary sets the row pointers to the start/end range
 * of the section delimiting the region before/after the viewport. The functions
 * buffer_row_{first,last} return the first/last logical row. And
 * buffer_row_{next,prev} allows to iterate over the logical lines in either
 * direction.
 *
 *                                     scroll back buffer
 *
 *                      scroll_buf->+----------------+-----+
 *                                  |                |     | ^  \
 *                                  |     before     |     | |  |
 *    current terminal content      |    viewport    |     | |  |
 *                                  |                |     |    |
 *    +----------------+-----+\     |                |     | s   > scroll_above
 *  ^ |                |  i  | \    |                |  i  | c  |
 *  | |                |  n  |  \   |                |  n  | r  |
 *    |                |  v  |   \  |                |  v  | o  |
 *  r |                |  i  |    \ |                |  i  | l  /
 *  o |    viewport    |  s  |     >|<- scroll_index |  s  | l  \
 *  w |                |  i  |    / |                |  i  |    |
 *  s |                |  b  |   /  |     after      |  b  | s   > scroll_below
 *    |                |  l  |  /   |    viewport    |  l  | i  |
 *  v |                |  e  | /    |                |  e  | z  /
 *    +----------------+-----+/     |     unused     |     | e
 *     <-    maxcols      ->        |   scroll back  |     |
 *     <-    cols    ->             |     buffer     |     | |
 *                                  |                |     | |
 *                                  |                |     | v
 *          roll_buf + scroll_size->+----------------+-----+
 *                                   <-    maxcols       ->
 *                                   <-    cols    ->
 */
typedef struct {
	Row *lines;            /* array of Row pointers of size 'rows' */
	Row *curs_row;         /* row on which the cursor currently resides */
	Row *scroll_buf;       /* a ring buffer holding the scroll back content */
	Row *scroll_top;       /* row in lines where scrolling region starts */
	Row *scroll_bot;       /* row in lines where scrolling region ends */
	bool *tabs;            /* a boolean flag for each column whether it is a tab */
	int scroll_size;       /* maximal capacity of scroll back buffer (in lines) */
	int scroll_index;      /* current index into the ring buffer */
	int scroll_above;      /* number of lines above current viewport */
	int scroll_below;      /* number of lines below current viewport */
	int rows, cols;        /* current dimension of buffer */
	int maxcols;           /* allocated cells (maximal cols over time) */
	attr_t curattrs, savattrs; /* current and saved attributes for cells */
	int curs_col;          /* current cursor column (zero based) */
	int curs_srow, curs_scol; /* saved cursor row/colmn (zero based) */
	short curfg, curbg;    /* current fore and background colors */
	short savfg, savbg;    /* saved colors */
	int32_t curfg_rgb, curbg_rgb; /* current truecolor fg/bg, -1 if unset */
	int32_t savfg_rgb, savbg_rgb; /* saved truecolor fg/bg */
} Buffer;

struct Vt {
	Buffer buffer_normal;    /* normal screen buffer */
	Buffer buffer_alternate; /* alternate screen buffer */
	Buffer *buffer;          /* currently active buffer (one of the above) */
	attr_t defattrs;         /* attributes to use for normal/empty cells */
	short deffg, defbg;      /* colors to use for back normal/empty cells (white/black) */
	int pty;                 /* master side pty file descriptor */
	pid_t pid;               /* process id of the process running in this vt */
	/* flags */
	unsigned seen_input:1;
	unsigned insert:1;
	unsigned escaped:1;
	unsigned curshid:1;
	unsigned curskeymode:1;
	unsigned bell:1;
	unsigned relposmode:1;
	unsigned mousetrack:1;
	unsigned pastemode:1; /* bracketed paste mode (CSI ?2004h/l) requested by the child */
	unsigned graphmode:1;
	unsigned savgraphmode:1;
	bool charsets[2];
	/* buffers and parsing state */
	char rbuf[BUFSIZ];
	char ebuf[BUFSIZ];
	unsigned int rlen, elen;
	int srow, scol;          /* last known offset to display start row, start column */
	char title[256];         /* xterm style window title */
	vt_title_handler_t title_handler; /* hook which is called when title changes */
	vt_urgent_handler_t urgent_handler; /* hook which is called upon bell */
	void *data;              /* user supplied data */
};

static const char *keytable[KEY_MAX+1] = {
	[KEY_ENTER]     = "\r",
	['\n']          = "\n",
	/* for the arrow keys the CSI / SS3 sequences are not stored here
	 * because they depend on the current cursor terminal mode
	 */
	[KEY_UP]        = "A",
	[KEY_DOWN]      = "B",
	[KEY_RIGHT]     = "C",
	[KEY_LEFT]      = "D",
#ifdef KEY_SUP
	[KEY_SUP]       = "\e[1;2A",
#endif
#ifdef KEY_SDOWN
	[KEY_SDOWN]     = "\e[1;2B",
#endif
	[KEY_SRIGHT]    = "\e[1;2C",
	[KEY_SLEFT]     = "\e[1;2D",
	[KEY_BACKSPACE] = "\177",
	[KEY_IC]        = "\e[2~",
	[KEY_DC]        = "\e[3~",
	[KEY_PPAGE]     = "\e[5~",
	[KEY_NPAGE]     = "\e[6~",
	[KEY_HOME]      = "\e[7~",
	[KEY_END]       = "\e[8~",
	[KEY_BTAB]      = "\e[Z",
	[KEY_SUSPEND]   = "\x1A",  /* Ctrl+Z gets mapped to this */
	[KEY_F(1)]      = "\e[11~",
	[KEY_F(2)]      = "\e[12~",
	[KEY_F(3)]      = "\e[13~",
	[KEY_F(4)]      = "\e[14~",
	[KEY_F(5)]      = "\e[15~",
	[KEY_F(6)]      = "\e[17~",
	[KEY_F(7)]      = "\e[18~",
	[KEY_F(8)]      = "\e[19~",
	[KEY_F(9)]      = "\e[20~",
	[KEY_F(10)]     = "\e[21~",
	[KEY_F(11)]     = "\e[23~",
	[KEY_F(12)]     = "\e[24~",
	[KEY_F(13)]     = "\e[23~",
	[KEY_F(14)]     = "\e[24~",
	[KEY_F(15)]     = "\e[25~",
	[KEY_F(16)]     = "\e[26~",
	[KEY_F(17)]     = "\e[28~",
	[KEY_F(18)]     = "\e[29~",
	[KEY_F(19)]     = "\e[31~",
	[KEY_F(20)]     = "\e[32~",
	[KEY_F(21)]     = "\e[33~",
	[KEY_F(22)]     = "\e[34~",
	[KEY_RESIZE]    = "",
#ifdef KEY_EVENT
	[KEY_EVENT]     = "",
#endif
};

static void puttab(Vt *t, int count);
static void process_nonprinting(Vt *t, wchar_t wc);
static void send_curs(Vt *t);
static int32_t palette_to_rgb(short idx);
static short vt_color_get_truecolor(int32_t fg_rgb, int32_t bg_rgb);

__attribute__ ((const))
static attr_t build_attrs(attr_t curattrs)
{
	return ((curattrs & ~A_COLOR) | COLOR_PAIR(curattrs & 0xff))
	    >> NCURSES_ATTR_SHIFT;
}

static void row_set(Row *row, int start, int len, Buffer *t)
{
	Cell cell = {
		.text = L'\0',
		.attr = t ? build_attrs(t->curattrs) : 0,
		.fg = t ? t->curfg : -1,
		.bg = t ? t->curbg : -1,
		.fg_rgb = t ? t->curfg_rgb : -1,
		.bg_rgb = t ? t->curbg_rgb : -1,
	};

	for (int i = start; i < len + start; i++)
		row->cells[i] = cell;
	row->dirty = true;
}

static void row_roll(Row *start, Row *end, int count)
{
	int n = end - start;

	count %= n;
	if (count < 0)
		count += n;

	if (count) {
		char buf[count * sizeof(Row)];
		memcpy(buf, start, count * sizeof(Row));
		memmove(start, start + count, (n - count) * sizeof(Row));
		memcpy(end - count, buf, count * sizeof(Row));
		for (Row *row = start; row < end; row++)
			row->dirty = true;
	}
}

static void buffer_clear(Buffer *b)
{
	Cell cell = {
		.text = L'\0',
		.attr = A_NORMAL,
		.fg = -1,
		.bg = -1,
		.fg_rgb = -1,
		.bg_rgb = -1,
	};

	for (int i = 0; i < b->rows; i++) {
		Row *row = b->lines + i;
		for (int j = 0; j < b->cols; j++) {
			row->cells[j] = cell;
			row->dirty = true;
		}
	}
}

static void buffer_free(Buffer *b)
{
	for (int i = 0; i < b->rows; i++)
		free(b->lines[i].cells);
	free(b->lines);
	for (int i = 0; i < b->scroll_size; i++)
		free(b->scroll_buf[i].cells);
	free(b->scroll_buf);
	free(b->tabs);
}

static void buffer_scroll(Buffer *b, int s)
{
	/* work in screenfuls */
	int ssz = b->scroll_bot - b->scroll_top;
	if (s > ssz) {
		buffer_scroll(b, ssz);
		buffer_scroll(b, s - ssz);
		return;
	}
	if (s < -ssz) {
		buffer_scroll(b, -ssz);
		buffer_scroll(b, s + ssz);
		return;
	}

	b->scroll_above += s;
	if (b->scroll_above >= b->scroll_size)
		b->scroll_above = b->scroll_size;

	if (s > 0 && b->scroll_size) {
		for (int i = 0; i < s; i++) {
			Row tmp = b->scroll_top[i];
			b->scroll_top[i] = b->scroll_buf[b->scroll_index];
			b->scroll_buf[b->scroll_index] = tmp;

			b->scroll_index++;
			if (b->scroll_index == b->scroll_size)
				b->scroll_index = 0;
		}
	}
	row_roll(b->scroll_top, b->scroll_bot, s);
	if (s < 0 && b->scroll_size) {
		for (int i = (-s) - 1; i >= 0; i--) {
			b->scroll_index--;
			if (b->scroll_index == -1)
				b->scroll_index = b->scroll_size - 1;

			Row tmp = b->scroll_top[i];
			b->scroll_top[i] = b->scroll_buf[b->scroll_index];
			b->scroll_buf[b->scroll_index] = tmp;
			b->scroll_top[i].dirty = true;
		}
	}
}

static void buffer_resize(Buffer *b, int rows, int cols)
{
	Row *lines = b->lines;

	if (b->rows != rows) {
		if (b->curs_row >= lines + rows) {
			/* scroll up instead of simply chopping off bottom */
			buffer_scroll(b, (b->curs_row - b->lines) - rows + 1);
		}
		while (b->rows > rows) {
			free(lines[b->rows - 1].cells);
			b->rows--;
		}

		lines = realloc(lines, sizeof(Row) * rows);
	}

	if (b->maxcols < cols) {
		for (int row = 0; row < b->rows; row++) {
			lines[row].cells = realloc(lines[row].cells, sizeof(Cell) * cols);
			if (b->cols < cols)
				row_set(lines + row, b->cols, cols - b->cols, NULL);
			lines[row].dirty = true;
		}
		Row *sbuf = b->scroll_buf;
		for (int row = 0; row < b->scroll_size; row++) {
			sbuf[row].cells = realloc(sbuf[row].cells, sizeof(Cell) * cols);
			if (b->cols < cols)
				row_set(sbuf + row, b->cols, cols - b->cols, NULL);
		}
		b->tabs = realloc(b->tabs, sizeof(*b->tabs) * cols);
		for (int col = b->cols; col < cols; col++)
			b->tabs[col] = !(col & 7);
		b->maxcols = cols;
		b->cols = cols;
	} else if (b->cols != cols) {
		for (int row = 0; row < b->rows; row++)
			lines[row].dirty = true;
		b->cols = cols;
	}

	int deltarows = 0;
	if (b->rows < rows) {
		while (b->rows < rows) {
			lines[b->rows].cells = calloc(b->maxcols, sizeof(Cell));
			row_set(lines + b->rows, 0, b->maxcols, b);
			b->rows++;
		}

		/* prepare for backfill */
		if (b->curs_row >= b->scroll_bot - 1) {
			deltarows = b->lines + rows - b->curs_row - 1;
			if (deltarows > b->scroll_above)
				deltarows = b->scroll_above;
		}
	}

	b->curs_row += lines - b->lines;
	b->scroll_top = lines;
	b->scroll_bot = lines + rows;
	b->lines = lines;

	/* perform backfill */
	if (deltarows > 0) {
		buffer_scroll(b, -deltarows);
		b->curs_row += deltarows;
	}
}

static bool buffer_init(Buffer *b, int rows, int cols, int scroll_size)
{
	b->curattrs = A_NORMAL;	/* white text over black background */
	b->curfg = b->curbg = -1;
	b->curfg_rgb = b->curbg_rgb = -1;
	if (scroll_size < 0)
		scroll_size = 0;
	if (scroll_size && !(b->scroll_buf = calloc(scroll_size, sizeof(Row))))
		return false;
	b->scroll_size = scroll_size;
	buffer_resize(b, rows, cols);
	return true;
}

static void buffer_boundry(Buffer *b, Row **bs, Row **be, Row **as, Row **ae) {
	if (bs)
		*bs = NULL;
	if (be)
		*be = NULL;
	if (as)
		*as = NULL;
	if (ae)
		*ae = NULL;
	if (!b->scroll_size)
		return;

	if (b->scroll_above) {
		if (bs)
			*bs = &b->scroll_buf[(b->scroll_index - b->scroll_above + b->scroll_size) % b->scroll_size];
		if (be)
			*be = &b->scroll_buf[(b->scroll_index-1 + b->scroll_size) % b->scroll_size];
	}
	if (b->scroll_below) {
		if (as)
			*as = &b->scroll_buf[b->scroll_index];
		if (ae)
			*ae = &b->scroll_buf[(b->scroll_index + b->scroll_below-1) % b->scroll_size];
	}
}

static Row *buffer_row_first(Buffer *b) {
	Row *bstart;
	if (!b->scroll_size || !b->scroll_above)
		return b->lines;
	buffer_boundry(b, &bstart, NULL, NULL, NULL);
	return bstart;
}

static Row *buffer_row_last(Buffer *b) {
	Row *aend;
	if (!b->scroll_size || !b->scroll_below)
		return b->lines + b->rows - 1;
	buffer_boundry(b, NULL, NULL, NULL, &aend);
	return aend;
}

static Row *buffer_row_next(Buffer *b, Row *row)
{
	Row *before_start, *before_end, *after_start, *after_end;
	Row *first = b->lines, *last = b->lines + b->rows - 1;

	if (!row)
		return NULL;

	buffer_boundry(b, &before_start, &before_end, &after_start, &after_end);

	if (row >= first && row < last)
		return ++row;
	if (row == last)
		return after_start;
	if (row == before_end)
		return first;
	if (row == after_end)
		return NULL;
	if (row == &b->scroll_buf[b->scroll_size - 1])
		return b->scroll_buf;
	return ++row;
}

static Row *buffer_row_prev(Buffer *b, Row *row)
{
	Row *before_start, *before_end, *after_start, *after_end;
	Row *first = b->lines, *last = b->lines + b->rows - 1;

	if (!row)
		return NULL;

	buffer_boundry(b, &before_start, &before_end, &after_start, &after_end);

	if (row > first && row <= last)
		return --row;
	if (row == first)
		return before_end;
	if (row == before_start)
		return NULL;
	if (row == after_start)
		return last;
	if (row == b->scroll_buf)
		return &b->scroll_buf[b->scroll_size - 1];
	return --row;
}

static void cursor_clamp(Vt *t)
{
	Buffer *b = t->buffer;
	Row *lines = t->relposmode ? b->scroll_top : b->lines;
	int rows = t->relposmode ? b->scroll_bot - b->scroll_top : b->rows;

	if (b->curs_row < lines)
		b->curs_row = lines;
	if (b->curs_row >= lines + rows)
		b->curs_row = lines + rows - 1;
	if (b->curs_col < 0)
		b->curs_col = 0;
	if (b->curs_col >= b->cols)
		b->curs_col = b->cols - 1;
}

static void cursor_line_down(Vt *t)
{
	Buffer *b = t->buffer;
	row_set(b->curs_row, b->cols, b->maxcols - b->cols, NULL);
	b->curs_row++;
	if (b->curs_row < b->scroll_bot)
		return;

	vt_noscroll(t);

	b->curs_row = b->scroll_bot - 1;
	buffer_scroll(b, 1);
	row_set(b->curs_row, 0, b->cols, b);
}

static void cursor_save(Vt *t)
{
	Buffer *b = t->buffer;
	b->curs_srow = b->curs_row - b->lines;
	b->curs_scol = b->curs_col;
}

static void cursor_restore(Vt *t)
{
	Buffer *b = t->buffer;
	b->curs_row = b->lines + b->curs_srow;
	b->curs_col = b->curs_scol;
	cursor_clamp(t);
}

static void attributes_save(Vt *t)
{
	Buffer *b = t->buffer;
	b->savattrs = b->curattrs;
	b->savfg = b->curfg;
	b->savbg = b->curbg;
	b->savfg_rgb = b->curfg_rgb;
	b->savbg_rgb = b->curbg_rgb;
	t->savgraphmode = t->graphmode;
}

static void attributes_restore(Vt *t)
{
	Buffer *b = t->buffer;
	b->curattrs = b->savattrs;
	b->curfg = b->savfg;
	b->curbg = b->savbg;
	b->curfg_rgb = b->savfg_rgb;
	b->curbg_rgb = b->savbg_rgb;
	t->graphmode = t->savgraphmode;
}

static void new_escape_sequence(Vt *t)
{
	t->escaped = true;
	t->elen = 0;
	t->ebuf[0] = '\0';
}

static void cancel_escape_sequence(Vt *t)
{
	t->escaped = false;
	t->elen = 0;
	t->ebuf[0] = '\0';
}

static bool is_valid_csi_ender(int c)
{
	return (c >= 'a' && c <= 'z')
	    || (c >= 'A' && c <= 'Z')
	    || (c == '@' || c == '`');
}

/* interprets a 'set attribute' (SGR) CSI escape sequence */
static void interpret_csi_sgr(Vt *t, int param[], int pcount)
{
	Buffer *b = t->buffer;
	if (pcount == 0) {
		/* special case: reset attributes */
		b->curattrs = A_NORMAL;
		b->curfg = b->curbg = -1;
		b->curfg_rgb = b->curbg_rgb = -1;
		return;
	}

	for (int i = 0; i < pcount; i++) {
		switch (param[i]) {
		case 0:
			b->curattrs = A_NORMAL;
			b->curfg = b->curbg = -1;
			b->curfg_rgb = b->curbg_rgb = -1;
			break;
		case 1:
			b->curattrs |= A_BOLD;
			break;
		case 2:
			b->curattrs |= A_DIM;
			break;
#ifdef A_ITALIC
		case 3:
			b->curattrs |= A_ITALIC;
			break;
#endif
		case 4:
			b->curattrs |= A_UNDERLINE;
			break;
		case 5:
			b->curattrs |= A_BLINK;
			break;
		case 7:
			b->curattrs |= A_REVERSE;
			break;
		case 8:
			b->curattrs |= A_INVIS;
			break;
		case 22:
			b->curattrs &= ~(A_BOLD | A_DIM);
			break;
#ifdef A_ITALIC
		case 23:
			b->curattrs &= ~A_ITALIC;
			break;
#endif
		case 24:
			b->curattrs &= ~A_UNDERLINE;
			break;
		case 25:
			b->curattrs &= ~A_BLINK;
			break;
		case 27:
			b->curattrs &= ~A_REVERSE;
			break;
		case 28:
			b->curattrs &= ~A_INVIS;
			break;
		case 30 ... 37:	/* fg */
			b->curfg = param[i] - 30;
			b->curfg_rgb = -1;
			break;
		case 38:
			if ((i + 4) < pcount && param[i + 1] == 2) {
				/* 38;2;r;g;b -- 24-bit truecolor foreground */
				int r = param[i + 2] & 0xff;
				int g = param[i + 3] & 0xff;
				int bl = param[i + 4] & 0xff;
				b->curfg_rgb = (r << 16) | (g << 8) | bl;
				i += 4;
			} else if ((i + 2) < pcount && param[i + 1] == 5) {
				/* 38;5;n -- 256-color palette foreground */
				b->curfg = param[i + 2];
				b->curfg_rgb = -1;
				i += 2;
			}
			break;
		case 39:
			b->curfg = -1;
			b->curfg_rgb = -1;
			break;
		case 40 ... 47:	/* bg */
			b->curbg = param[i] - 40;
			b->curbg_rgb = -1;
			break;
		case 48:
			if ((i + 4) < pcount && param[i + 1] == 2) {
				/* 48;2;r;g;b -- 24-bit truecolor background */
				int r = param[i + 2] & 0xff;
				int g = param[i + 3] & 0xff;
				int bl = param[i + 4] & 0xff;
				b->curbg_rgb = (r << 16) | (g << 8) | bl;
				i += 4;
			} else if ((i + 2) < pcount && param[i + 1] == 5) {
				/* 48;5;n -- 256-color palette background */
				b->curbg = param[i + 2];
				b->curbg_rgb = -1;
				i += 2;
			}
			break;
		case 49:
			b->curbg = -1;
			b->curbg_rgb = -1;
			break;
		case 90 ... 97:	/* hi fg */
			b->curfg = param[i] - 82;
			b->curfg_rgb = -1;
			break;
		case 100 ... 107: /* hi bg */
			b->curbg = param[i] - 92;
			b->curbg_rgb = -1;
			break;
		default:
			break;
		}
	}
}

/* interprets an 'erase display' (ED) escape sequence */
static void interpret_csi_ed(Vt *t, int param[], int pcount)
{
	Row *row, *start, *end;
	Buffer *b = t->buffer;

	attributes_save(t);
	b->curattrs = A_NORMAL;
	b->curfg = b->curbg = -1;

	if (pcount && param[0] == 2) {
		start = b->lines;
		end = b->lines + b->rows;
	} else if (pcount && param[0] == 1) {
		start = b->lines;
		end = b->curs_row;
		row_set(b->curs_row, 0, b->curs_col + 1, b);
	} else {
		row_set(b->curs_row, b->curs_col, b->cols - b->curs_col, b);
		start = b->curs_row + 1;
		end = b->lines + b->rows;
	}

	for (row = start; row < end; row++)
		row_set(row, 0, b->cols, b);

	attributes_restore(t);
}

/* interprets a 'move cursor' (CUP) escape sequence */
static void interpret_csi_cup(Vt *t, int param[], int pcount)
{
	Buffer *b = t->buffer;
	Row *lines = t->relposmode ? b->scroll_top : b->lines;

	if (pcount == 0) {
		b->curs_row = lines;
		b->curs_col = 0;
	} else if (pcount == 1) {
		b->curs_row = lines + param[0] - 1;
		b->curs_col = 0;
	} else {
		b->curs_row = lines + param[0] - 1;
		b->curs_col = param[1] - 1;
	}

	cursor_clamp(t);
}

/* Interpret the 'relative mode' sequences: CUU, CUD, CUF, CUB, CNL,
 * CPL, CHA, HPR, VPA, VPR, HPA */
static void interpret_csi_c(Vt *t, char verb, int param[], int pcount)
{
	Buffer *b = t->buffer;
	int n = (pcount && param[0] > 0) ? param[0] : 1;

	switch (verb) {
	case 'A':
		b->curs_row -= n;
		break;
	case 'B':
	case 'e':
		b->curs_row += n;
		break;
	case 'C':
	case 'a':
		b->curs_col += n;
		break;
	case 'D':
		b->curs_col -= n;
		break;
	case 'E':
		b->curs_row += n;
		b->curs_col = 0;
		break;
	case 'F':
		b->curs_row -= n;
		b->curs_col = 0;
		break;
	case 'G':
	case '`':
		b->curs_col = n - 1;
		break;
	case 'd':
		b->curs_row = b->lines + n - 1;
		break;
	}

	cursor_clamp(t);
}

/* Interpret the 'erase line' escape sequence */
static void interpret_csi_el(Vt *t, int param[], int pcount)
{
	Buffer *b = t->buffer;
	switch (pcount ? param[0] : 0) {
	case 1:
		row_set(b->curs_row, 0, b->curs_col + 1, b);
		break;
	case 2:
		row_set(b->curs_row, 0, b->cols, b);
		break;
	default:
		row_set(b->curs_row, b->curs_col, b->cols - b->curs_col, b);
		break;
	}
}

/* Interpret the 'insert blanks' sequence (ICH) */
static void interpret_csi_ich(Vt *t, int param[], int pcount)
{
	Buffer *b = t->buffer;
	Row *row = b->curs_row;
	int n = (pcount && param[0] > 0) ? param[0] : 1;

	if (b->curs_col + n > b->cols)
		n = b->cols - b->curs_col;

	for (int i = b->cols - 1; i >= b->curs_col + n; i--)
		row->cells[i] = row->cells[i - n];

	row_set(row, b->curs_col, n, b);
}

/* Interpret the 'delete chars' sequence (DCH) */
static void interpret_csi_dch(Vt *t, int param[], int pcount)
{
	Buffer *b = t->buffer;
	Row *row = b->curs_row;
	int n = (pcount && param[0] > 0) ? param[0] : 1;

	if (b->curs_col + n > b->cols)
		n = b->cols - b->curs_col;

	for (int i = b->curs_col; i < b->cols - n; i++)
		row->cells[i] = row->cells[i + n];

	row_set(row, b->cols - n, n, b);
}

/* Interpret an 'insert line' sequence (IL) */
static void interpret_csi_il(Vt *t, int param[], int pcount)
{
	Buffer *b = t->buffer;
	int n = (pcount && param[0] > 0) ? param[0] : 1;

	if (b->curs_row + n >= b->scroll_bot) {
		for (Row *row = b->curs_row; row < b->scroll_bot; row++)
			row_set(row, 0, b->cols, b);
	} else {
		row_roll(b->curs_row, b->scroll_bot, -n);
		for (Row *row = b->curs_row; row < b->curs_row + n; row++)
			row_set(row, 0, b->cols, b);
	}
}

/* Interpret a 'delete line' sequence (DL) */
static void interpret_csi_dl(Vt *t, int param[], int pcount)
{
	Buffer *b = t->buffer;
	int n = (pcount && param[0] > 0) ? param[0] : 1;

	if (b->curs_row + n >= b->scroll_bot) {
		for (Row *row = b->curs_row; row < b->scroll_bot; row++)
			row_set(row, 0, b->cols, b);
	} else {
		row_roll(b->curs_row, b->scroll_bot, n);
		for (Row *row = b->scroll_bot - n; row < b->scroll_bot; row++)
			row_set(row, 0, b->cols, b);
	}
}

/* Interpret an 'erase characters' (ECH) sequence */
static void interpret_csi_ech(Vt *t, int param[], int pcount)
{
	Buffer *b = t->buffer;
	int n = (pcount && param[0] > 0) ? param[0] : 1;

	if (b->curs_col + n > b->cols)
		n = b->cols - b->curs_col;

	row_set(b->curs_row, b->curs_col, n, b);
}

/* Interpret a 'set scrolling region' (DECSTBM) sequence */
static void interpret_csi_decstbm(Vt *t, int param[], int pcount)
{
	Buffer *b = t->buffer;
	int new_top, new_bot;

	switch (pcount) {
	case 0:
		b->scroll_top = b->lines;
		b->scroll_bot = b->lines + b->rows;
		break;
	case 2:
		new_top = param[0] - 1;
		new_bot = param[1];

		/* clamp to bounds */
		if (new_top < 0)
			new_top = 0;
		if (new_top >= b->rows)
			new_top = b->rows - 1;
		if (new_bot < 0)
			new_bot = 0;
		if (new_bot >= b->rows)
			new_bot = b->rows;

		/* check for range validity */
		if (new_top < new_bot) {
			b->scroll_top = b->lines + new_top;
			b->scroll_bot = b->lines + new_bot;
		}
		break;
	default:
		return;	/* malformed */
	}
	b->curs_row = b->scroll_top;
	b->curs_col = 0;
}

static void interpret_csi_mode(Vt *t, int param[], int pcount, bool set)
{
	for (int i = 0; i < pcount; i++) {
		switch (param[i]) {
		case 4: /* insert/replace mode */
			t->insert = set;
			break;
		}
	}
}

static void interpret_csi_priv_mode(Vt *t, int param[], int pcount, bool set)
{
	for (int i = 0; i < pcount; i++) {
		switch (param[i]) {
		case 1: /* set application/normal cursor key mode (DECCKM) */
			t->curskeymode = set;
			break;
		case 6: /* set origin to relative/absolute (DECOM) */
			t->relposmode = set;
			break;
		case 25: /* make cursor visible/invisible (DECCM) */
			t->curshid = !set;
			break;
		case 1049: /* combine 1047 + 1048 */
			/*
			 * Save/restore the cursor position against whichever
			 * buffer is current *before* switching buffers below,
			 * matching mtm's ordering (mtm.c's `mode` handler calls
			 * sc/rc, THEN falls through to the buffer switch).
			 * cursor_save()/cursor_restore() both read/write
			 * t->buffer's own curs_srow/curs_scol -- buffer_normal
			 * and buffer_alternate each have independent copies of
			 * that field -- so doing the buffer switch first (the
			 * previous order here) meant:
			 *   enter (?1049h): switch to alternate, THEN save --
			 *     saves into buffer_alternate's slot, not normal's.
			 *   exit  (?1049l): switch to normal, THEN restore --
			 *     restores from buffer_normal's slot, which was
			 *     never written, instead of the position actually
			 *     saved on entry.
			 * The net effect: returning from any alternate-screen
			 * program (lazygit, nvim, etc) could leave the cursor
			 * at a stale/garbage row in the now-current normal
			 * buffer, which is exactly the kind of thing that reads
			 * as "the shell looks glitched" right after quitting
			 * such a program -- the next prompt draws starting from
			 * wherever that garbage cursor position was, not from
			 * where the shell actually left off.
			 */
			if (set)
				cursor_save(t);
			else
				cursor_restore(t);
			/* fall through */
		case 47:   /* use alternate/normal screen buffer */
		case 1047:
			if (!set)
				buffer_clear(&t->buffer_alternate);
			t->buffer = set ? &t->buffer_alternate : &t->buffer_normal;
			vt_dirty(t);
			break;
		case 1048: /* save/restore cursor */
			if (set)
				cursor_save(t);
			else
				cursor_restore(t);
			break;
		case 1000: /* enable/disable normal mouse tracking */
			t->mousetrack = set;
			break;
		case 2004: /* bracketed paste mode -- tracked so paste() in
		            * det.c knows whether to wrap pasted text in
		            * \e[200~ ... \e[201~. Without this, a program
		            * that requested bracketed paste (nvim does, by
		            * default) can't tell a multi-line paste from
		            * someone actually typing every character and
		            * pressing enter after each line -- which is
		            * exactly what causes auto-indent to fire on every
		            * line of pasted text, compounding indentation. */
			t->pastemode = set;
			break;
		}
	}
}

static void interpret_csi(Vt *t)
{
	Buffer *b = t->buffer;
	int csiparam[16];
	unsigned int param_count = 0;
	const char *p = t->ebuf + 1;
	char verb = t->ebuf[t->elen - 1];

	/* parse numeric parameters */
	for (p += (t->ebuf[1] == '?'); *p; p++) {
		if (IS_CONTROL(*p)) {
			process_nonprinting(t, *p);
		} else if (*p == ';') {
			if (param_count >= LENGTH(csiparam))
				return;	/* too long! */
			csiparam[param_count++] = 0;
		} else if (isdigit((unsigned char)*p)) {
			if (param_count == 0)
				csiparam[param_count++] = 0;
			csiparam[param_count - 1] *= 10;
			csiparam[param_count - 1] += *p - '0';
		}
	}

	if (t->ebuf[1] == '?') {
		switch (verb) {
		case 'h':
		case 'l': /* private set/reset mode */
			interpret_csi_priv_mode(t, csiparam, param_count, verb == 'h');
			break;
		}
		return;
	}

	/* delegate handling depending on command character (verb) */
	switch (verb) {
	case 'h':
	case 'l': /* set/reset mode */
		interpret_csi_mode(t, csiparam, param_count, verb == 'h');
		break;
	case 'm': /* set attribute */
		interpret_csi_sgr(t, csiparam, param_count);
		break;
	case 'J': /* erase display */
		interpret_csi_ed(t, csiparam, param_count);
		break;
	case 'H':
	case 'f': /* move cursor */
		interpret_csi_cup(t, csiparam, param_count);
		break;
	case 'A':
	case 'B':
	case 'C':
	case 'D':
	case 'E':
	case 'F':
	case 'G':
	case 'e':
	case 'a':
	case 'd':
	case '`': /* relative move */
		interpret_csi_c(t, verb, csiparam, param_count);
		break;
	case 'K': /* erase line */
		interpret_csi_el(t, csiparam, param_count);
		break;
	case '@': /* insert characters */
		interpret_csi_ich(t, csiparam, param_count);
		break;
	case 'P': /* delete characters */
		interpret_csi_dch(t, csiparam, param_count);
		break;
	case 'L': /* insert lines */
		interpret_csi_il(t, csiparam, param_count);
		break;
	case 'M': /* delete lines */
		interpret_csi_dl(t, csiparam, param_count);
		break;
	case 'X': /* erase chars */
		interpret_csi_ech(t, csiparam, param_count);
		break;
	case 'S': /* SU: scroll up */
		vt_scroll(t, param_count ? -csiparam[0] : -1);
		break;
	case 'T': /* SD: scroll down */
		vt_scroll(t, param_count ? csiparam[0] : 1);
		break;
	case 'Z': /* CBT: cursor backward tabulation */
		puttab(t, param_count ? -csiparam[0] : -1);
		break;
	case 'g': /* TBC: tabulation clear */
		switch (param_count ? csiparam[0] : 0) {
		case 0:
			b->tabs[b->curs_col] = false;
			break;
		case 3:
			memset(b->tabs, 0, sizeof(*b->tabs) * b->maxcols);
			break;
		}
		break;
	case 'r': /* set scrolling region */
		interpret_csi_decstbm(t, csiparam, param_count);
		break;
	case 's': /* save cursor location */
		cursor_save(t);
		break;
	case 'u': /* restore cursor location */
		cursor_restore(t);
		break;
	case 'n': /* query cursor location */
		if (param_count == 1 && csiparam[0] == 6)
			send_curs(t);
		break;
	case 'c': /* DA1/DA2: device attributes -- nvim >=0.12 sends this
	           * at startup as part of terminal-capability detection
	           * (same family as the cursor-position DSR probe); with
	           * no reply at all (dvtm/det never implemented this),
	           * nvim falls back to its slow-terminal path and shows
	           * the "Did not detect ... response" / ttyfast warning.
	           * \e[c or \e[0c -> DA1 (primary attributes): reply
	           * \e[?1;2c, the same "VT100 with AVO" identification
	           * mtm uses, which is enough for nvim/most clients to
	           * conclude the terminal is real and answers queries.
	           * \e[>c -> DA2 (secondary attributes, terminal
	           * type/firmware version): reply \e[>1;10;0c, again
	           * matching mtm's reply verbatim since it's a value
	           * nvim doesn't actually validate beyond "did something
	           * answer". */
		if (t->ebuf[1] == '>')
			vt_write(t, "\033[>1;10;0c", 10);
		else
			vt_write(t, "\033[?1;2c", 7);
		break;
	default:
		break;
	}
}

/* Interpret an 'index' (IND) sequence */
static void interpret_csi_ind(Vt *t)
{
	Buffer *b = t->buffer;
	if (b->curs_row < b->lines + b->rows - 1)
		b->curs_row++;
}

/* Interpret a 'reverse index' (RI) sequence */
static void interpret_csi_ri(Vt *t)
{
	Buffer *b = t->buffer;
	if (b->curs_row > b->scroll_top)
		b->curs_row--;
	else {
		row_roll(b->scroll_top, b->scroll_bot, -1);
		row_set(b->scroll_top, 0, b->cols, b);
	}
}

/* Interpret a 'next line' (NEL) sequence */
static void interpret_csi_nel(Vt *t)
{
	Buffer *b = t->buffer;
	if (b->curs_row < b->lines + b->rows - 1) {
		b->curs_row++;
		b->curs_col = 0;
	}
}

/* Interpret a 'select character set' (SCS) sequence */
static void interpret_csi_scs(Vt *t)
{
	/* ESC ( sets G0, ESC ) sets G1 */
	t->charsets[!!(t->ebuf[0] == ')')] = (t->ebuf[1] == '0');
	t->graphmode = t->charsets[0];
}

/* Interpret an 'operating system command' (OSC) sequence */
static void interpret_osc(Vt *t)
{
	/* ESC ] command ; data BEL
	 * ESC ] command ; data ESC \\
	 * Note that BEL or ESC \\ have already been replaced with NUL.
	 */
	char *data = NULL;
	int command = strtoul(t->ebuf + 1, &data, 10);
	if (data && *data == ';') {
		switch (command) {
		case 0: /* icon name and window title */
		case 2: /* window title */
			if (t->title_handler)
				t->title_handler(t, data+1);
			break;
		case 1: /* icon name */
			break;
		case 10: /* query/set default foreground color */
		case 11: /* query/set default background color */
			if (data[1] == '?') {
				/* query: reply with the color an empty/default
				 * cell would actually render with, since apps
				 * like nvim send this to decide light vs dark
				 * background -- answering keeps that detection
				 * fast instead of falling back to a timeout
				 * (nvim's "ttyfast"/E1568 warning is exactly
				 * this query going unanswered). */
				short idx = (command == 10) ? t->deffg : t->defbg;
				int32_t rgb;
				if (idx == -1)
					idx = (command == 10) ? default_fg : default_bg;
				rgb = palette_to_rgb(idx);
				if (rgb == -1)
					rgb = (command == 10) ? 0xffffff : 0x000000;
				char reply[32];
				int len = snprintf(reply, sizeof reply,
				                    "\033]%d;rgb:%02x%02x/%02x%02x/%02x%02xz",
				                    command,
				                    (int)((rgb >> 16) & 0xff), (int)((rgb >> 16) & 0xff),
				                    (int)((rgb >> 8) & 0xff), (int)((rgb >> 8) & 0xff),
				                    (int)(rgb & 0xff), (int)(rgb & 0xff));
				if (len > 0 && (size_t)len < sizeof reply) {
					reply[len - 1] = '\007'; /* terminate with BEL, the
					                          * format most readers (incl.
					                          * nvim) expect back */
					vt_write(t, reply, len);
				}
			}
			/* setting (data[1] != '?') is intentionally a no-op: det
			 * doesn't currently support changing its own default
			 * colors at runtime from inside a pane. */
			break;
		default:
#ifndef NDEBUG
			fprintf(stderr, "unknown OSC command: %d\n", command);
#endif
			break;
		}
	}
}

static void try_interpret_escape_seq(Vt *t)
{
	char lastchar = t->ebuf[t->elen - 1];

	if (!*t->ebuf)
		return;

	switch (*t->ebuf) {
	case '#': /* ignore DECDHL, DECSWL, DECDWL, DECHCP, DECFPP */
		if (t->elen == 2) {
			if (lastchar == '8') { /* DECALN */
				interpret_csi_ed(t, (int []){ 2 }, 1);
				goto handled;
			}
			goto cancel;
		}
		break;
	case '(':
	case ')':
		if (t->elen == 2) {
			interpret_csi_scs(t);
			goto handled;
		}
		break;
	case ']': /* OSC - operating system command */
		if (lastchar == '\a' ||
		   (lastchar == '\\' && t->elen >= 2 && t->ebuf[t->elen - 2] == '\e')) {
			t->elen -= lastchar == '\a' ? 1 : 2;
			t->ebuf[t->elen] = '\0';
			interpret_osc(t);
			goto handled;
		}
		break;
	case '[': /* CSI - control sequence introducer */
		if (is_valid_csi_ender(lastchar)) {
			interpret_csi(t);
			goto handled;
		}
		break;
	case '7': /* DECSC: save cursor and attributes */
		attributes_save(t);
		cursor_save(t);
		goto handled;
	case '8': /* DECRC: restore cursor and attributes */
		attributes_restore(t);
		cursor_restore(t);
		goto handled;
	case 'D': /* IND: index */
		interpret_csi_ind(t);
		goto handled;
	case 'M': /* RI: reverse index */
		interpret_csi_ri(t);
		goto handled;
	case 'E': /* NEL: next line */
		interpret_csi_nel(t);
		goto handled;
	case 'H': /* HTS: horizontal tab set */
		t->buffer->tabs[t->buffer->curs_col] = true;
		goto handled;
	default:
		goto cancel;
	}

	if (t->elen + 1 >= sizeof(t->ebuf)) {
cancel:
#ifndef NDEBUG
		fprintf(stderr, "cancelled: \\033");
		for (unsigned int i = 0; i < t->elen; i++) {
			if (isprint(t->ebuf[i])) {
				fputc(t->ebuf[i], stderr);
			} else {
				fprintf(stderr, "\\%03o", t->ebuf[i]);
			}
		}
		fputc('\n', stderr);
#endif
handled:
		cancel_escape_sequence(t);
	}
}

static void puttab(Vt *t, int count)
{
	Buffer *b = t->buffer;
	int direction = count >= 0 ? 1 : -1;
	for (int col = b->curs_col + direction; count; col += direction) {
		if (col < 0) {
			b->curs_col = 0;
			break;
		}
		if (col >= b->cols) {
			b->curs_col = b->cols - 1;
			break;
		}
		if (b->tabs[col]) {
			b->curs_col = col;
			count -= direction;
		}
	}
}

static void process_nonprinting(Vt *t, wchar_t wc)
{
	Buffer *b = t->buffer;
	switch (wc) {
	case '\e': /* ESC */
		new_escape_sequence(t);
		break;
	case '\a': /* BEL */
		if (t->urgent_handler)
			t->urgent_handler(t);
		break;
	case '\b': /* BS */
		if (b->curs_col > 0)
			b->curs_col--;
		break;
	case '\t': /* HT */
		puttab(t, 1);
		break;
	case '\r': /* CR */
		b->curs_col = 0;
		break;
	case '\v': /* VT */
	case '\f': /* FF */
	case '\n': /* LF */
		cursor_line_down(t);
		break;
	case '\016': /* SO: shift out, invoke the G1 character set */
		t->graphmode = t->charsets[1];
		break;
	case '\017': /* SI: shift in, invoke the G0 character set */
		t->graphmode = t->charsets[0];
		break;
	}
}

static void is_utf8_locale(void)
{
	const char *cset = nl_langinfo(CODESET);
	if (!cset)
		cset = "ANSI_X3.4-1968";
	is_utf8 = !strcmp(cset, "UTF-8");
}

static wchar_t get_vt100_graphic(char c)
{
	static char vt100_acs[] = "`afgjklmnopqrstuvwxyz{|}~";

	/*
	 * 5f-7e standard vt100
	 * 40-5e rxvt extension for extra curses acs chars
	 */
	static uint16_t const vt100_utf8[62] = {
		        0x2191, 0x2193, 0x2192, 0x2190, 0x2588, 0x259a, 0x2603, // 41-47
		     0,      0,      0,      0,      0,      0,      0,      0, // 48-4f
		     0,      0,      0,      0,      0,      0,      0,      0, // 50-57
		     0,      0,      0,      0,      0,      0,      0, 0x0020, // 58-5f
		0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1, // 60-67
		0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba, // 68-6f
		0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c, // 70-77
		0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,         // 78-7e
	};

	if (is_utf8)
		return vt100_utf8[c - 0x41];
	else if (strchr(vt100_acs, c))
		return NCURSES_ACS(c);
	return '\0';
}

static void put_wc(Vt *t, wchar_t wc)
{
	int width = 0;

	if (!t->seen_input) {
		t->seen_input = 1;
		kill(-t->pid, SIGWINCH);
	}

	if (t->escaped) {
		if (t->elen + 1 < sizeof(t->ebuf)) {
			t->ebuf[t->elen] = wc;
			t->ebuf[++t->elen] = '\0';
			try_interpret_escape_seq(t);
		} else {
			cancel_escape_sequence(t);
		}
	} else if (IS_CONTROL(wc)) {
		process_nonprinting(t, wc);
	} else {
		if (t->graphmode) {
			if (wc >= 0x41 && wc <= 0x7e) {
				wchar_t gc = get_vt100_graphic(wc);
				if (gc)
					wc = gc;
			}
			width = 1;
		} else if ((width = wcwidth(wc)) < 1) {
			width = 1;
		}
		Buffer *b = t->buffer;
		Cell blank_cell = { L'\0', build_attrs(b->curattrs), b->curfg, b->curbg, b->curfg_rgb, b->curbg_rgb };
		if (width == 2 && b->curs_col == b->cols - 1) {
			b->curs_row->cells[b->curs_col++] = blank_cell;
			b->curs_row->dirty = true;
		}

		if (b->curs_col >= b->cols) {
			b->curs_col = 0;
			cursor_line_down(t);
		}

		if (t->insert) {
			Cell *src = b->curs_row->cells + b->curs_col;
			Cell *dest = src + width;
			size_t len = b->cols - b->curs_col - width;
			memmove(dest, src, len * sizeof *dest);
		}

		b->curs_row->cells[b->curs_col] = blank_cell;
		b->curs_row->cells[b->curs_col++].text = wc;
		b->curs_row->dirty = true;
		if (width == 2)
			b->curs_row->cells[b->curs_col++] = blank_cell;
	}
}

int vt_process(Vt *t)
{
	int res;

	if (t->pty < 0) {
		errno = EINVAL;
		return -1;
	}

	/*
	 * Drain everything currently available on the (non-blocking) pty
	 * in this single call, rather than one BUFSIZ chunk per
	 * select()/vt_process() round trip -- a busy truecolor repaint can
	 * easily exceed one BUFSIZ, and previously each extra chunk meant
	 * another full select()+read()+draw()+doupdate() cycle before the
	 * screen actually caught up, which is exactly the kind of thing
	 * that reads as "lag" while scrolling or typing under a colorful
	 * scheme. Each iteration still parses its chunk fully (including
	 * carrying over a trailing incomplete multibyte sequence via
	 * t->rlen/memmove, same as before) before reading more, so this
	 * doesn't change parsing behavior or buffer-carryover semantics --
	 * it just stops yielding back to select() between chunks that were
	 * already sitting in the kernel's pty buffer.
	 */
	for (;;) {
		res = read(t->pty, t->rbuf + t->rlen, sizeof(t->rbuf) - t->rlen);
		if (res < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break; /* nothing more buffered right now */
			if (errno == EINTR)
				continue;
			/*
			 * A real error (e.g. EIO from a dead child) must still
			 * be reported even if this call already successfully
			 * read and processed some data first -- det.c's main
			 * loop checks `vt_process(...) < 0 && errno == EIO` to
			 * detect a dead client, and silently returning 0 here
			 * would delay that detection by up to one extra
			 * select() cycle. errno is preserved across the return
			 * since nothing between here and the caller touches it.
			 */
			return -1;
		}
		if (res == 0)
			break; /* EOF on a non-blocking fd that selected readable
			        * shouldn't really happen for a pty, but don't spin */

		t->rlen += res;
		unsigned int pos = 0;
		mbstate_t ps;
		memset(&ps, 0, sizeof(ps));

		while (pos < t->rlen) {
			wchar_t wc;
			ssize_t len;

			len = (ssize_t)mbrtowc(&wc, t->rbuf + pos, t->rlen - pos, &ps);
			if (len == -2) {
				t->rlen -= pos;
				memmove(t->rbuf, t->rbuf + pos, t->rlen);
				goto next_read;
			}

			if (len == -1) {
				len = 1;
				wc = t->rbuf[pos];
			}

			pos += len ? len : 1;
			put_wc(t, wc);
		}

		t->rlen -= pos;
		memmove(t->rbuf, t->rbuf + pos, t->rlen);

	next_read:
		/* rbuf is full and still ended mid-sequence/mid-chunk -- avoid
		 * an infinite loop if for some reason no bytes were consumed;
		 * normal operation always makes forward progress here. */
		if (t->rlen >= sizeof(t->rbuf))
			break;
	}

	return 0;
}

void vt_default_colors_set(Vt *t, attr_t attrs, short fg, short bg)
{
	t->defattrs = attrs;
	t->deffg = fg;
	t->defbg = bg;
}

Vt *vt_create(int rows, int cols, int scroll_size)
{
	if (rows <= 0 || cols <= 0)
		return NULL;

	Vt *t = calloc(1, sizeof(Vt));
	if (!t)
		return NULL;

	t->pty = -1;
	t->deffg = t->defbg = -1;
	t->buffer = &t->buffer_normal;

	if (!buffer_init(&t->buffer_normal, rows, cols, scroll_size) ||
	    !buffer_init(&t->buffer_alternate, rows, cols, 0)) {
		free(t);
		return NULL;
	}

	return t;
}

void vt_resize(Vt *t, int rows, int cols)
{
	struct winsize ws = { .ws_row = rows, .ws_col = cols };

	if (rows <= 0 || cols <= 0)
		return;

	vt_noscroll(t);
	buffer_resize(&t->buffer_normal, rows, cols);
	buffer_resize(&t->buffer_alternate, rows, cols);
	cursor_clamp(t);
	ioctl(t->pty, TIOCSWINSZ, &ws);
	kill(-t->pid, SIGWINCH);
}

void vt_destroy(Vt *t)
{
	if (!t)
		return;
	buffer_free(&t->buffer_normal);
	buffer_free(&t->buffer_alternate);
	close(t->pty);
	free(t);
}

void vt_dirty(Vt *t)
{
	Buffer *b = t->buffer;
	for (Row *row = b->lines, *end = row + b->rows; row < end; row++)
		row->dirty = true;
}

void vt_draw(Vt *t, WINDOW *win, int srow, int scol)
{
	Buffer *b = t->buffer;

	if (srow != t->srow || scol != t->scol) {
		vt_dirty(t);
		t->srow = srow;
		t->scol = scol;
	}

	for (int i = 0; i < b->rows; i++) {
		Row *row = b->lines + i;

		if (!row->dirty)
			continue;

		wmove(win, srow + i, scol);
		Cell *cell = NULL;
		for (int j = 0; j < b->cols; j++) {
			Cell *prev_cell = cell;
			cell = row->cells + j;
			if (!prev_cell || cell->attr != prev_cell->attr
			    || cell->fg != prev_cell->fg
			    || cell->bg != prev_cell->bg
			    || cell->fg_rgb != prev_cell->fg_rgb
			    || cell->bg_rgb != prev_cell->bg_rgb) {
				if (cell->attr == A_NORMAL)
					cell->attr = t->defattrs;
				if (cell->fg == -1)
					cell->fg = t->deffg;
				if (cell->bg == -1)
					cell->bg = t->defbg;
				wattrset(win, cell->attr << NCURSES_ATTR_SHIFT);
				if (cell->fg_rgb != -1 || cell->bg_rgb != -1) {
					int32_t fg_rgb = cell->fg_rgb;
					int32_t bg_rgb = cell->bg_rgb;
					/* a side without a truecolor override still
					 * needs an RGB value to pair against if it has
					 * a real palette index (e.g. truecolor fg over
					 * a 256-color bg) -- derive one so they pair
					 * correctly. If the side has NO palette index
					 * either (cell->fg/bg == -1, i.e. genuinely
					 * transparent/terminal-default), leave it as
					 * -1 and let vt_color_get_truecolor pass that
					 * through to ncurses' own default-color
					 * convention, rather than misreading -1 as a
					 * literal RGB triple (which reads as opaque
					 * white). */
					if (fg_rgb == -1 && cell->fg != -1)
						fg_rgb = palette_to_rgb(cell->fg);
					if (bg_rgb == -1 && cell->bg != -1)
						bg_rgb = palette_to_rgb(cell->bg);
					short pair = vt_color_get_truecolor(fg_rgb, bg_rgb);
					if (pair > 0) {
						wcolor_set(win, pair, NULL);
					} else {
						wcolor_set(win, vt_color_get(t, cell->fg, cell->bg), NULL);
					}
				} else {
					wcolor_set(win, vt_color_get(t, cell->fg, cell->bg), NULL);
				}
			}

			if (is_utf8 && cell->text >= 128) {
				char buf[MB_CUR_MAX + 1];
				size_t len = wcrtomb(buf, cell->text, NULL);
				if (len > 0) {
					waddnstr(win, buf, len);
					if (wcwidth(cell->text) > 1)
						j++;
				}
			} else {
				waddch(win, cell->text > ' ' ? cell->text : ' ');
			}
		}

		int x, y;
		getyx(win, y, x);
		(void)y;
		if (x && x < b->cols - 1)
			whline(win, ' ', b->cols - x);

		row->dirty = false;
	}

	wmove(win, srow + b->curs_row - b->lines, scol + b->curs_col);
}

void vt_scroll(Vt *t, int rows)
{
	Buffer *b = t->buffer;
	if (!b->scroll_size)
		return;
	if (rows < 0) { /* scroll back */
		if (rows < -b->scroll_above)
			rows = -b->scroll_above;
	} else { /* scroll forward */
		if (rows > b->scroll_below)
			rows = b->scroll_below;
	}
	buffer_scroll(b, rows);
	b->scroll_below -= rows;
}

void vt_noscroll(Vt *t)
{
	int scroll_below = t->buffer->scroll_below;
	if (scroll_below)
		vt_scroll(t, scroll_below);
}

pid_t vt_forkpty(Vt *t, const char *p, const char *argv[], const char *cwd, const char *env[], int *to, int *from)
{
	int vt2ed[2], ed2vt[2];
	struct winsize ws;
	ws.ws_row = t->buffer->rows;
	ws.ws_col = t->buffer->cols;
	ws.ws_xpixel = ws.ws_ypixel = 0;

	if (to && pipe(vt2ed)) {
		*to = -1;
		to = NULL;
	}
	if (from && pipe(ed2vt)) {
		*from = -1;
		from = NULL;
	}

	pid_t pid = forkpty(&t->pty, NULL, NULL, &ws);
	if (pid < 0)
		return -1;

	if (pid == 0) {
		setsid();

		sigset_t emptyset;
		sigemptyset(&emptyset);
		sigprocmask(SIG_SETMASK, &emptyset, NULL);

		if (to) {
			close(vt2ed[1]);
			dup2(vt2ed[0], STDIN_FILENO);
			close(vt2ed[0]);
		}

		if (from) {
			close(ed2vt[0]);
			dup2(ed2vt[1], STDOUT_FILENO);
			close(ed2vt[1]);
		}

		int maxfd = sysconf(_SC_OPEN_MAX);
		for (int fd = 3; fd < maxfd; fd++)
			if (close(fd) == -1 && errno == EBADF)
				break;

		for (const char **envp = env; envp && envp[0]; envp += 2)
			setenv(envp[0], envp[1], 1);
		setenv("TERM", vt_term, 1);

		if (cwd) {
			int err = chdir(cwd);
			if (err) {
				fprintf(stderr, "\nchdir() failed. ");
				perror(cwd);
				exit(1);
			}
		}

		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_flags = 0;
		sigemptyset(&sa.sa_mask);
		sa.sa_handler = SIG_DFL;
		sigaction(SIGPIPE, &sa, NULL);

		execvp(p, (char *const *)argv);
		fprintf(stderr, "\nexecv() failed.\nCommand: '%s'\n", argv[0]);
		exit(1);
	}

	if (to) {
		close(vt2ed[0]);
		*to = vt2ed[1];
	}

	if (from) {
		close(ed2vt[1]);
		*from = ed2vt[0];
	}

	/*
	 * Non-blocking so vt_process() can drain everything currently
	 * buffered on the pty in one call instead of one BUFSIZ (8KB)
	 * chunk per select()/vt_process() round trip. A single full-screen
	 * truecolor repaint can easily exceed 8KB, which previously meant
	 * several extra select()+read()+draw()+doupdate() cycles for what
	 * should have been one -- a real, measurable source of perceived
	 * input lag under a busy truecolor colorscheme.
	 */
	int flags = fcntl(t->pty, F_GETFL, 0);
	if (flags != -1)
		fcntl(t->pty, F_SETFL, flags | O_NONBLOCK);

	return t->pid = pid;
}

int vt_pty_get(Vt *t)
{
	return t->pty;
}

ssize_t vt_write(Vt *t, const char *buf, size_t len)
{
	ssize_t ret = len;

	while (len > 0) {
		ssize_t res = write(t->pty, buf, len);
		if (res < 0) {
			if (errno != EAGAIN && errno != EINTR)
				return -1;
			continue;
		}
		buf += res;
		len -= res;
	}

	return ret;
}

static void send_curs(Vt *t)
{
	Buffer *b = t->buffer;
	char keyseq[32];
	/* DSR cursor-position replies are 1-indexed (row 1 = top-left) per
	 * spec, matching the CUP (\e[row;colH) sequence's own indexing --
	 * but curs_row/curs_col are stored 0-indexed internally, so they
	 * need the same +1 conversion CUP's own handler applies when
	 * *receiving* a position, just in the other direction here. */
	snprintf(keyseq, sizeof keyseq, "\e[%d;%dR",
	         (int)(b->curs_row - b->lines) + 1, b->curs_col + 1);
	vt_write(t, keyseq, strlen(keyseq));
}

void vt_keypress(Vt *t, int keycode)
{
	vt_noscroll(t);

	if (keycode >= 0 && keycode <= KEY_MAX && keytable[keycode]) {
		switch (keycode) {
		case KEY_UP:
		case KEY_DOWN:
		case KEY_RIGHT:
		case KEY_LEFT: {
			char keyseq[3] = { '\e', (t->curskeymode ? 'O' : '['), keytable[keycode][0] };
			vt_write(t, keyseq, sizeof keyseq);
			break;
		}
		default:
			vt_write(t, keytable[keycode], strlen(keytable[keycode]));
		}
	} else if (keycode <= UCHAR_MAX) {
		char c = keycode;
		vt_write(t, &c, 1);
	} else {
#ifndef NDEBUG
		fprintf(stderr, "unhandled key %#o\n", keycode);
#endif
	}
}

void vt_mouse(Vt *t, int x, int y, mmask_t mask)
{
#ifdef NCURSES_MOUSE_VERSION
	char seq[6] = { '\e', '[', 'M' }, state = 0, button = 0;

	if (!t->mousetrack)
		return;

	if (mask & (BUTTON1_PRESSED | BUTTON1_CLICKED))
		button = 0;
	else if (mask & (BUTTON2_PRESSED | BUTTON2_CLICKED))
		button = 1;
	else if (mask & (BUTTON3_PRESSED | BUTTON3_CLICKED))
		button = 2;
	else if (mask & (BUTTON1_RELEASED | BUTTON2_RELEASED | BUTTON3_RELEASED))
		button = 3;

	if (mask & BUTTON_SHIFT)
		state |= 4;
	if (mask & BUTTON_ALT)
		state |= 8;
	if (mask & BUTTON_CTRL)
		state |= 16;

	seq[3] = 32 + button + state;
	seq[4] = 32 + x;
	seq[5] = 32 + y;

	vt_write(t, seq, sizeof seq);

	if (mask & (BUTTON1_CLICKED | BUTTON2_CLICKED | BUTTON3_CLICKED)) {
		/* send a button release event */
		button = 3;
		seq[3] = 32 + button + state;
		vt_write(t, seq, sizeof seq);
	}
#endif /* NCURSES_MOUSE_VERSION */
}

/*
 * Dimension used for the palette hash table below. curfg/curbg (see
 * interpret_csi_sgr) only ever hold basic-16 or 256-palette indices --
 * never raw truecolor RGB, which is carried separately in
 * curfg_rgb/curbg_rgb and never touches this table -- so this only
 * needs to cover the 256-color palette range regardless of how large
 * COLORS is. Using raw COLORS here directly would overflow the
 * (COLORS+2)*(COLORS+2) calloc size on 24-bit/truecolor terminals
 * (COLORS=16777216), wrapping to a too-small allocation and corrupting
 * memory the first time a real index near the true COLORS value was
 * hashed.
 */
static int palette_hash_dim(void)
{
	int dim = COLORS;
	if (dim > 256 || dim <= 0)
		dim = 256;
	return dim;
}

static unsigned int color_hash(short fg, short bg)
{
	int dim = palette_hash_dim();
	if (fg == -1 || fg >= dim)
		fg = dim;
	if (bg == -1 || bg >= dim)
		bg = dim;
	return fg * (dim + 2) + bg;
}

short vt_color_get(Vt *t, short fg, short bg)
{
	if (fg >= COLORS)
		fg = (t ? t->deffg : default_fg);
	if (bg >= COLORS)
		bg = (t ? t->defbg : default_bg);

	if (!has_default_colors) {
		if (fg == -1)
			fg = (t && t->deffg != -1 ? t->deffg : default_fg);
		if (bg == -1)
			bg = (t && t->defbg != -1 ? t->defbg : default_bg);
	}

	if (!color2palette || (fg == -1 && bg == -1))
		return 0;
	unsigned int index = color_hash(fg, bg);
	if (color2palette[index] == 0) {
		short oldfg, oldbg;
		for (;;) {
			if (++color_pair_current >= color_pairs_max)
				color_pair_current = color_pairs_reserved + 1;
			pair_content(color_pair_current, &oldfg, &oldbg);
			unsigned int old_index = color_hash(oldfg, oldbg);
			if (color2palette[old_index] >= 0) {
				if (init_pair(color_pair_current, fg, bg) == OK) {
					color2palette[old_index] = 0;
					color2palette[index] = color_pair_current;
				}
				break;
			}
		}
	}

	short color_pair = color2palette[index];
	return color_pair >= 0 ? color_pair : -color_pair;
}

short vt_color_reserve(short fg, short bg)
{
	if (!color2palette || fg >= COLORS || bg >= COLORS)
		return 0;
	if (!has_default_colors && fg == -1)
		fg = default_fg;
	if (!has_default_colors && bg == -1)
		bg = default_bg;
	if (fg == -1 && bg == -1)
		return 0;
	unsigned int index = color_hash(fg, bg);
	if (color2palette[index] >= 0) {
		if (init_pair(color_pairs_reserved + 1, fg, bg) == OK)
			color2palette[index] = -(++color_pairs_reserved);
	}
	short color_pair = color2palette[index];
	return color_pair >= 0 ? color_pair : -color_pair;
}

/* map an 8-bit RGB channel (0-255) onto curses' 0-1000 color scale */
static int rgb_channel_to_curses(int c)
{
	return (c * 1000) / 255;
}

/* nearest xterm 256-color palette index for an RGB triple, used as a
 * fallback on ncurses builds without extended-color/RGB support so
 * truecolor sequences still degrade to a close palette match instead
 * of being dropped. */
static short rgb_to_palette256(int r, int g, int b)
{
	/* 6x6x6 color cube (indices 16-231) */
	int qr = (r * 5) / 255, qg = (g * 5) / 255, qb = (b * 5) / 255;
	int cube_idx = 16 + 36 * qr + 6 * qg + qb;
	static const int levels[6] = { 0, 95, 135, 175, 215, 255 };
	int cr = levels[qr], cg = levels[qg], cb = levels[qb];
	int cube_dist = (r - cr) * (r - cr) + (g - cg) * (g - cg) + (b - cb) * (b - cb);

	/* grayscale ramp (indices 232-255) */
	int gray = (r + g + b) / 3;
	int gray_idx = gray < 8 ? 0 : (gray > 238 ? 23 : (gray - 8) / 10);
	int gv = 8 + gray_idx * 10;
	int gray_dist = (r - gv) * (r - gv) + (g - gv) * (g - gv) + (b - gv) * (b - gv);

	if (gray_dist < cube_dist)
		return 232 + gray_idx;
	return cube_idx;
}

/* approximate RGB for a curses palette index, used when only one side
 * of an (fg, bg) pair has an explicit truecolor override -- lets that
 * side still pair correctly against the other's palette color instead
 * of guessing. -1/unset palette index maps to -1 (caller's default). */
static int32_t palette_to_rgb(short idx)
{
	static const int32_t ansi16[16] = {
		0x000000, 0x800000, 0x008000, 0x808000,
		0x000080, 0x800080, 0x008080, 0xc0c0c0,
		0x808080, 0xff0000, 0x00ff00, 0xffff00,
		0x0000ff, 0xff00ff, 0x00ffff, 0xffffff,
	};
	if (idx < 0)
		return -1;
	if (idx < 16)
		return ansi16[idx];
	if (idx < 232) {
		static const int levels[6] = { 0, 95, 135, 175, 215, 255 };
		int i = idx - 16;
		int r = levels[(i / 36) % 6];
		int g = levels[(i / 6) % 6];
		int b = levels[i % 6];
		return (r << 16) | (g << 8) | b;
	}
	if (idx < 256) {
		int v = 8 + (idx - 232) * 10;
		return (v << 16) | (v << 8) | v;
	}
	return -1;
}

/* allocate/recycle a curses color pair for a 24-bit RGB (fg, bg) combo.
 * Mirrors vt_color_get()'s recycling strategy, but keyed by packed RGB
 * rather than an 8-bit palette index, since RGB space can't be
 * enumerated into MAX_COLOR_PAIRS slots up front.
 *
 * fg_rgb/bg_rgb of -1 means "use the terminal's default color" (e.g. a
 * transparent background under a truecolor foreground) rather than a
 * literal RGB value -- ncurses' use_default_colors()/-1 convention is
 * honored here by passing -1 straight through to init_extended_pair()
 * for that side instead of unpacking it as bytes, which would
 * otherwise read as opaque white (0xffffff). */
static short vt_color_get_truecolor(int32_t fg_rgb, int32_t bg_rgb)
{
	if (!has_truecolor || truecolor_pair_base == 0)
		return 0;
	if (fg_rgb == -1 && bg_rgb == -1)
		return 0; /* nothing truecolor about this cell; let the caller use the palette path */
	if (!has_default_colors && (fg_rgb == -1 || bg_rgb == -1))
		return 0; /* terminal can't do default colors; caller must fall back */

	int hit = truecolor_hash_find(fg_rgb, bg_rgb);
	if (hit >= 0)
		return truecolor_cache[hit].pair;

	int slot;
	if (truecolor_cache_count >= truecolor_cache_size) {
		/* out of reserved pairs/colors: evict the oldest entry rather
		 * than reuse a slot whose *color* definition (not just its
		 * pair) might still be referenced by another live, cached
		 * pair -- each cache slot owns its own dedicated pair+color
		 * indices for its whole lifetime, so eviction here only ever
		 * frees a slot that's no longer pointed at by this cache. */
		slot = truecolor_cache_next;
		truecolor_cache_next = (truecolor_cache_next + 1) % truecolor_cache_size;
		truecolor_hash_remove(slot);
		truecolor_cache[slot].used = false;
	} else {
		/* cache isn't full yet: truecolor_cache_count itself is the
		 * next free slot, since slots are filled in order until the
		 * eviction path above starts recycling. */
		slot = truecolor_cache_count;
	}

	short pair = truecolor_pair_base + slot;

#if defined(NCURSES_EXT_COLORS) && NCURSES_EXT_COLORS
	if (has_direct_color) {
		/*
		 * Direct-color terminfo (det-direct's setaf/setab use the
		 * xterm-direct-style parameterized RGB template, decoding
		 * a packed 0xRRGGBB integer back into three SGR parameters
		 * via terminfo arithmetic) lets init_extended_pair() take
		 * the packed RGB value *directly* as its color argument --
		 * no init_extended_color() registration step, no OSC4. This
		 * is the actual ncurses-documented way to drive a -direct
		 * terminal (see init_extended_pair(3NCURSES)/ncurses FAQ):
		 * "the application should pass actual packed RGB colours
		 * ... such as 0xFF7F00" directly to init_extended_pair().
		 * curses' own buffered output (vidputs, used by doupdate())
		 * resolves the resulting pair through setaf/setab at the
		 * correct point in the output stream, so unlike a raw
		 * putp() write this can't desync relative to character
		 * output during line wraps -- it goes through the exact
		 * same buffering path normal indexed colors already use.
		 * -1 (terminal default) is passed straight through, same
		 * as the indexed path below.
		 */
		if (init_extended_pair(pair, fg_rgb, bg_rgb) != OK)
			return 0;
	} else {
		/* each slot owns two dedicated extended-color indices for
		 * its entire lifetime in the cache, so redefining them here
		 * can never corrupt a different, still-displayed pair the
		 * way sharing a small round-robin color range could. -1 on
		 * either side is passed straight through as the *color
		 * index* (not an RGB triple) so ncurses resolves it as the
		 * terminal default rather than us misreading it as white. */
		int fg_idx = -1, bg_idx = -1;
		if (fg_rgb != -1) {
			fg_idx = truecolor_color_base + slot * 2;
			init_extended_color(fg_idx, rgb_channel_to_curses((fg_rgb >> 16) & 0xff),
			                     rgb_channel_to_curses((fg_rgb >> 8) & 0xff),
			                     rgb_channel_to_curses(fg_rgb & 0xff));
		}
		if (bg_rgb != -1) {
			bg_idx = truecolor_color_base + slot * 2 + 1;
			init_extended_color(bg_idx, rgb_channel_to_curses((bg_rgb >> 16) & 0xff),
			                     rgb_channel_to_curses((bg_rgb >> 8) & 0xff),
			                     rgb_channel_to_curses(bg_rgb & 0xff));
		}
		if (init_extended_pair(pair, fg_idx, bg_idx) != OK)
			return 0;
	}
#else
	/* no ext-colors: degrade to nearest-256-color; ncurses' classic
	 * init_pair() also accepts -1 for either side when
	 * use_default_colors() succeeded, so this passes through the same
	 * way. */
	short fg_idx = (fg_rgb == -1) ? -1
	             : rgb_to_palette256((fg_rgb >> 16) & 0xff, (fg_rgb >> 8) & 0xff, fg_rgb & 0xff);
	short bg_idx = (bg_rgb == -1) ? -1
	             : rgb_to_palette256((bg_rgb >> 16) & 0xff, (bg_rgb >> 8) & 0xff, bg_rgb & 0xff);
	if (init_pair(pair, fg_idx, bg_idx) != OK)
		return 0;
#endif

	truecolor_cache[slot].fg_rgb = fg_rgb;
	truecolor_cache[slot].bg_rgb = bg_rgb;
	truecolor_cache[slot].pair = pair;
	truecolor_cache[slot].used = true;
	truecolor_hash_insert(fg_rgb, bg_rgb, slot);
	if (truecolor_cache_count < truecolor_cache_size)
		truecolor_cache_count++;
	return pair;
}

static void init_colors(void)
{
	pair_content(0, &default_fg, &default_bg);
	if (default_fg == -1)
		default_fg = COLOR_WHITE;
	if (default_bg == -1)
		default_bg = COLOR_BLACK;
	has_default_colors = (use_default_colors() == OK);
	color_pairs_max = MIN(MAX_COLOR_PAIRS, SHRT_MAX);

	/*
	 * Detect whether init_extended_pair()'s packed-RGB convention will
	 * actually work against the *host* terminal's currently-loaded
	 * terminfo (whatever det itself was launched under: foot, st,
	 * xterm, etc, set by initscr() before this runs). This only works
	 * when that terminfo has setaf/setab parameterized RGB templates
	 * AND advertises the RGB boolean -- ncurses validates
	 * init_extended_pair()'s color arguments against COLORS using the
	 * *loaded* terminfo, not any environment-variable heuristic, so
	 * $COLORTERM=truecolor alone is not sufficient here even though
	 * it's a reasonable signal in other contexts (e.g. nvim's own
	 * truecolor auto-detection). Concretely this means det's host
	 * $TERM needs to be a *-direct entry (foot-direct, st-direct,
	 * xterm-direct, det-direct, ...) for this path to engage; plain
	 * foot/st/xterm-256color will correctly fall through to the
	 * indexed extended-color + OSC4 registration path further below,
	 * which is slower but still correct.
	 */
	has_direct_color = (tigetflag("RGB") == 1);

	/*
	 * Reserve a dedicated block of pairs (and, on ext-colors builds, a
	 * matching block of color indices) for truecolor use, carved out of
	 * the same capped pair pool the palette allocator below uses --
	 * COLOR_PAIRS/MAX_COLOR_PAIRS is itself capped at SHRT_MAX (32767
	 * here), so there's no headroom *beyond* color_pairs_max to claim;
	 * the reserve has to come out of it instead.
	 */
	has_truecolor = false;
	truecolor_pair_base = 0;
	truecolor_color_base = 0;
	truecolor_cache_size = 0;
	truecolor_cache_count = 0;
	truecolor_cache_next = 0;

#if defined(NCURSES_EXT_COLORS) && NCURSES_EXT_COLORS
	long color_headroom = (long)COLORS - 16; /* leave the base 16 ANSI slots alone */
	long want = TRUECOLOR_PAIRS_MAX;
	if (color_headroom / 2 < want)
		want = color_headroom / 2;
	if (want > color_pairs_max / 4) /* never take more than a quarter of pair space */
		want = color_pairs_max / 4;
	if (want > 0 && color_pairs_max > want) {
		has_truecolor = true;
		truecolor_pair_base = color_pairs_max - want;
		truecolor_color_base = 16;
		truecolor_cache_size = (int)want;
		color_pairs_max -= want; /* keep the palette allocator out of this range */
	}
#else
	/* no ext-colors: truecolor degrades to nearest-256-color via the
	 * normal init_pair() path, so this just needs spare *pairs*, not
	 * spare *colors*. */
	long pair_reserve = color_pairs_max / 4;
	if (pair_reserve > TRUECOLOR_PAIRS_MAX)
		pair_reserve = TRUECOLOR_PAIRS_MAX;
	if (pair_reserve > 0 && color_pairs_max > pair_reserve) {
		has_truecolor = true;
		truecolor_pair_base = color_pairs_max - pair_reserve;
		truecolor_cache_size = (int)pair_reserve;
		color_pairs_max -= pair_reserve;
	}
#endif

	if (truecolor_cache_size > TRUECOLOR_PAIRS_MAX)
		truecolor_cache_size = TRUECOLOR_PAIRS_MAX;
	if (has_truecolor) {
		memset(truecolor_cache, 0, sizeof truecolor_cache);
		for (int i = 0; i < TRUECOLOR_HASH_SIZE; i++)
			truecolor_hash_index[i] = -1;
	}

	if (COLORS) {
		int dim = palette_hash_dim();
		color2palette = calloc((dim + 2) * (dim + 2), sizeof(short));
	}
	/*
	 * XXX: On undefined color-pairs NetBSD curses pair_content() set fg
	 *      and bg to default colors while ncurses set them respectively to
	 *      0 and 0. Initialize all color-pairs in order to have consistent
	 *      behaviour despite the implementation used.
	 */
	for (short i = 1; i < color_pairs_max; i++)
		init_pair(i, 0, 0);
	vt_color_reserve(COLOR_WHITE, COLOR_BLACK);
}

void vt_init(void)
{
	init_colors();
	is_utf8_locale();
	char *term = getenv("DET_TERM");
	if (!term)
		term = getenv("DVTM_TERM"); /* back-compat with upstream dvtm */
	if (!term)
		term = "det";
	const char *suffix = "";
	if (COLORS >= 256)
		suffix = has_truecolor ? "-direct" : "-256color";
	snprintf(vt_term, sizeof vt_term, "%s%s", term, suffix);
}

void vt_keytable_set(const char * const keytable_overlay[], int count)
{
	for (int k = 0; k < count && k < KEY_MAX; k++) {
		const char *keyseq = keytable_overlay[k];
		if (keyseq)
			keytable[k] = keyseq;
	}
}

void vt_shutdown(void)
{
	free(color2palette);
}

void vt_title_handler_set(Vt *t, vt_title_handler_t handler)
{
	t->title_handler = handler;
}

void vt_urgent_handler_set(Vt *t, vt_urgent_handler_t handler)
{
	t->urgent_handler = handler;
}

void vt_data_set(Vt *t, void *data)
{
	t->data = data;
}

void *vt_data_get(Vt *t)
{
	return t->data;
}

bool vt_cursor_visible(Vt *t)
{
	return t->buffer->scroll_below ? false : !t->curshid;
}

bool vt_pastemode_get(Vt *t)
{
	return t->pastemode;
}

pid_t vt_pid_get(Vt *t)
{
	return t->pid;
}

size_t vt_content_get(Vt *t, char **buf, bool colored)
{
	Buffer *b = t->buffer;
	int lines = b->scroll_above + b->scroll_below + b->rows + 1;
	size_t size = lines * ((b->cols + 1) * ((colored ? 96 : 0) + MB_CUR_MAX));
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));

	if (!(*buf = malloc(size)))
		return 0;

	char *s = *buf;
	Cell *prev_cell = NULL;

	for (Row *row = buffer_row_first(b); row; row = buffer_row_next(b, row)) {
		size_t len = 0;
		char *last_non_space = s;
		for (int col = 0; col < b->cols; col++) {
			Cell *cell = row->cells + col;
			if (colored) {
				int esclen = 0;
				if (!prev_cell || cell->attr != prev_cell->attr) {
					attr_t attr = cell->attr << NCURSES_ATTR_SHIFT;
					esclen = sprintf(s, "\033[0%s%s%s%s%s%sm",
						attr & A_BOLD ? ";1" : "",
						attr & A_DIM ? ";2" : "",
						attr & A_UNDERLINE ? ";4" : "",
						attr & A_BLINK ? ";5" : "",
						attr & A_REVERSE ? ";7" : "",
						attr & A_INVIS ? ";8" : "");
					if (esclen > 0)
						s += esclen;
				}
				if (!prev_cell || cell->fg != prev_cell->fg || cell->fg_rgb != prev_cell->fg_rgb
				    || cell->attr != prev_cell->attr) {
					if (cell->fg_rgb != -1) {
						int r = (cell->fg_rgb >> 16) & 0xff;
						int g = (cell->fg_rgb >> 8) & 0xff;
						int bl = cell->fg_rgb & 0xff;
						esclen = sprintf(s, "\033[38;2;%d;%d;%dm", r, g, bl);
					} else if (cell->fg == -1)
						esclen = sprintf(s, "\033[39m");
					else
						esclen = sprintf(s, "\033[38;5;%dm", cell->fg);
					if (esclen > 0)
						s += esclen;
				}
				if (!prev_cell || cell->bg != prev_cell->bg || cell->bg_rgb != prev_cell->bg_rgb
				    || cell->attr != prev_cell->attr) {
					if (cell->bg_rgb != -1) {
						int r = (cell->bg_rgb >> 16) & 0xff;
						int g = (cell->bg_rgb >> 8) & 0xff;
						int bl = cell->bg_rgb & 0xff;
						esclen = sprintf(s, "\033[48;2;%d;%d;%dm", r, g, bl);
					} else if (cell->bg == -1)
						esclen = sprintf(s, "\033[49m");
					else
						esclen = sprintf(s, "\033[48;5;%dm", cell->bg);
					if (esclen > 0)
						s += esclen;
				}
				prev_cell = cell;
			}
			if (cell->text) {
				len = wcrtomb(s, cell->text, &ps);
				if (len > 0)
					s += len;
				last_non_space = s;
			} else if (len) {
				len = 0;
			} else {
				*s++ = ' ';
			}
		}

		s = last_non_space;
		*s++ = '\n';
	}

	return s - *buf;
}

int vt_content_start(Vt *t)
{
	return t->buffer->scroll_above;
}
