/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "cbus.h"

/**
 * Cord interconnect.
 */
struct cbus {
	/** cbus statistics */
	struct rmean *stats;
	/** A mutex to protect bus join. */
	pthread_mutex_t mutex;
	/** Condition for synchronized start of the bus. */
	pthread_cond_t cond;
	/** Connected endpoints */
	struct rlist endpoints;
};

/** A singleton for all cords. */
static struct cbus cbus;

/* A single cbus endpoint - the context of a connected consumer cord. */
struct cbus_endpoint {
	/** Fiber pool associated with the endpoint. */
	struct fiber_pool *pool;
	/**
	 * Endpoint name, used to identify the endpoint when
	 * establishing a route.
	 */
	char name[FIBER_NAME_MAX];
	/** Member of cbus->endpoints */
	struct rlist in_cbus;
};

const char *cbus_stat_strings[CBUS_STAT_LAST] = {
	"EVENTS",
	"LOCKS",
};

static inline void
cmsg_deliver(struct cmsg *msg);

/* {{{ fiber_pool */

enum { FIBER_POOL_SIZE = 4096, FIBER_POOL_IDLE_TIMEOUT = 1 };

/**
 * Main function of the fiber invoked to handle all outstanding
 * tasks in a queue.
 */
static int
fiber_pool_f(va_list ap)
{
	struct fiber_pool *pool = va_arg(ap, struct fiber_pool *);
	struct cord *cord = cord();
	struct fiber *f = fiber();
	struct ev_loop *loop = pool->consumer;
	struct stailq *output = &pool->output;
	struct cmsg *msg;
	ev_tstamp last_active_at = ev_now(loop);
	pool->size++;
restart:
	msg = NULL;
	while (! stailq_empty(output)) {
		 msg = stailq_shift_entry(output, struct cmsg, fifo);

		if (f->caller == &cord->sched && ! stailq_empty(output) &&
		    ! rlist_empty(&pool->idle)) {
			/*
			 * Activate a "backup" fiber for the next
			 * message in the queue.
			 */
			f->caller = rlist_shift_entry(&pool->idle,
						      struct fiber,
						      state);
			assert(f->caller->caller == &cord->sched);
		}
		cmsg_deliver(msg);
	}
	/** Put the current fiber into a fiber cache. */
	if (msg != NULL || ev_now(loop) - last_active_at < pool->idle_timeout) {
		if (msg != NULL)
			last_active_at = ev_now(loop);
		/*
		 * Add the fiber to the front of the list, so that
		 * it is most likely to get scheduled again.
		 */
		rlist_add_entry(&pool->idle, fiber(), state);
		fiber_yield();
		goto restart;
	}
	pool->size--;
	return 0;
}

/** }}} fiber_pool */

/**
 * Find a joined cbus endpoint by name.
 * This is an internal helper method which should be called
 * under cbus::mutex.
 *
 * @return endpoint or NULL if not found
 */
static struct cbus_endpoint *
cbus_find_endpoint(struct cbus *bus, const char *name)
{
	struct cbus_endpoint *endpoint;
	rlist_foreach_entry(endpoint, &bus->endpoints, in_cbus) {
		if (strcmp(endpoint->name, name) == 0)
			return endpoint;
	}
	return NULL;
}

static void
cpipe_flush_cb(ev_loop * /* loop */, struct ev_async *watcher,
	       int /* events */);

void
cpipe_create(struct cpipe *pipe, const char *consumer)
{
	stailq_create(&pipe->input);

	pipe->n_input = 0;
	pipe->max_input = INT_MAX;

	ev_async_init(&pipe->flush_input, cpipe_flush_cb);
	pipe->flush_input.data = pipe;

	tt_pthread_mutex_lock(&cbus.mutex);
	struct cbus_endpoint *endpoint = cbus_find_endpoint(&cbus, consumer);
	while (endpoint == NULL) {
		tt_pthread_cond_wait(&cbus.cond, &cbus.mutex);
		endpoint = cbus_find_endpoint(&cbus, consumer);
	}
	pipe->producer = cord()->loop;
	pipe->pool = endpoint->pool;
	tt_pthread_mutex_unlock(&cbus.mutex);

	/* Set the default max input size. */
	cpipe_set_max_input(pipe, 2 * FIBER_POOL_SIZE);
}

static void
cbus_create(struct cbus *bus)
{
	bus->stats = rmean_new(cbus_stat_strings, CBUS_STAT_LAST);
	if (bus->stats == NULL)
		panic_syserror("cbus_create");


	/* Initialize queue lock mutex. */
	(void) tt_pthread_mutex_init(&bus->mutex, NULL);

	(void) tt_pthread_cond_init(&bus->cond, NULL);

	rlist_create(&bus->endpoints);
}

static void
cbus_destroy(struct cbus *bus)
{
	(void) tt_pthread_mutex_destroy(&bus->mutex);
	(void) tt_pthread_cond_destroy(&bus->cond);
	rmean_delete(bus->stats);
}

/**
 * Join a new endpoint (message consumer) to the bus. The endpoint
 * must have a unique name. The endpoint object is created
 * automatically. Wakes up all producers (@sa cpipe_create()) who
 * are blocked waiting for this endpoint to become available.
 */
