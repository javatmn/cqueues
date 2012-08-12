/* ==========================================================================
 * signal.c - Lua Continuation Queues
 * --------------------------------------------------------------------------
 * Copyright (c) 2012  William Ahern
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ==========================================================================
 */
#include <string.h>

#include <signal.h>

#include <errno.h>

#include <unistd.h>

#if defined _REENTRANT || defined _THREAD_SAFE
#include <pthread.h>
#endif

#include <lua.h>
#include <lauxlib.h>


/*
 * S I G N A L  L I S T E N E R  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define HAVE_EPOLL (defined __linux)
#define HAVE_PORTS (defined __sun)
#define HAVE_KQUEUE (!HAVE_EPOLL && !HAVE_PORTS)

#if HAVE_EPOLL
#include <sys/epoll.h>
#elif HAVE_PORTS
#include <port.h>
#else
#include <sys/time.h>
#include <sys/event.h>
#endif


#define LSL_CLASS "Signal Listener"

struct signalfd {
	int fd;
	sigset_t desired;
	sigset_t polling;
	sigset_t pending;
}; /* struct signalfd */


static void sfd_preinit(struct signalfd *S) {
	S->fd = -1;

	sigemptyset(&S->desired);
	sigemptyset(&S->polling);
	sigemptyset(&S->pending);
} /* sfd_preinit() */


static int sfd_init(struct signalfd *S) {
	if (-1 == (S->fd = kqueue()))
		return errno;

	return 0;
} /* sfd_init() */


static void sfd_destroy(struct signalfd *S) {
	close(S->fd);

	sfd_preinit(S);
} /* sfd_destroy() */


static int sfd_diff(const sigset_t *a, const sigset_t *b) {
	for (int signo = 1; signo < 32; signo++) {
		if (!!sigismember(a, signo) ^ !!sigismember(b, signo))
			return signo;
	}

	return 0;
} /* sfd_diff() */


static int sfd_update(struct signalfd *S) {
	int signo;

#if HAVE_KQUEUE
	while ((signo = sfd_diff(&S->desired, &S->polling))) {
		struct kevent event;

		if (sigismember(&S->desired, signo)) {
			EV_SET(&event, signo, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);

			if (0 != kevent(S->fd, &event, 1, 0, 0, 0))
				return errno;

			sigaddset(&S->polling, signo);
		} else {
			EV_SET(&event, signo, EVFILT_SIGNAL, EV_DELETE, 0, 0, 0);

			if (0 != kevent(S->fd, &event, 1, 0, 0, 0))
				return errno;

			sigdelset(&S->polling, signo);
		}
	}

	return 0;
#else
#error no kqueue
#endif
} /* sfd_update() */


static int sfd_query(struct signalfd *S) {
#if HAVE_KQUEUE
	struct kevent event;
	int n;

retry:
	if (1 == (n = kevent(S->fd, 0, 0, &event, 1, &(struct timespec){ 0, 0 }))) {
		if (event.filter == EVFILT_SIGNAL) {
			sigaddset(&S->pending, event.ident);
			sigdelset(&S->polling, event.ident);
		}
	} else if (n == -1) {
		if (errno == EINTR)
			goto retry;

		return errno;
	}

	return sfd_update(S);
#else
#error no kqueue
#endif
} /* sfd_query() */


static int lsl_listen(lua_State *L) {
	struct signalfd *S;
	int index, error;

	S = lua_newuserdata(L, sizeof *S);

	sfd_preinit(S);

	for (index = 1; index <= lua_gettop(L) - 1; index++)
		sigaddset(&S->desired, luaL_checkint(L, index));

	luaL_getmetatable(L, LSL_CLASS);
	lua_setmetatable(L, -2);

	if ((error = sfd_init(S)) || (error = sfd_update(S)))
		return luaL_error(L, "signal.listen: %s", strerror(error));

	return 1;
} /* lsl_listen() */


static int lsl__gc(lua_State *L) {
	struct signalfd *S = luaL_checkudata(L, 1, LSL_CLASS);

	sfd_destroy(S);

	return 0;
} /* lsl__gc() */


static int lsl_wait(lua_State *L) {
	struct signalfd *S = luaL_checkudata(L, 1, LSL_CLASS);
	sigset_t none;
	int error, signo;

	if ((error = sfd_query(S)))
		return luaL_error(L, "signal:get: %s", strerror(error));

	sigemptyset(&none);

	if ((signo = sfd_diff(&S->pending, &none))) {
		lua_pushinteger(L, signo);
		sigdelset(&S->pending, signo);

		return 1;
	}

	return 0;
} /* lsl_wait() */


static int lsl_pollfd(lua_State *L) {
	struct signalfd *S = luaL_checkudata(L, 1, LSL_CLASS);

	lua_pushinteger(L, S->fd);

	return 1;
} /* lsl_pollfd() */


static int lsl_events(lua_State *L) {
	struct signalfd *S = luaL_checkudata(L, 1, LSL_CLASS);

	lua_pushliteral(L, "r");

	return 1;
} /* lsl_events() */


