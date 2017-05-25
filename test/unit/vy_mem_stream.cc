#define MP_LIBRARY
#include <box/vy_mem.h>
#include <small/quota.h>
#include <small/lsregion.h>
#include "unit.h"



void
test1()
{
	header();

	int rc;
	struct quota quota;
	quota_init(&quota, 16 * 1024 * 1024);
	struct slab_arena arena;

	rc = slab_arena_create(&arena, &quota, 0, 1024 * 1024, MAP_PRIVATE);
	ok(rc == 0, "slab_arena_create failed");

	struct lsregion lsreg;
	lsregion_create(&lsreg, &arena);

	struct vy_mem *mem = vy_mem_new(&lsreg, 0, 0, 0, 0, 0, 0);
	(void)mem;


	lsregion_destroy(&lsreg);

	slab_arena_destroy(&arena);

	footer();
}


int
main()
{
	test1();
}