/*
 * Status bar module system.
 *
 * The status bar is a horizontal sequence of "modules" you declare in
 * config.h as a plain array:
 *
 *     static Module *statusline[] = {
 *         &M_PROCESS, &SPACER, &TAGS, &SPACER, &LAYOUT, &M_GIT,
 *     };
 *
 * Each module is a `Module` -- a name, a render function that fills a
 * small text buffer, an optional refresh interval, and curses
 * attributes to draw it with. SPACER and SEPARATOR are built in.
 * TAGS and LAYOUT wrap det's existing tag-list/layout-symbol
 * rendering so they sit in the array like any other module instead of
 * being hardcoded into drawbar() the way they used to be.
 *
 * To write your own module: copy modules/git.h as a template, drop
 * your new file in modules/, #include it from config.h, and
 * reference its Module constant in the statusline array. A module
 * file needs nothing but a render function and one Module struct
 * literal -- see modules/git.h for a complete, real example.
 */
#ifndef DET_MODULES_H
#define DET_MODULES_H

#include <curses.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <stdbool.h>

/* Maximum rendered width (in bytes, not display columns) of a single
 * module's text. Plenty for anything reasonable -- a module that
 * needs more is almost certainly rendering something that doesn't
 * belong in a one-line status bar. */
#define MODULE_TEXT_MAX 256

typedef struct Module Module;

/*
 * A module's render function fills `out` (a buffer of at least
 * MODULE_TEXT_MAX bytes) with the text to display, NUL-terminated.
 * Returning fewer than MODULE_TEXT_MAX bytes written is fine and
 * expected -- most modules render a handful of characters.
 *
 * The function receives the Module itself (so one render function
 * could theoretically back several differently-configured Module
 * instances, though most modules won't need this) and may return
 * false to mean "nothing to show right now" (e.g. git module outside
 * a repo) -- the engine then skips the module and any SEPARATOR
 * immediately adjacent to it on the side that would otherwise double
 * up, so you don't end up with stray "| |" when a module has nothing
 * to say.
 */
typedef bool (*ModuleRenderFn)(const Module *self, char *out, size_t outsz);

/*
 * For modules that need more than "one block of text in one color"
 * -- TAGS is the motivating case, since each tag label gets its own
 * color (selected/urgent/occupied/normal) -- a module may instead (or
 * in addition) provide a `draw` function that's called directly
 * during rendering, with the curses cursor already positioned and at
 * most `maxwidth` columns of room left on the line. It should draw
 * with addch/addstr/attrset etc itself. Used instead of render+_cache
 * when present (render is ignored for a module that sets draw).
 *
 * A draw-based module must also provide `measure`, returning the
 * display-column width it WOULD draw at, without actually drawing
 * anything -- the engine needs every module's width up front (in a
 * first pass) to correctly divide remaining space among SPACERs
 * before the second, actual-drawing pass runs. Keep measure and draw
 * consistent with each other (same width) or spacer math will be off
 * by whatever they disagree by.
 */
typedef int (*ModuleDrawFn)(const Module *self, int maxwidth);
typedef int (*ModuleMeasureFn)(const Module *self);

/*
 * Async modules: for anything that needs to shell out to an external
 * command (git status, a network check, etc) -- running that
 * synchronously inside render() would block det's entire main loop,
 * freezing every pane's redraw for however long the command takes.
 * git status alone is commonly 100-300ms+ depending on repo size,
 * which is exactly the kind of stall this fork has spent a lot of
 * effort eliminating elsewhere, so modules must not reintroduce it.
 *
 * Implement async_start + async_poll instead of render (both, not
 * just one -- a module is either fully synchronous via render(), or
 * fully async via these two; mixing isn't supported):
 *
 *   async_start(self) is called once when the module's refresh
 *   interval is due. Fork/launch whatever you need (fork()+exec(),
 *   popen(), etc) and return a *non-blocking* fd to poll for output,
 *   stashing whatever state you need (pid, buffers) in self->_async.
 *   Return -1 on a launch failure -- the module is treated as
 *   "nothing to show" for this cycle and will retry next time it's
 *   due.
 *
 *   async_poll(self, fd) is called from the main loop's normal
 *   select() handling once that fd is readable (det.c wires this up
 *   automatically for any module with async_start set -- you don't
 *   need to touch det.c's main loop yourself). Read whatever's
 *   available (non-blocking, may be a partial read), and return:
 *     1  = done; you've parsed and written self->_cache yourself
 *          (NUL-terminated, same as render() would), and det should
 *          stop polling this fd and close it.
 *     0  = not done yet, keep polling.
 *    -1  = failed/EOF with nothing useful; det closes the fd, module
 *          shows as "nothing to show" until next refresh.
 */
typedef int (*ModuleAsyncStartFn)(Module *self);
typedef int (*ModuleAsyncPollFn)(Module *self, int fd);