static int lsl_timeout(lua_State *L) {
	struct signalfd *S = luaL_checkudata(L, 1, LSL_CLASS);
	sigset_t none;

	sigemptyset(&none);

	if (sfd_diff(&S->pending, &none)) {
		lua_pushnumber(L, 0.0);
	} else {
		lua_pushnil(L);
	}

	return 1;
} /* lsl_timeout() */


static int lsl_interpose(lua_State *L) {
	luaL_getmetatable(L, LSL_CLASS);
	lua_getfield(L, -1, "__index");
	
	lua_pushvalue(L, -4); /* push method name */
	lua_gettable(L, -2);  /* push old method */
			
	lua_pushvalue(L, -5); /* push method name */
	lua_pushvalue(L, -5); /* push new method */
	lua_settable(L, -4);  /* replace old method */

	return 1; /* return old method */
} /* lsl_interpose() */


static const luaL_Reg lsl_methods[] = {
	{ "wait",    &lsl_wait },
	{ "pollfd",  &lsl_pollfd },
	{ "events",  &lsl_events },
	{ "timeout", &lsl_timeout },
	{ NULL,      NULL },
}; /* lsl_methods[] */


static const luaL_Reg lsl_metatable[] = {
	{ "__gc", &lsl__gc },
	{ NULL,   NULL },
}; /* lsl_metatable[] */


/*
 * S I G N A L  D I S P O S I T I O N  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static int xsigmask(int how, const sigset_t *set, sigset_t *oset) {
#if defined _REENTRANT || defined _THREAD_SAFE
	return pthread_sigmask(how, set, oset);
#else
	return (0 == sigprocmask(how, set, oset))? 0 : errno;
#endif
} /* xsigmask() */


static int ls_ignore(lua_State *L) {
	struct sigaction sa;
	int index;
	
	for (index = 1; index <= lua_gettop(L); index++) {
		sa.sa_handler = SIG_IGN;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;

		if (0 != sigaction(luaL_checkint(L, index), &sa, 0))
			return luaL_error(L, "signal.ignore: %s", strerror(errno));
	}

	return 0;
} /* ls_ignore() */


static int ls_default(lua_State *L) {
	struct sigaction sa;
	int index;
	
	for (index = 1; index <= lua_gettop(L); index++) {
		sa.sa_handler = SIG_DFL;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;

		if (0 != sigaction(luaL_checkint(L, index), &sa, 0))
			return luaL_error(L, "signal.default: %s", strerror(errno));
	}

	return 0;
} /* ls_default() */


static int ls_block(lua_State *L) {
	sigset_t set;
	int index, error;

	sigemptyset(&set);

	for (index = 1; index <= lua_gettop(L); index++) {
		sigaddset(&set, luaL_checkint(L, index));
	}

	if ((error = xsigmask(SIG_BLOCK, &set, 0)))
		return luaL_error(L, "signal.block: %s", strerror(error));

	return 0;
} /* ls_block() */


static int ls_unblock(lua_State *L) {
	sigset_t set;
	int index, error;

	sigemptyset(&set);

	for (index = 1; index <= lua_gettop(L); index++) {
		sigaddset(&set, luaL_checkint(L, index));
	}

	if ((error = xsigmask(SIG_UNBLOCK, &set, 0)))
		return luaL_error(L, "signal.unblock: %s", strerror(error));

	return 0;
} /* ls_unblock() */


static int ls_raise(lua_State *L) {
	int index;

	for (index = 1; index <= lua_gettop(L); index++) {
		raise(luaL_checkint(L, index));
	}

	return 0;
} /* ls_raise() */


static int ls_strsignal(lua_State *L) {
	lua_pushstring(L, strsignal(luaL_checkint(L, 1)));

	return 1;
} /* ls_strsignal() */


static const luaL_Reg ls_globals[] = {
	{ "listen",    &lsl_listen },
	{ "interpose", &lsl_interpose },
	{ "ignore",    &ls_ignore },
	{ "default",   &ls_default },
	{ "block",     &ls_block },
	{ "unblock",   &ls_unblock },
	{ "raise",     &ls_raise },
	{ "strsignal", &ls_strsignal },
	{ NULL, NULL }
};


int luaopen__cqueues_signal(lua_State *L) {
	static const struct {
		const char *name;
		int value;
	} siglist[] = {
		{ "SIGALRM", SIGALRM },
		{ "SIGCHLD", SIGCHLD },
		{ "SIGHUP",  SIGHUP  },
		{ "SIGINT",  SIGINT  },
		{ "SIGPIPE", SIGPIPE },
		{ "SIGQUIT", SIGQUIT },
		{ "SIGTERM", SIGTERM },
	};
	unsigned i;

	if (luaL_newmetatable(L, LSL_CLASS)) {
		luaL_setfuncs(L, lsl_metatable, 0);

		luaL_newlib(L, lsl_methods);
		lua_setfield(L, -2, "__index");
	}

	luaL_newlib(L, ls_globals);

	for (i = 0; i < sizeof siglist / sizeof *siglist; i++) {
		lua_pushstring(L, siglist[i].name);
		lua_pushinteger(L, siglist[i].value);
		lua_settable(L, -3);

		lua_pushinteger(L, siglist[i].value);
		lua_pushstring(L, siglist[i].name);
		lua_settable(L, -3);
	}

	return 1;
} /* luaopen__cqueues_signal() */

