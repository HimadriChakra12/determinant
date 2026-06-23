/*
 * Implementation of the status module rendering engine. #include this
 * from det.c (after Client/Screen/etc are defined, since TAGS/LAYOUT
 * need them) -- see modules.h for the public API and design notes.
 */
#include "modules.h"
#include <wchar.h>
#include <stdlib.h>
#include <unistd.h>

/* -------------------------------------------------------------------
 * SPACER / SEPARATOR
 * ---------------------------------------------------------------- */

static bool
spacer_render(const Module *self, char *out, size_t outsz) {
	(void)self;
	out[0] = '\0';
	return true; /* width is computed specially in statusline_render(),
	              * not from this text -- see the two-pass layout below. */
}

static bool
separator_render(const Module *self, char *out, size_t outsz) {
	(void)self;
	snprintf(out, outsz, " | ");
	return true;
}

Module SPACER     = { .name = "SPACER",    .render = spacer_render };
Module SEPARATOR  = { .name = "SEPARATOR", .render = separator_render };

/* -------------------------------------------------------------------
 * Rendering engine
 * ---------------------------------------------------------------- */

/* display width of a UTF-8 string, in columns (not bytes) -- modules
 * may emit multi-byte UTF-8 (icons, etc), so byte length is the wrong
 * measure for layout. */
static int
module_display_width(const char *s) {
	wchar_t wbuf[MODULE_TEXT_MAX];
	size_t n = mbstowcs(wbuf, s, MODULE_TEXT_MAX);
	if (n == (size_t)-1)
		return (int)strlen(s); /* fallback: treat as already single-byte */
	int w = wcswidth(wbuf, n);
	return w < 0 ? (int)n : w;
}

/* Re-renders a module's cached text if due (its own timer fired, or
 * it has never been rendered yet), and reports whether it's currently
 * visible (its render function returned true). A module that's not
 * due for a timed refresh keeps showing its last cached text -- this
 * means a 0-interval module always re-renders (cheap, redraw-driven
 * modules like TAGS/LAYOUT/a static label), while a module with
 * refresh_interval_sec > 0 only pays its own render cost that often,
 * even if drawbar() itself is called far more frequently (e.g. once
 * per keystroke). */
static bool
module_refresh(Module *m) {
	if (m->async_start) {
		/* Async module: this function never blocks. If one is
		 * already in flight (_async_fd != -1), leave it alone --
		 * modules_async_dispatch() (called from det.c's main loop
		 * once the fd is actually readable) is what advances it.
		 * Just report whatever the last completed result was, so the
		 * bar keeps showing stale-but-real data while a fresh fetch
		 * is still running, rather than blanking out for however
		 * long that takes. */
		time_t now = time(NULL);
		bool due = (m->_last_render == 0)
		        || (now - m->_last_render >= m->refresh_interval_sec)
		        || (m->should_refresh_now && m->should_refresh_now(m));
		/* "no operation in flight" means either we've never started
		 * one yet (_has_started false -- covers the fresh-module
		 * case where _async_fd is zero-initialized to 0, not -1) or
		 * a previous one completed and reset _async_fd to -1. */
		bool nothing_in_flight = !m->_has_started || m->_async_fd == -1;
		if (due && nothing_in_flight) {
			int fd = m->async_start(m);
			m->_async_fd = fd; /* -1 on launch failure is fine -- next
			                     * due check will just try again */
			m->_has_started = true;
			if (fd == -1)
				m->_last_render = now; /* don't retry every single
				                         * drawbar() call on failure;
				                         * wait out the interval */
		}
		return m->_last_visible;
	}

	time_t now = time(NULL);
	bool due = (m->refresh_interval_sec == 0)
	        || (m->_last_render == 0)
	        || (now - m->_last_render >= m->refresh_interval_sec);
	if (due) {
		m->_last_visible = m->render(m, m->_cache, sizeof m->_cache);
		m->_last_render = now;
		if (!m->_last_visible)
			m->_cache[0] = '\0';
	}
	return m->_last_visible;
}

void
modules_async_fdset(Module *modules[], int nmodules, fd_set *fds, int *nfds) {
	for (int i = 0; i < nmodules; i++) {
		Module *m = modules[i];
		if (!m->async_start)
			continue;
		/* A Module literal with async_start set but no explicit
		 * .async_fd_initialized marker would otherwise have
		 * _async_fd zero-initialized (i.e. looking like "fd 0, stdin,
		 * is in flight") until its first module_refresh() call sets
		 * a real value -- which may not have happened yet on an
		 * early main-loop iteration, before drawbar() has run even
		 * once. _has_started distinguishes "never started" from
		 * "started and currently has fd 0" (vanishingly unlikely in
		 * practice, since fd 0 is stdin and pipe() won't hand it out
		 * while stdin is open, but correctness shouldn't depend on
		 * that). */
		if (!m->_has_started)
			continue;
		if (m->_async_fd != -1) {
			FD_SET(m->_async_fd, fds);
			if (m->_async_fd > *nfds)
				*nfds = m->_async_fd;
		}
	}
}

