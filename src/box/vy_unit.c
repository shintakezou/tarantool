#include "vy_unit.h"
#include "vy_mem.h"
#include "vy_run.h"
#include "key_def.h"
#include "xlog.h"
#include "replication.h"
#include "vy_write_iterator.h"
#include <small/quota.h>
#include <small/lsregion.h>

extern struct tuple_format_vtab vy_tuple_format_vtab;

static struct tuple *
vy_unit_create_tuple(struct tuple_format *fmt,
		     int64_t lsn, uint32_t a, uint32_t b, uint32_t c)
{
	char buf[16];
	char *buf_end = buf + mp_format(buf, sizeof(buf), "[%u%u%u]", a, b, c);
	struct tuple *t = vy_stmt_new_replace(fmt, buf, buf_end);
	if (t != NULL)
		vy_stmt_set_lsn(t, lsn);
	return t;
}

static struct tuple *
vy_unit_create_tuple_reg(struct tuple_format *fmt, struct lsregion *reg,
			 int64_t lsn, uint32_t a, uint32_t b, uint32_t c)
{
	struct tuple *t = vy_unit_create_tuple(fmt, lsn, a, b, c);
	if (t == NULL)
		return NULL;

	struct tuple *ret = vy_stmt_dup_lsregion(t, reg, 0);

	tuple_unref(t);
	return ret;
}

static struct tuple *
vy_unit_create_upsert(struct tuple_format *fmt,
		     int64_t lsn, uint32_t a, uint32_t b, uint32_t c)
{
	char buf[16];
	char *buf_end = buf + mp_format(buf, sizeof(buf), "[%u%u%u]", a, b, c);
	struct iovec vec;
	char ops[16];
	size_t ops_size = mp_format(ops, sizeof(ops), "[[%s,%d,%d]]", "+", 1, 1);
	vec.iov_base = (void *)ops;
	vec.iov_len = ops_size;
	struct tuple *t = vy_stmt_new_upsert(fmt, buf, buf_end, &vec, 1);
	if (t != NULL)
		vy_stmt_set_lsn(t, lsn);
	return t;
}

static struct tuple *
vy_unit_create_upsert_reg(struct tuple_format *fmt, struct lsregion *reg,
			 int64_t lsn, uint32_t a, uint32_t b, uint32_t c)
{
	struct tuple *t = vy_unit_create_upsert(fmt, lsn, a, b, c);
	if (t == NULL)
		return NULL;

	struct tuple *ret = vy_stmt_dup_lsregion(t, reg, 0);

	tuple_unref(t);
	return ret;
}



