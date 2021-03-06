/*
 * Copyright 2015-2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * obj_recovery.c -- unit test for pool recovery
 */
#include "unittest.h"
#if defined(USE_VG_PMEMCHECK) || defined(USE_VALGRIND)
#include <valgrind/pmemcheck.h>
#define VALGRIND_PMEMCHECK_END_TX VALGRIND_PMC_END_TX
#else
#define VALGRIND_PMEMCHECK_END_TX
#endif

POBJ_LAYOUT_BEGIN(recovery);
POBJ_LAYOUT_ROOT(recovery, struct root);
POBJ_LAYOUT_TOID(recovery, struct foo);
POBJ_LAYOUT_END(recovery);

struct foo {
	int bar;
};

struct root {
	PMEMmutex lock;
	TOID(struct foo) foo;
};

#define BAR_VALUE 5

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_recovery");

	/* root doesn't count */
	UT_COMPILE_ERROR_ON(POBJ_LAYOUT_TYPES_NUM(recovery) != 1);

	if (argc != 5)
		UT_FATAL("usage: %s [file] [lock: y/n] "
			"[cmd: c/o] [type: n/f/s]",
			argv[0]);

	const char *path = argv[1];

	PMEMobjpool *pop = NULL;
	int exists = argv[3][0] == 'o';
	enum { TEST_NEW, TEST_FREE, TEST_SET } type;

	if (argv[4][0] == 'n')
		type = TEST_NEW;
	else if (argv[4][0] == 'f')
		type = TEST_FREE;
	else if (argv[4][0] == 's')
		type = TEST_SET;
	else
		UT_FATAL("invalid type");

	if (!exists) {
		if ((pop = pmemobj_create(path, POBJ_LAYOUT_NAME(recovery),
			0, S_IWUSR | S_IRUSR)) == NULL) {
			UT_FATAL("failed to create pool\n");
		}
	} else {
		if ((pop = pmemobj_open(path, POBJ_LAYOUT_NAME(recovery)))
						== NULL) {
			UT_FATAL("failed to open pool\n");
		}
	}

	TOID(struct root) root = POBJ_ROOT(pop, struct root);

	int lock_type = TX_PARAM_NONE;
	void *lock = NULL;

	if (argv[2][0] == 'y') {
		lock_type = TX_PARAM_MUTEX;
		lock = &D_RW(root)->lock;
	}

	if (type == TEST_SET) {
		if (!exists) {
			TX_BEGIN_PARAM(pop, lock_type, lock) {
				TX_ADD(root);

				TOID(struct foo) f = TX_NEW(struct foo);
				D_RW(root)->foo = f;
				D_RW(f)->bar = BAR_VALUE;
			} TX_END

			TX_BEGIN_PARAM(pop, lock_type, lock) {
				TX_ADD_FIELD(D_RW(root)->foo, bar);

				D_RW(D_RW(root)->foo)->bar = BAR_VALUE * 2;

				/*
				 * Even though flushes are not required inside
				 * of a transaction, this is done here to
				 * suppress irrelevant pmemcheck issues, because
				 * we exit the program before the data is
				 * flushed, while preserving any real ones.
				 */
				pmemobj_persist(pop,
					&D_RW(D_RW(root)->foo)->bar,
					sizeof(int));
				/*
				 * We also need to cleanup the transaction state
				 * of pmemcheck.
				 */
				VALGRIND_PMEMCHECK_END_TX;

				exit(0); /* simulate a crash */
			} TX_END
		} else {
			UT_ASSERT(D_RW(D_RW(root)->foo)->bar == BAR_VALUE);
		}
	} else if (type == TEST_NEW) {
		if (!exists) {
			TX_BEGIN_PARAM(pop, lock_type, lock) {
				TOID(struct foo) f = TX_NEW(struct foo);
				TX_SET(root, foo, f);
				pmemobj_persist(pop,
					&D_RW(root)->foo,
					sizeof(PMEMoid));
				VALGRIND_PMEMCHECK_END_TX;

				exit(0); /* simulate a crash */
			} TX_END

		} else {
			UT_ASSERT(TOID_IS_NULL(D_RW(root)->foo));
		}
	} else { /* TEST_FREE */
		if (!exists) {
			TX_BEGIN_PARAM(pop, lock_type, lock) {
				TX_ADD(root);

				TOID(struct foo) f = TX_NEW(struct foo);
				D_RW(root)->foo = f;
				D_RW(f)->bar = BAR_VALUE;
			} TX_END

			TX_BEGIN_PARAM(pop, lock_type, lock) {
				TX_ADD(root);
				TX_FREE(D_RW(root)->foo);
				D_RW(root)->foo = TOID_NULL(struct foo);
				pmemobj_persist(pop,
					&D_RW(root)->foo,
					sizeof(PMEMoid));
				VALGRIND_PMEMCHECK_END_TX;

				exit(0); /* simulate a crash */
			} TX_END

		} else {
			UT_ASSERT(!TOID_IS_NULL(D_RW(root)->foo));
		}
	}

	UT_ASSERT(pmemobj_check(path, POBJ_LAYOUT_NAME(recovery)));

	pmemobj_close(pop);

	DONE(NULL);
}