bool
modules_async_dispatch(Module *modules[], int nmodules, fd_set *fds) {
	bool any_completed = false;
	for (int i = 0; i < nmodules; i++) {
		Module *m = modules[i];
		if (!m->async_start || !m->_has_started || m->_async_fd == -1)
			continue;
		if (!FD_ISSET(m->_async_fd, fds))
			continue;
		int r = m->async_poll(m, m->_async_fd);
		if (r == 0)
			continue; /* not done yet, keep polling next time */
		/* done (success or failure) -- stop polling this fd */
		close(m->_async_fd);
		m->_async_fd = -1;
		m->_last_render = time(NULL);
		m->_last_visible = (r == 1) && m->_cache[0] != '\0';
		if (!m->_last_visible)
			m->_cache[0] = '\0';
		any_completed = true;
	}
	return any_completed;
}

void
statusline_render(Module *modules[], int nmodules, int maxwidth, attr_t default_attrs) {
	if (maxwidth <= 0)
		return;

	/* Pass 1: refresh every module's cache and compute the total fixed
	 * (non-spacer) width, the count of visible spacers, and which
	 * separators should be suppressed (a separator immediately next
	 * to an invisible module, or at either end of the line, draws
	 * nothing -- otherwise removing a module that has "nothing to
	 * show right now" would leave a stray "| |" or a leading/trailing
	 * "|" behind). */
	bool visible[64];
	int n = nmodules < 64 ? nmodules : 64;
	int fixed_width = 0;
	int spacer_count = 0;

	for (int i = 0; i < n; i++) {
		Module *m = modules[i];
		if (m == &SPACER) {
			visible[i] = true; /* resolved in pass 2 once we know the
			                     * per-spacer width; always "visible"
			                     * for adjacency purposes below. */
			spacer_count++;
			continue;
		}
		if (m->draw) {
			/* draw-based modules are always considered visible --
			 * TAGS/LAYOUT always have something to show in practice,
			 * and "nothing to show" for a draw-based module would
			 * need its own signal beyond a bool return, which no
			 * current built-in needs. */
			visible[i] = true;
			continue;
		}
		visible[i] = module_refresh(m);
	}

	/* Separators are suppressed based on their neighbors' visibility,
	 * resolved after the loop above so every module's visibility is
	 * already known regardless of array order. */
	for (int i = 0; i < n; i++) {
		Module *m = modules[i];
		if (m != &SEPARATOR)
			continue;
		bool left_ok = false, right_ok = false;
		for (int j = i - 1; j >= 0; j--) {
			if (modules[j] == &SEPARATOR)
				continue;
			left_ok = visible[j];
			break;
		}
		for (int j = i + 1; j < n; j++) {
			if (modules[j] == &SEPARATOR)
				continue;
			right_ok = visible[j];
			break;
		}
		visible[i] = left_ok && right_ok;
	}

	for (int i = 0; i < n; i++) {
		Module *m = modules[i];
		if (m == &SPACER || !visible[i])
			continue;
		if (m->draw)
			fixed_width += m->measure(m);
		else
			fixed_width += module_display_width(m->_cache);
	}

	/* Pass 2: divide remaining width evenly among visible spacers (any
	 * leftover column from integer division goes to the last spacer,
	 * so the line fills exactly to maxwidth rather than falling a
	 * column or two short). With zero spacers this naturally behaves
	 * like the old left-aligned-with-empty-tail layout, since nothing
	 * claims the remaining space. */
	int remaining = maxwidth - fixed_width;
	int spacer_width = (spacer_count > 0 && remaining > 0) ? remaining / spacer_count : 0;
	int spacer_extra = (spacer_count > 0 && remaining > 0) ? remaining % spacer_count : 0;
	int spacer_index = 0;

	int used = 0;
	for (int i = 0; i < n; i++) {
		Module *m = modules[i];

		if (m->attrs)
			attrset(m->attrs);
		else
			attrset(default_attrs);

		if (m == &SPACER) {
			int w = spacer_width;
			spacer_index++;
			if (spacer_index == spacer_count)
				w += spacer_extra;
			if (w > 0 && used + w <= maxwidth) {
				for (int k = 0; k < w; k++)
					addch(' ');
				used += w;
			}
			continue;
		}

		if (!visible[i])
			continue;

		if (m->draw) {
			/* draw-based modules set their own attrs internally
			 * (that's the whole reason they exist -- TAGS needs a
			 * different color per tag), so the attrset() above is
			 * just a sane starting point for them, not the final
			 * word. */
			int budget = maxwidth - used;
			if (budget <= 0)
				break;
			int w = m->draw(m, budget);
			used += w;
			continue;
		}

		int w = module_display_width(m->_cache);
		if (used + w > maxwidth) {
			/* doesn't fit -- truncate to whatever's left rather than
			 * overflow onto the next line/wrap, mirroring how the old
			 * bar.text rendering clipped to maxwidth. */
			int budget = maxwidth - used;
			if (budget <= 0)
				break;
			wchar_t wbuf[MODULE_TEXT_MAX];
			size_t wn = mbstowcs(wbuf, m->_cache, MODULE_TEXT_MAX);
			if (wn == (size_t)-1)
				break;
			int col = 0, take = 0;
			for (size_t k = 0; k < wn; k++) {
				int cw = wcwidth(wbuf[k]);
				if (cw < 0)
					cw = 0;
				if (col + cw > budget)
					break;
				col += cw;
				take++;
			}
			addnwstr(wbuf, take);
			used += col;
			break;
		}

		addstr(m->_cache);
		used += w;
	}

	attrset(default_attrs);
	clrtoeol();
}