const char *
vy_test_mem_stream()
{
	const char *ret = "ok";
	int rc;
	struct quota quota;
	quota_init(&quota, 16 * 1024 * 1024);
	struct slab_arena arena;

	rc = slab_arena_create(&arena, &quota, 0, 1024 * 1024, MAP_PRIVATE);
	if (rc != 0) {
		ret = "slab_arena_create failed";
		goto free_arena;

	}
	struct lsregion lsreg;
	lsregion_create(&lsreg, &arena);

	struct key_def *def = NULL;
	struct tuple_format *fmt = NULL, *fmtc = NULL, *fmtu = NULL;
	struct vy_mem *mem = NULL;

	uint32_t field_no = 0;
	uint32_t field_type = FIELD_TYPE_UNSIGNED;
	def = box_key_def_new(&field_no, &field_type, 1);
	if (def == NULL) {
		ret = "box_key_def_new failed";
		goto free_all;
	}

	fmt = tuple_format_new(&vy_tuple_format_vtab, &def, 1, 0);
	fmtc = tuple_format_new(&vy_tuple_format_vtab, &def, 1, sizeof(uint64_t));
	fmtu = tuple_format_new(&vy_tuple_format_vtab, &def, 1, sizeof(uint8_t));

	if (fmt == NULL || fmtc == NULL || fmtu == NULL) {
		ret = "tuple_format_new failed";
		goto free_all;
	} else {
		tuple_format_ref(fmt, 1);
		tuple_format_ref(fmtc, 1);
		tuple_format_ref(fmtu, 1);
	}

	mem = vy_mem_new(&lsreg, 0, def, fmt, fmtc, fmtu, 0);
	if (mem == NULL) {
		ret = "vy_mem_new failed";
		goto free_all;
	}

	for (uint32_t i = 0; i < 100; i++) {
		struct tuple *t = vy_unit_create_tuple_reg(fmt, &lsreg, 100, i, i, i);
		if (t == NULL) {
			ret = "tuple_new failed";
			goto free_all;
		}
		vy_mem_insert(mem, t);
		vy_mem_commit_stmt(mem, t);
	}

	struct vy_mem_stream strm;
	vy_mem_stream_open(&strm, mem);
	for (uint32_t i = 0; i < 100; i++) {
		struct tuple *t;
		rc = strm.base.iface->next(&strm.base, &t);
		if (rc != 0) {
			ret = "vy_mem_stream_next failed";
			goto close_stream;
		}
		const char *data = tuple_data(t);
		if (mp_decode_array(&data) != 3) {
			ret = "wrong tuple";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i) {
			ret = "wrong tuple";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i) {
			ret = "wrong tuple";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i) {
			ret = "wrong tuple";
			goto close_stream;
		}
	}

	struct tuple *t;
	rc = strm.base.iface->next(&strm.base, &t);
	if (rc != 0) {
		ret = "vy_mem_stream_next failed";
	} else if (t != NULL) {
		ret = "vy_mem_stream_next: steam is not ended";
	}

close_stream:
	if (strm.base.iface->close)
		strm.base.iface->close(&strm.base);

free_all:
	if (mem)
		vy_mem_delete(mem);

	if (fmt)
		tuple_format_delete(fmt);
	if (fmtc)
		tuple_format_delete(fmtc);
	if (fmtu)
		tuple_format_delete(fmtu);
	if (def)
		box_key_def_delete(def);

	lsregion_destroy(&lsreg);

free_arena:
	slab_arena_destroy(&arena);

	return ret;
}

