/*
 * Status bar module: git branch + working-tree status, e.g.
 *
 *     master +2 ~1 ?3
 *
 * +N = staged changes, ~N = unstaged (modified) changes, ?N =
 * untracked files. Shows nothing (module hidden, no stray separator)
 * when the current directory isn't inside a git repo.
 *
 * This is also the reference template for writing your own ASYNC
 * module -- one that shells out to an external command without
 * blocking det's main loop while that command runs. `git status` on
 * a large repo can easily take several hundred ms to a few seconds;
 * running it synchronously inside a render() function would freeze
 * every pane's redraw for that whole time, which is exactly the kind
 * of stall this fork has put a lot of effort into eliminating
 * elsewhere -- so modules must not reintroduce it.
 *
 * The async pattern, in three pieces:
 *
 *   1. async_start(self): fork()+exec() (or popen(), but see the note
 *      below on why this uses fork/pipe/exec directly instead) the
 *      command, return the read end of a pipe connected to its
 *      stdout, set O_NONBLOCK on that fd, and stash whatever you need
 *      to track the child (here: just its pid, so we can reap it) in
 *      self->_state.
 *
 *   2. async_poll(self, fd): called by det's main loop once that fd
 *      is actually readable. Do a single non-blocking read, append to
 *      a buffer, and return 0 ("not done, keep polling") until you
 *      see EOF (read() returns 0), at which point parse the
 *      accumulated buffer into self->_cache and return 1.
 *
 *   3. Module struct: set async_start/async_poll instead of render.
 *      refresh_interval_sec still controls how often a NEW git status
 *      is started; while one is in flight the bar keeps showing the
 *      previous result rather than blanking out.
 *
 * Why fork+pipe+exec instead of popen(): popen() is simpler but always
 * blocks (its FILE* doesn't give you a way to set O_NONBLOCK on the
 * underlying fd before the child has started writing, and waitpid
 * timing around pclose() varies by libc) -- doing it by hand gives
 * full control over non-blocking behavior, which is the whole point
 * here.
 */
#include "git.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

#define GIT_MODULE_REFRESH_SEC 2
#define GIT_MODULE_BUF_MAX 8192

typedef struct {
	pid_t pid;
	char buf[GIT_MODULE_BUF_MAX];
	size_t len;
} GitState;

static int
git_async_start(Module *self) {
	int pipefd[2];
	if (pipe(pipefd) != 0)
		return -1;

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}

	if (pid == 0) {
		/* child: stdout -> pipe write end, stderr discarded (a
		 * non-repo directory makes git fail with an error message on
		 * stderr we don't want leaking into anything; the pipe simply
		 * has no useful output in that case, which the parser treats
		 * the same as "not a repo"). */
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull != -1)
			dup2(devnull, STDERR_FILENO);
		close(pipefd[1]);
		execlp("git", "git", "status", "--branch", "--porcelain=v2", (char *)NULL);
		_exit(127); /* execlp failed (git not installed, etc) */
	}

	/* parent */
	close(pipefd[1]);
	int flags = fcntl(pipefd[0], F_GETFL, 0);
	if (flags != -1)
		fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

	GitState *st = calloc(1, sizeof *st);
	if (!st) {
		close(pipefd[0]);
		return -1;
	}
	st->pid = pid;
	self->_state = st;
	return pipefd[0];
}

static void
git_parse(const char *data, size_t len, char *out, size_t outsz) {
	char branch[128] = "";
	int staged = 0, unstaged = 0, untracked = 0;
	bool in_repo = false;

	const char *p = data, *end = data + len;
	while (p < end) {
		const char *nl = memchr(p, '\n', end - p);
		size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);

		if (linelen >= 14 && !strncmp(p, "# branch.head ", 14)) {
			in_repo = true;
			size_t blen = linelen - 14;
			if (blen >= sizeof branch)
				blen = sizeof branch - 1;
			memcpy(branch, p + 14, blen);
			branch[blen] = '\0';
		} else if (linelen >= 4 && (p[0] == '1' || p[0] == '2')) {
			in_repo = true;
			char x = p[2], y = p[3];
			if (x != '.')
				staged++;
			if (y != '.')
				unstaged++;
		} else if (linelen >= 1 && p[0] == 'u') {
			in_repo = true;
			staged++;
			unstaged++;
		} else if (linelen >= 1 && p[0] == '?') {
			in_repo = true;
			untracked++;
		}

		p = nl ? nl + 1 : end;
	}

	if (!in_repo) {
		out[0] = '\0';
		return;
	}
	if (branch[0] == '\0')
		snprintf(branch, sizeof branch, "(detached)");

	snprintf(out, outsz, "%s +%d ~%d ?%d", branch, staged, unstaged, untracked);
}

static int
git_async_poll(Module *self, int fd) {
	GitState *st = self->_state;
	if (!st) /* shouldn't happen, but don't crash if it does */
		return -1;

	ssize_t r;
	for (;;) {
		if (st->len >= sizeof st->buf - 1)
			break; /* buffer full -- stop reading, parse what we have */
		r = read(fd, st->buf + st->len, sizeof st->buf - 1 - st->len);
		if (r > 0) {
			st->len += r;
			continue;
		}
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0; /* nothing more available right now, not done */
		break; /* r == 0 (EOF) or a real read error -- either way done */
	}

	st->buf[st->len] = '\0';
	git_parse(st->buf, st->len, self->_cache, sizeof self->_cache);

	int status;
	waitpid(st->pid, &status, 0); /* reap -- we know it's finished or
	                                * close enough (EOF/error on the
	                                * pipe), a brief blocking wait here
	                                * is for a process that's already
	                                * exited or about to, not a stall */
	free(st);
	self->_state = NULL;

	/* Always "done" at this point (EOF or read error both end the
	 * attempt) -- modules_async_dispatch() itself decides visibility
	 * by checking whether _cache ended up empty, so just report 1
	 * (don't conflate "git ran but found no repo" with "the async
	 * operation itself failed", which would retry needlessly). */
	return 1;
}

Module M_GIT = {
	.name = "M_GIT",
	.async_start = git_async_start,
	.async_poll = git_async_poll,
	.refresh_interval_sec = GIT_MODULE_REFRESH_SEC,
};