void
cbus_join(const char *name)
{
	struct fiber_pool *pool = &cord()->fiber_pool;
	if (pool->max_size == 0) {
		fiber_pool_create(pool, FIBER_POOL_SIZE,
				  FIBER_POOL_IDLE_TIMEOUT, fiber_pool_f);
	}
	tt_pthread_mutex_lock(&cbus.mutex);
	struct cbus_endpoint *endpoint = cbus_find_endpoint(&cbus, name);
	if (endpoint != NULL)
		panic("cbus endpoint %s joined twice", name);

	endpoint = (struct cbus_endpoint *)calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		panic("failed to allocate cbus endpoint %s", name);
	snprintf(endpoint->name, sizeof(endpoint->name), "%s", name);
	endpoint->pool = pool;
	rlist_add_tail(&cbus.endpoints, &endpoint->in_cbus);
	tt_pthread_mutex_unlock(&cbus.mutex);
	/*
	 * Alert all waiting producers.
	 *
	 * POSIX: pthread_cond_broadcast() function shall
	 * have no effect if there are no threads currently
	 * blocked on cond.
	 */
	tt_pthread_cond_broadcast(&cbus.cond);
}

static void
cpipe_flush_cb(ev_loop *loop, struct ev_async *watcher, int events)
{
	(void) loop;
	(void) events;
	struct cpipe *pipe = (struct cpipe *) watcher->data;
	struct fiber_pool *pool = pipe->pool;
	if (pipe->n_input == 0)
		return;

	/* Trigger task processing when the queue becomes non-empty. */
	bool pipe_was_empty;

	tt_pthread_mutex_lock(&pool->mutex);
	pipe_was_empty = stailq_empty(&pool->pipe);
	/** Flush input */
	stailq_concat(&pool->pipe, &pipe->input);
	tt_pthread_mutex_unlock(&pool->mutex);

	pipe->n_input = 0;
	if (pipe_was_empty) {
		/* Count statistics */
		rmean_collect(cbus.stats, CBUS_STAT_EVENTS, 1);

		ev_async_send(pool->consumer, &pool->fetch_output);
	}
}

void
cbus_init()
{
	cbus_create(&cbus);
}

void
cbus_free()
{
	cbus_destroy(&cbus);
}

/* {{{ cmsg */

/**
 * Dispatch the message to the next hop.
 */
static inline void
cmsg_dispatch(struct cpipe *pipe, struct cmsg *msg)
{
	/**
	 * 'pipe' pointer saved in class constructor works as
	 * a guard that the message is alive. If a message route
	 * has the next pipe, then the message mustn't have been
	 * destroyed on this hop. Otherwise msg->hop->pipe could
	 * be already pointing to garbage.
	 */
	if (pipe) {
		/*
		 * Once we pushed the message to the bus,
		 * we relinquished all write access to it,
		 * so we must increase the current hop *before*
		 * push.
		 */
		msg->hop++;
		cpipe_push(pipe, msg);
	}
}

/**
 * Deliver the message and dispatch it to the next hop.
 */
static inline void
cmsg_deliver(struct cmsg *msg)
{
	/*
	 * Save the pointer to the last pipe,
	 * the memory where it is stored may be destroyed
	 * on the last hop.
	 */
	struct cpipe *pipe = msg->hop->pipe;
	msg->hop->f(msg);
	cmsg_dispatch(pipe, msg);
}

static void
cmsg_notify_deliver(struct cmsg *msg)
{
	fiber_wakeup(((struct cmsg_notify *) msg)->fiber);
}

void
cmsg_notify_init(struct cmsg_notify *msg)
{
	static struct cmsg_hop route[] = { { cmsg_notify_deliver, NULL }, };

	cmsg_init(&msg->base, route);
	msg->fiber = fiber();
}

/* }}} cmsg */

/**
 * Call the target function and store the results (diag, rc) in
 * struct cbus_call_msg.
 */
void
cbus_call_perform(struct cmsg *m)
{
	struct cbus_call_msg *msg = (struct cbus_call_msg *)m;
	msg->rc = msg->func(msg);
	if (msg->rc)
		diag_move(&fiber()->diag, &msg->diag);
}

/**
 * Wake up the caller fiber to reap call results.
 * If the fiber is gone, e.g. in case of call timeout
 * or cancellation, invoke free_cb to free message state.
 */
void
cbus_call_done(struct cmsg *m)
{
	struct cbus_call_msg *msg = (struct cbus_call_msg *)m;
	if (msg->caller == NULL) {
		if (msg->free_cb)
			msg->free_cb(msg);
		return;
	}
	msg->complete = true;
	fiber_wakeup(msg->caller);
}

/**
 * Execute a synchronous call over cbus.
 */
int
cbus_call(struct cpipe *callee, struct cpipe *caller, struct cbus_call_msg *msg,
	cbus_call_f func, cbus_call_f free_cb, double timeout)
{
	int rc;

	diag_create(&msg->diag);
	msg->caller = fiber();
	msg->complete = false;
	msg->route[0].f = cbus_call_perform;
	msg->route[0].pipe = caller;
	msg->route[1].f = cbus_call_done;
	msg->route[1].pipe = NULL;
	cmsg_init(cmsg(msg), msg->route);

	msg->func = func;
	msg->free_cb = free_cb;
	msg->rc = 0;

	cpipe_push(callee, cmsg(msg));

	fiber_yield_timeout(timeout);
	if (msg->complete == false) {           /* timed out or cancelled */
		msg->caller = NULL;
		if (fiber_is_cancelled())
			diag_set(FiberIsCancelled);
		else
			diag_set(TimedOut);
		return -1;
	}
	if ((rc = msg->rc))
		diag_move(&msg->diag, &fiber()->diag);
	return rc;
}