#if 0
const char *
vy_test_run_stream()
{
	const char *ret = "ok";

	struct vy_run_env env;
	vy_run_env_create(&env);

	int rc;
	struct quota quota;
	quota_init(&quota, 16 * 1024 * 1024);
	struct slab_arena arena;

	rc = slab_arena_create(&arena, &quota, 0, 1024 * 1024, MAP_PRIVATE);
	if (rc != 0) {
		ret = "slab_arena_create failed";
		goto free_arena;
	}

	struct slab_cache slabc;
	slab_cache_create(&slabc, &arena);

	uint32_t field_no = 0;
	uint32_t field_type = FIELD_TYPE_UNSIGNED;
	struct key_def *def = box_key_def_new(&field_no, &field_type, 1);
	if (def == NULL) {
		ret = "box_key_def_new failed";
		goto free_all;
	}

	struct tuple_format *fmt =
		tuple_format_new(&vy_tuple_format_vtab, &def, 1, 0);
	struct tuple_format *fmtc =
		tuple_format_new(&vy_tuple_format_vtab, &def, 1, sizeof(uint64_t));
	struct tuple_format *fmtu =
		tuple_format_new(&vy_tuple_format_vtab, &def, 1, sizeof(uint8_t));

	if (fmt == NULL || fmtc == NULL || fmtu == NULL) {
		ret = "tuple_format_new failed";
		goto free_all;
	} else {
		tuple_format_ref(fmt, 1);
		tuple_format_ref(fmtc, 1);
		tuple_format_ref(fmtu, 1);
	}

	struct vy_run *run = vy_run_new(0);
	if (run == NULL) {
		ret = "vy_run_new failed";
		goto free_all;
	}

	char path[PATH_MAX] = "run_XXXXXX";
	mktemp(path);
	if (path[0] == 0) {
		ret = "mktemp failed";
		goto free_all;
	}

	struct xlog data_xlog;
	struct xlog_meta meta = {
		.filetype = XLOG_META_TYPE_RUN,
		.instance_uuid = INSTANCE_UUID,
	};
	if (xlog_create(&data_xlog, path, &meta) < 0)
		goto free_all;

	struct vy_run_info *run_info = &run->info;
	run_info->min_lsn = INT64_MAX;
	run_info->max_lsn = -1;

	struct vy_page_info *page = NULL;
	const char *region_key;
	struct ibuf page_index_buf;
	ibuf_create(&page_index_buf, &slabc, sizeof(uint32_t) * 4096);




	/* Sync data and link the file to the final name. */
	if (xlog_sync(&data_xlog) < 0 ||
	    xlog_rename(&data_xlog) < 0)
		goto err_close_xlog;



	vy_log_tx_begin();
	vy_log_prepare_run(index->index_def->opts.lsn, run->id);
	if (vy_log_tx_commit() < 0) {
		vy_run_unref(run);
		return NULL;
	}


	vy_unit_create_tuple_reg(fmt, &lsreg, 100, i, i, i);

	for (uint32_t i = 0; i < 100; i++) {
		struct tuple *t = vy_unit_create_tuple_reg(fmt, &lsreg, 100, i, i, i);
		if (t == NULL) {
			ret = "tuple_new failed";
			goto free_all;
		}
		vy_mem_insert(mem, t);
		vy_mem_commit_stmt(mem, t);
	}

	struct vy_mem_stream strm;
	vy_mem_stream_open(&strm, mem);
	for (uint32_t i = 0; i < 100; i++) {
		struct tuple *t;
		rc = strm.base.iface->next(&strm.base, &t);
		if (rc != 0) {
			ret = "vy_mem_stream_next failed";
			goto close_stream;
		}
		const char *data = tuple_data(t);
		if (mp_decode_array(&data) != 3) {
			ret = "wrong tuple";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i) {
			ret = "wrong tuple";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i) {
			ret = "wrong tuple";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i) {
			ret = "wrong tuple";
			goto close_stream;
		}
	}

	close_stream:
	if (strm.base.iface->close)
		strm.base.iface->close(&strm.base);

free_all:
	if (run)
		vy_run_unref(run);

	if (fmt)
		tuple_format_delete(fmt);
	if (fmtc)
		tuple_format_delete(fmtc);
	if (fmtu)
		tuple_format_delete(fmtu);
	if (def)
		box_key_def_delete(def);

	slab_cache_destroy(&slabc);

free_arena:
	slab_arena_destroy(&arena);

	vy_run_env_destroy(&env);

	return ret;
}
#endif