struct Module {
	const char *name;          /* for your own debugging/reference only */
	ModuleRenderFn render;
	ModuleDrawFn draw;          /* see ModuleDrawFn above; NULL for
	                             * ordinary text modules */
	ModuleMeasureFn measure;    /* required if draw is set; see above */
	ModuleAsyncStartFn async_start; /* see ModuleAsyncFn docs above;
	                                  * mutually exclusive with render */
	ModuleAsyncPollFn async_poll;
	int refresh_interval_sec;  /* 0 = re-render on every drawbar() call
	                             * (redraw-driven only); >0 = also force
	                             * a re-render at most this often even
	                             * with no redraw trigger, so e.g. a
	                             * clock or git-status module updates
	                             * on its own between redraws. Ignored
	                             * for draw-based modules, which always
	                             * draw fresh every call since they're
	                             * cheap, redraw-driven things like
	                             * TAGS/LAYOUT in practice. */
	attr_t attrs;              /* 0 = inherit whatever attrset() was
	                             * last set by the bar; non-zero
	                             * overrides it for this module's text.
	                             * Ignored for draw-based modules, which
	                             * set their own attrs internally. */

	/* internal cache -- do not set these in your own Module literals */
	char _cache[MODULE_TEXT_MAX];
	time_t _last_render;
	bool _last_visible;
	void *_state;     /* free for an async module's own use (pid, a
	                    * partial-read buffer, whatever) -- the engine
	                    * never touches this itself beyond passing it
	                    * through; allocate/free it yourself */
	int _async_fd;    /* -1 = no async operation currently in flight.
	                    * See _has_started below for why a fresh
	                    * Module literal's zero-initialized 0 here
	                    * doesn't get misread as "fd 0 in flight". */
	bool _has_started; /* false until async_start() has actually run
	                     * at least once -- distinguishes "never
	                     * started yet" from "_async_fd happens to be
	                     * 0/stdin", since a plain Module literal
	                     * zero-initializes _async_fd to 0, not -1. */
};

/*
 * Built-in pseudo-modules. SPACER divides remaining horizontal space
 * evenly among all SPACERs in the line (the same way a flexbox
 * `flex: 1` element would) -- with zero SPACERs the line is simply
 * left-aligned with empty space on the right, matching the old
 * drawbar() behavior. SEPARATOR draws a single fixed " | " between
 * modules and is automatically skipped if the module on either side
 * rendered nothing this frame.
 */
extern Module SPACER;
extern Module SEPARATOR;

/*
 * TAGS and LAYOUT wrap det's existing tag-list and layout-symbol
 * rendering (occupied/urgent/selected tag coloring, the layout
 * symbol like " T "/" G ") so they can sit in the statusline array
 * like any other module. M_KEYS echoes the in-progress key-chord
 * buffer (e.g. while typing a multi-key binding). M_FIFO wraps the
 * original FIFO-driven external status text (`det -s /path/to/fifo`)
 * as an ordinary module. All four are defined in det.c, where the
 * data they need (tags[], tagset, layout, client list, bar.text)
 * already lives.
 */
extern Module TAGS;
extern Module LAYOUT;
extern Module M_KEYS;
extern Module M_FIFO;

/*
 * Renders the full statusline array starting at the current curses
 * position, using at most `maxwidth` display columns. Call this from
 * drawbar() in place of the old hardcoded sequence. attrset() should
 * already be set to a sane default (e.g. BAR_ATTR) before calling --
 * each module restores that default attribute after drawing unless
 * it has its own `attrs` set.
 */
void statusline_render(Module *modules[], int nmodules, int maxwidth, attr_t default_attrs);

/*
 * Async module integration with det's own main loop. Call once per
 * main-loop iteration:
 *
 *   modules_async_fdset(modules, n, &fd_set, &nfds) -- adds every
 *   currently in-flight async module's fd to the given fd_set (and
 *   bumps *nfds if needed), the same way det.c already does for pty
 *   fds, so pselect() wakes up when an async module's data arrives.
 *
 *   modules_async_dispatch(modules, n, &fd_set) -- after pselect()
 *   returns, call this to poll and consume any async module fd that
 *   FD_ISSET'd ready. Internally calls each due module's async_poll()
 *   and, if it didn't already, async_start() for a module whose
 *   refresh interval came due with nothing in flight yet. Returns
 *   true if any module actually completed this call (i.e. its
 *   displayed text may have changed) -- callers should trigger a
 *   redraw of the bar when this returns true, since finishing an
 *   async fetch doesn't otherwise correspond to any of det's normal
 *   redraw triggers (focus change, resize, etc) and the bar would
 *   otherwise show stale content until something else happened to
 *   redraw it.
 */
void modules_async_fdset(Module *modules[], int nmodules, fd_set *fds, int *nfds);
bool modules_async_dispatch(Module *modules[], int nmodules, fd_set *fds);

#endif
