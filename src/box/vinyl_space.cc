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
#include "vinyl_space.h"
#include "vinyl_index.h"
#include "xrow.h"
#include "txn.h"
#include "vinyl.h"
#include "vy_stmt.h"
#include "scoped_guard.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

VinylSpace::VinylSpace(Engine *e)
	:Handler(e)
{}

void
VinylSpace::applyInitialJoinRow(struct space *space, struct request *request)
{
	assert(request->header != NULL);
	struct vy_env *env = ((VinylEngine *)space->handler->engine)->env;

	struct vy_tx *tx = vy_begin(env);
	if (tx == NULL)
		diag_raise();

	int64_t signature = request->header->lsn;

	struct txn_stmt stmt;
	memset(&stmt, 0, sizeof(stmt));

	int rc;
	switch (request->type) {
	case IPROTO_REPLACE:
		rc = vy_replace(tx, &stmt, space, request);
		break;
	case IPROTO_UPSERT:
		rc = vy_upsert(tx, &stmt, space, request);
		break;
	case IPROTO_DELETE:
		rc = vy_delete(tx, &stmt, space, request);
		break;
	default:
		tnt_raise(ClientError, ER_UNKNOWN_REQUEST_TYPE,
			  (uint32_t) request->type);
	}
	if (rc != 0)
		diag_raise();

	if (stmt.old_tuple)
		tuple_unref(stmt.old_tuple);
	if (stmt.new_tuple)
		tuple_unref(stmt.new_tuple);

	if (vy_prepare(tx)) {
		vy_rollback(tx);
		diag_raise();
	}
	vy_commit(tx, signature);
}

/*
 * Four cases:
 *  - insert in one index
 *  - insert in multiple indexes
 *  - replace in one index
 *  - replace in multiple indexes.
 */
struct tuple *
VinylSpace::executeReplace(struct txn *txn, struct space *space,
			   struct request *request)
{
	assert(request->index_id == 0);
	struct vy_tx *tx = (struct vy_tx *)txn->engine_tx;
	struct txn_stmt *stmt = txn_current_stmt(txn);

	if (vy_replace(tx, stmt, space, request))
		diag_raise();
	return stmt->new_tuple;
}

struct tuple *
VinylSpace::executeDelete(struct txn *txn, struct space *space,
                          struct request *request)
{
	struct txn_stmt *stmt = txn_current_stmt(txn);
	struct vy_tx *tx = (struct vy_tx *) txn->engine_tx;
	if (vy_delete(tx, stmt, space, request))
		diag_raise();
	/*
	 * Delete may or may not set stmt->old_tuple, but we
	 * always return NULL.
	 */
	return NULL;
}

struct tuple *
VinylSpace::executeUpdate(struct txn *txn, struct space *space,
                          struct request *request)
{
	struct vy_tx *tx = (struct vy_tx *)txn->engine_tx;
	struct txn_stmt *stmt = txn_current_stmt(txn);
	if (vy_update(tx, stmt, space, request) != 0)
		diag_raise();
	return stmt->new_tuple;
}

void
VinylSpace::executeUpsert(struct txn *txn, struct space *space,
                           struct request *request)
{
	struct vy_tx *tx = (struct vy_tx *)txn->engine_tx;
	struct txn_stmt *stmt = txn_current_stmt(txn);
	if (vy_upsert(tx, stmt, space, request) != 0)
		diag_raise();
}

Index *
VinylSpace::createIndex(struct space *space, struct index_def *index_def)
{
	VinylEngine *engine = (VinylEngine *) this->engine;
	struct vy_index *db = vy_index_create(engine->env, index_def, space);
	if (db == NULL)
		diag_raise();
	auto guard = make_scoped_guard([=] { vy_index_destroy(db); });
	VinylIndex *i = new VinylIndex(index_def, db);
	/* @db will be destroyed by VinylIndex destructor. */
	guard.is_active = false;
	return i;
}

void
VinylSpace::commitIndex(Index *index)
{
	VinylIndex *i = (VinylIndex *)index;
	vy_index_commit(i->db);
}

void
VinylSpace::dropIndex(Index *index)
{
	VinylIndex *i = (VinylIndex *)index;
	vy_index_drop(i->db);
}

void
VinylSpace::commitTruncateSpace(struct space *old_space,
				struct space *new_space)
{
	vy_commit_truncate_space(old_space, new_space);
}

void
VinylSpace::prepareAlterSpace(struct space *old_space, struct space *new_space)
{
	if (vy_prepare_alter_space(old_space, new_space) != 0)
		diag_raise();
}

void
VinylSpace::commitAlterSpace(struct space *old_space, struct space *new_space)
{
	if (new_space == NULL || new_space->index_count == 0) {
		/* This is a drop space. */
		return;
	}
	if (vy_commit_alter_space(old_space, new_space) != 0)
		diag_raise();
}