const char *
vy_test_write_iterator()
{
	const char *ret = "ok";
	int rc;
	struct quota quota;
	quota_init(&quota, 16 * 1024 * 1024);
	struct slab_arena arena;

	rc = slab_arena_create(&arena, &quota, 0, 1024 * 1024, MAP_PRIVATE);
	if (rc != 0) {
		ret = "slab_arena_create failed";
		goto free_arena;

	}

	struct lsregion lsreg;
	lsregion_create(&lsreg, &arena);

	struct key_def *def = NULL;
	struct tuple_format *fmt = NULL, *fmtc = NULL, *fmtu = NULL;
	struct vy_mem *mem = NULL, *mem2 = NULL;
	struct vy_write_iterator *wi = NULL;

	uint32_t field_no = 0;
	uint32_t field_type = FIELD_TYPE_UNSIGNED;
	def = box_key_def_new(&field_no, &field_type, 1);
	if (def == NULL) {
		ret = "box_key_def_new failed";
		goto free_all;
	}

	fmt = tuple_format_new(&vy_tuple_format_vtab, &def, 1, 0);
	fmtc = tuple_format_new(&vy_tuple_format_vtab, &def, 1, sizeof(uint64_t));
	fmtu = tuple_format_new(&vy_tuple_format_vtab, &def, 1, sizeof(uint8_t));

	if (fmt == NULL || fmtc == NULL || fmtu == NULL) {
		ret = "tuple_format_new failed";
		goto free_all;
	} else {
		tuple_format_ref(fmt, 1);
		tuple_format_ref(fmtc, 1);
		tuple_format_ref(fmtu, 1);
	}

	mem = vy_mem_new(&lsreg, 0, def, fmt, fmtc, fmtu, 0);
	if (mem == NULL) {
		ret = "vy_mem_new failed";
		goto free_all;
	}

	for (uint32_t i = 0; i < 10; i++) {
		for (uint32_t j = 0; j < 10; j++) {
			struct tuple *t = vy_unit_create_upsert_reg(fmtu, &lsreg, j * 10 + 10, i, i, i);
			if (t == NULL) {
				ret = "tuple_new failed";
				goto free_all;
			}
			vy_mem_insert(mem, t);
			vy_mem_commit_stmt(mem, t);
		}
	}

	wi = vy_write_iterator_new(def, fmt, fmtu, true, 7, true, 1000);
	if (wi == NULL) {
		ret = "vy_write_iterator_new failed";
		goto free_all;
	}

	if (vy_write_iterator_add_mem(wi, mem)) {
		ret = "vy_write_iterator_add_mem failed";
		goto free_all;
	}
	if (vy_write_iterator_start(wi)) {
		ret = "vy_write_iterator_start failed";
		goto free_all;
	}

	for (uint32_t i = 0; i < 10; i++) {
		const struct tuple *t;
		rc = vy_write_iterator_next(wi, &t);
		if (rc != 0) {
			ret = "vy_write_iterator_next failed";
			goto close_stream;
		}
		if (vy_stmt_type(t) != IPROTO_REPLACE) {
			ret = "wrong tuple 0";
			goto close_stream;
		}
		const char *data = tuple_data(t);
		if (mp_decode_array(&data) != 3) {
			ret = "wrong tuple 1";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i) {
			ret = "wrong tuple 2";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i + 9) {
			assert(false);
			ret = "wrong tuple 3";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i) {
			ret = "wrong tuple 4";
			goto close_stream;
		}
	}

	const struct tuple *t;
	rc = vy_write_iterator_next(wi, &t);
	if (rc != 0) {
		ret = "vy_write_iterator_next failed";
	} else if (t != NULL) {
		ret = "vy_write_iterator_next: steam is not ended";
	}

	vy_write_iterator_cleanup(wi);
	vy_write_iterator_delete(wi);

	wi = vy_write_iterator_new(def, fmt, fmtu, true, 7, false, 0);
	if (wi == NULL) {
		ret = "vy_write_iterator_new failed";
		goto free_all;
	}

	if (vy_write_iterator_add_mem(wi, mem)) {
		ret = "vy_write_iterator_add_mem failed";
		goto free_all;
	}
	if (vy_write_iterator_start(wi)) {
		ret = "vy_write_iterator_start failed";
		goto free_all;
	}

	for (uint32_t i = 0; i < 10; i++) {
		for (uint32_t j = 0; j < 10; j++) {
			const struct tuple *t;
			rc = vy_write_iterator_next(wi, &t);
			if (rc != 0) {
				ret = "vy_write_iterator_next failed";
				goto close_stream;
			}
			if (vy_stmt_type(t) != IPROTO_UPSERT) {
				ret = "wrong tuple 0";
				goto close_stream;
			}
			const char *data = tuple_data(t);
			if (mp_decode_array(&data) != 3) {
				ret = "wrong tuple 1 ";
				goto close_stream;
			}
			if (mp_decode_uint(&data) != i) {
				ret = "wrong tuple 2 ";
				goto close_stream;
			}
			if (mp_decode_uint(&data) != i) {
				assert(false);
				ret = "wrong tuple 3";
				goto close_stream;
			}
			if (mp_decode_uint(&data) != i) {
				ret = "wrong tuple 4";
				goto close_stream;
			}
		}
	}

	rc = vy_write_iterator_next(wi, &t);
	if (rc != 0) {
		ret = "vy_write_iterator_next failed";
	} else if (t != NULL) {
		ret = "vy_write_iterator_next: steam is not ended";
	}


	vy_mem_delete(mem);

	vy_write_iterator_cleanup(wi);
	vy_write_iterator_delete(wi);

	mem = vy_mem_new(&lsreg, 0, def, fmt, fmtc, fmtu, 0);
	if (mem == NULL) {
		ret = "vy_mem_new failed";
		goto free_all;
	}

	for (uint32_t i = 0; i < 10; i++) {
		struct tuple *t = vy_unit_create_upsert_reg(fmtu, &lsreg, 10, i, i, i);
		if (t == NULL) {
			ret = "tuple_new failed";
			goto free_all;
		}
		vy_mem_insert(mem, t);
		vy_mem_commit_stmt(mem, t);
	}

	mem2 = vy_mem_new(&lsreg, 0, def, fmt, fmtc, fmtu, 0);
	if (mem2 == NULL) {
		ret = "vy_mem_new failed";
		goto free_all;
	}

	for (uint32_t i = 0; i < 10; i++) {
		struct tuple *t = vy_unit_create_upsert_reg(fmtu, &lsreg, 20, i, i, i);
		if (t == NULL) {
			ret = "tuple_new failed";
			goto free_all;
		}
		vy_mem_insert(mem, t);
		vy_mem_commit_stmt(mem, t);
	}

	wi = vy_write_iterator_new(def, fmt, fmtu, true, 7, true, 1000);
	if (wi == NULL) {
		ret = "vy_write_iterator_new failed";
		goto free_all;
	}
	if (vy_write_iterator_add_mem(wi, mem)) {
		ret = "vy_write_iterator_add_mem failed";
		goto free_all;
	}
	if (vy_write_iterator_add_mem(wi, mem2)) {
		ret = "vy_write_iterator_add_mem failed";
		goto free_all;
	}
	if (vy_write_iterator_start(wi)) {
		ret = "vy_write_iterator_start failed";
		goto free_all;
	}

	for (uint32_t i = 0; i < 10; i++) {
		const struct tuple *t;
		rc = vy_write_iterator_next(wi, &t);
		if (rc != 0) {
			ret = "vy_write_iterator_next failed";
			goto close_stream;
		}
		if (vy_stmt_type(t) != IPROTO_REPLACE) {
			ret = "wrong tuple 0";
			goto close_stream;
		}
		const char *data = tuple_data(t);
		if (mp_decode_array(&data) != 3) {
			ret = "wrong tuple 1";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i) {
			ret = "wrong tuple 2";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i + 1) {
			assert(false);
			ret = "wrong tuple 3";
			goto close_stream;
		}
		if (mp_decode_uint(&data) != i) {
			ret = "wrong tuple 4";
			goto close_stream;
		}
	}


close_stream:
	vy_write_iterator_cleanup(wi);
	vy_write_iterator_delete(wi);

free_all:
	if (mem)
		vy_mem_delete(mem);
	if (mem2)
		vy_mem_delete(mem2);

	if (fmt)
		tuple_format_delete(fmt);
	if (fmtc)
		tuple_format_delete(fmtc);
	if (fmtu)
		tuple_format_delete(fmtu);
	if (def)
		box_key_def_delete(def);

	lsregion_destroy(&lsreg);

free_arena:
	slab_arena_destroy(&arena);

	return ret;
}
