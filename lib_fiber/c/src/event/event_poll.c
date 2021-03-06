#include "stdafx.h"
#include "common.h"

#ifdef HAS_POLL

#include "event.h"
#include "event_poll.h"

typedef int(*poll_fn)(struct pollfd *, nfds_t, int);

static poll_fn __sys_poll = NULL;

static void hook_init(void)
{
#ifdef SYS_UNIX
	static pthread_mutex_t __lock = PTHREAD_MUTEX_INITIALIZER;
	static int __called = 0;

	(void) pthread_mutex_lock(&__lock);

	if (__called) {
		(void) pthread_mutex_unlock(&__lock);
		return;
	}

	__called++;

	__sys_poll = (poll_fn) dlsym(RTLD_NEXT, "poll");
	assert(__sys_poll);

	(void) pthread_mutex_unlock(&__lock);
#endif
}

/****************************************************************************/

typedef struct EVENT_POLL {
	EVENT  event;
	FILE_EVENT **files;
	int    size;
	int    count;
	struct pollfd *pfds;
} EVENT_POLL;

static void poll_free(EVENT *ev)
{
	EVENT_POLL *ep = (EVENT_POLL *) ev;

	free(ep->files);
	free(ep->pfds);
	free(ep);
}

static int poll_add_read(EVENT_POLL *ep, FILE_EVENT *fe)
{
	struct pollfd *pfd;

	if (fe->id == -1) {
		assert(ep->count < ep->size);
		fe->id = ep->count++;
	}

	pfd = &ep->pfds[fe->id];

	if (pfd->events & (POLLIN | POLLOUT)) {
		assert(ep->files[fe->id] == fe);
	} else {
		pfd->events       = 0;
		pfd->fd           = fe->fd;
		pfd->revents      = 0;
		ep->files[fe->id] = fe;
	}

	fe->mask    |= EVENT_READ;
	pfd->events |= POLLIN;
	return 0;
}

static int poll_add_write(EVENT_POLL *ep, FILE_EVENT *fe)
{
	struct pollfd *pfd = (fe->id >= 0 && fe->id < ep->count)
		? &ep->pfds[fe->id] : NULL;

	if (fe->id == -1) {
		assert(ep->count < ep->size);
		fe->id = ep->count++;
	}

	pfd = &ep->pfds[fe->id];

	if (pfd->events & (POLLIN | POLLOUT)) {
		assert(ep->files[fe->id] == fe);
	} else {
		pfd->events       = 0;
		pfd->fd           = fe->fd;
		pfd->revents      = 0;
		ep->files[fe->id] = fe;
	}

	fe->mask    |= EVENT_WRITE;
	pfd->events |= POLLOUT;
	return 0;
}

static int poll_del_read(EVENT_POLL *ep, FILE_EVENT *fe)
{
	struct pollfd *pfd;

	assert(fe->id >= 0 && fe->id < ep->count);
	pfd = &ep->pfds[fe->id];
	assert(pfd);

	if (pfd->events & POLLIN) {
		pfd->events &= ~POLLIN;
	}
	if (!(pfd->events & POLLOUT)) {
		if (fe->id < --ep->count) {
			ep->pfds[fe->id]      = ep->pfds[ep->count];
			ep->files[fe->id]     = ep->files[ep->count];
			ep->files[fe->id]->id = fe->id;
		}
		ep->pfds[ep->count].fd      = -1;
		ep->pfds[ep->count].events  = 0;
		ep->pfds[ep->count].revents = 0;
		fe->id = -1;
	}
	fe->mask &= ~EVENT_READ;
	return 0;
}

static int poll_del_write(EVENT_POLL *ep, FILE_EVENT *fe)
{
	struct pollfd *pfd;

	assert(fe->id >= 0 && fe->id < ep->count);
	pfd = &ep->pfds[fe->id];
	assert(pfd);

	if (pfd->events & POLLOUT) {
		pfd->events &= ~POLLOUT;
	}
	if (!(pfd->events & POLLIN)) {
		if (fe->id < --ep->count) {
			ep->pfds[fe->id]      = ep->pfds[ep->count];
			ep->files[fe->id]     = ep->files[ep->count];
			ep->files[fe->id]->id = fe->id;
		}
		ep->pfds[ep->count].fd      = -1;
		ep->pfds[ep->count].events  = 0;
		ep->pfds[ep->count].revents = 0;
		fe->id = -1;
	}
	fe->mask &= ~EVENT_WRITE;
	return 0;
}

static int poll_wait(EVENT *ev, int timeout)
{
	EVENT_POLL *ep = (EVENT_POLL *) ev;
	int n, i;

	n = __sys_poll(ep->pfds, ep->count, timeout);
	if (n < 0) {
		if (errno == EINTR) {
			return 0;
		}
		msg_fatal("%s: poll error %s", __FUNCTION__, last_serror());
	}

	for (i = 0; i < ep->count; i++) {
		FILE_EVENT *fe     = ep->files[i];
		struct pollfd *pfd = &ep->pfds[fe->id];

		if (pfd->revents & POLLIN && fe->r_proc) {
			fe->r_proc(ev, fe);
		}
		if (pfd->revents & POLLOUT && fe->w_proc) {
			fe->w_proc(ev, fe);
		}
	}

	return n;
}

static int poll_handle(EVENT *ev)
{
	(void) ev;
	return -1;
}

static const char *poll_name(void)
{
	return "poll";
}

EVENT *event_poll_create(int size)
{
	EVENT_POLL *ep = (EVENT_POLL *) malloc(sizeof(EVENT_POLL));

	if (__sys_poll == NULL) {
		hook_init();
	}

	// override size with system open limit setting
	size      = open_limit(0);
	ep->size  = size;
	ep->pfds  = (struct pollfd *) calloc(size, sizeof(struct pollfd));
	ep->files = (FILE_EVENT**) calloc(size, sizeof(FILE_EVENT*));
	ep->count = 0;

	ep->event.name   = poll_name;
	ep->event.handle = poll_handle;
	ep->event.free   = poll_free;

	ep->event.event_wait = poll_wait;
	ep->event.add_read   = (event_oper *) poll_add_read;
	ep->event.add_write  = (event_oper *) poll_add_write;
	ep->event.del_read   = (event_oper *) poll_del_read;
	ep->event.del_write  = (event_oper *) poll_del_write;

	return (EVENT*) ep;
}

#endif
