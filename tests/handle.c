/** @file Unit tests for the event loop's handles (src/loop.c), which reach
 *  into its slot table. */
#include <stdint.h>
#include <stdio.h>

#include "loop.c"

static unsigned failures;

#define CHECK(cond) \
	((cond) ? (void)0 \
	        : (void)(++failures, fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
	                                     __FILE__, __LINE__, #cond)))

/* The slot @p h names, whatever its generation. */
static uint32_t
slot (struct handle h)
{
	return (uint32_t)(h.bits >> 32) & (SLOTS_MAX - 1);
}

/* A slot freed and named again: every copy of the old handle names nothing,
 * not even once forgotten again, and the new one names its object. */
static void
test_reuse (struct loop *l)
{
	int a;
	int b;
	struct handle ha;
	struct handle hb;
	CHECK(!loop_handle(l, &a, &ha));
	CHECK(ha.bits && loop_lookup(l, ha) == &a);
	loop_forget(l, ha);
	CHECK(!loop_lookup(l, ha));
	CHECK(!loop_handle(l, &b, &hb));
	CHECK(slot(hb) == slot(ha) && hb.bits != ha.bits);
	CHECK(!loop_lookup(l, ha));
	CHECK(loop_lookup(l, hb) == &b);
	loop_forget(l, ha);
	CHECK(loop_lookup(l, hb) == &b);
	loop_forget(l, hb);
	CHECK(!loop_lookup(l, hb));
}

/* Another loop's handle, a slot never used, and no handle name nothing. */
static void
test_foreign (struct loop *l)
{
	int a;
	struct handle h;
	CHECK(!loop_handle(l, &a, &h));
	CHECK(h.bits >> 56 == l->id);
	CHECK(!loop_lookup(l, (struct handle){ h.bits ^ 1ULL << 56 }));
	CHECK(!loop_lookup(l, (struct handle){ h.bits + (1ULL << 40) }));
	CHECK(!loop_lookup(l, (struct handle){}));
	loop_forget(l, (struct handle){ h.bits ^ 1ULL << 56 });
	CHECK(loop_lookup(l, h) == &a);
	loop_forget(l, h);
}

/* The generation passes UINT32_MAX to 1, never to 0. */
static void
test_wrap (struct loop *l)
{
	int a;
	struct handle h;
	CHECK(!loop_handle(l, &a, &h));
	loop_forget(l, h);
	l->slots[slot(h)].gen = UINT32_MAX;
	CHECK(!loop_handle(l, &a, &h));
	CHECK((uint32_t)h.bits == UINT32_MAX && loop_lookup(l, h) == &a);
	loop_forget(l, h);
	CHECK(!loop_handle(l, &a, &h));
	CHECK((uint32_t)h.bits == 1 && loop_lookup(l, h) == &a);
	loop_forget(l, h);
}

/* The table grows past its first slots as more are named at once, and the
 * slots freed are named again, the last freed first, before it grows on. */
static void
test_grow (struct loop *l)
{
	enum { N = 3 * SLOTS_MIN };
	static int objs[N];
	struct handle h[N];
	for (size_t i = 0; i < N; ++i)
		CHECK(!loop_handle(l, &objs[i], &h[i]));
	CHECK(l->slots_cap >= N);
	size_t cap = l->slots_cap;
	uint32_t used = l->used;
	for (size_t i = 0; i < N; ++i)
		CHECK(loop_lookup(l, h[i]) == &objs[i]);
	loop_forget(l, h[5]);
	loop_forget(l, h[N - 1]);
	struct handle again;
	CHECK(!loop_handle(l, &objs[0], &again));
	CHECK(slot(again) == slot(h[N - 1]));
	CHECK(!loop_handle(l, &objs[1], &again));
	CHECK(slot(again) == slot(h[5]));
	CHECK(l->slots_cap == cap && l->used == used);
	CHECK(!loop_lookup(l, h[5]) && loop_lookup(l, again) == &objs[1]);
}

int
main (void)
{
	struct loop l = loop();
	l.id = 7;
	if (loop_init(&l)) {
		fprintf(stderr, "loop_init failed\n");
		return 1;
	}
	test_reuse(&l);
	test_foreign(&l);
	test_wrap(&l);
	test_grow(&l);
	loop_fini(&l);
	if (failures) {
		fprintf(stderr, "handle: %u checks failed\n", failures);
		return 1;
	}
	printf("handle: all checks passed\n");
	return 0;
}
