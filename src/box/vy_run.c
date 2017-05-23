/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
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
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "vy_run.h"
#include "fiber.h"
#include "ipc.h"
#include "coeio.h"
#include "xrow.h"
#include "xlog.h"
#include "fio.h"
#include "memory.h"

/**
 * coio task for vinyl page read
 */
struct vy_page_read_task {
	/** parent */
	struct coio_task base;
	/** vinyl page metadata */
	struct vy_page_info page_info;
	/** vy_slice with fd - ref. counted */
	struct vy_slice *slice;
	/** vy_run_env - contains environment with task mempool */
	struct vy_run_env *run_env;
	/** [out] resulting vinyl page */
	struct vy_page *page;
	/** [out] result code */
	int rc;
};

/** Destructor for env->zdctx_key thread-local variable */
static void
vy_free_zdctx(void *arg)
{
	assert(arg != NULL);
	ZSTD_freeDStream(arg);
}

/**
 * Initialize vinyl run environment
 */
void
vy_run_env_create(struct vy_run_env *env)
{
	tt_pthread_key_create(&env->zdctx_key, vy_free_zdctx);

	struct slab_cache *slab_cache = cord_slab_cache();
	mempool_create(&env->read_task_pool, slab_cache,
		       sizeof(struct vy_page_read_task));
}

/**
 * Destroy vinyl run environment
 */
void
vy_run_env_destroy(struct vy_run_env *env)
{
	mempool_destroy(&env->read_task_pool);
	tt_pthread_key_delete(env->zdctx_key);
}

/**
 * Initialize page info struct
 *
 * @retval 0 for Success
 * @retval -1 for error
 */
int
vy_page_info_create(struct vy_page_info *page_info, uint64_t offset,
		    const struct tuple *min_key, const struct key_def *key_def)
{
	memset(page_info, 0, sizeof(*page_info));
	page_info->offset = offset;
	page_info->unpacked_size = 0;
	struct region *region = &fiber()->gc;
	size_t used = region_used(region);
	uint32_t size;
	const char *region_key = tuple_extract_key(min_key, key_def, &size);
	if (region_key == NULL)
		return -1;
	page_info->min_key = vy_key_dup(region_key);
	region_truncate(region, used);
	return page_info->min_key == NULL ? -1 : 0;
}

/**
 * Destroy page info struct
 */
void
vy_page_info_destroy(struct vy_page_info *page_info)
{
	if (page_info->min_key != NULL)
		free(page_info->min_key);
}

struct vy_run *
vy_run_new(int64_t id)
{
	struct vy_run *run = (struct vy_run *)malloc(sizeof(struct vy_run));
	if (unlikely(run == NULL)) {
		diag_set(OutOfMemory, sizeof(struct vy_run), "malloc",
			 "struct vy_run");
		return NULL;
	}
	memset(&run->info, 0, sizeof(run->info));
	run->id = id;
	run->dump_lsn = -1;
	run->fd = -1;
	run->refs = 1;
	run->compacted_slice_count = 0;
	rlist_create(&run->in_index);
	rlist_create(&run->in_unused);
	TRASH(&run->info.bloom);
	run->info.has_bloom = false;
	return run;
}

void
vy_run_delete(struct vy_run *run)
{
	assert(run->refs == 0);
	if (run->fd >= 0 && close(run->fd) < 0)
		say_syserror("close failed");
	if (run->info.page_infos != NULL) {
		uint32_t page_no;
		for (page_no = 0; page_no < run->info.count; ++page_no)
			vy_page_info_destroy(run->info.page_infos + page_no);
		free(run->info.page_infos);
	}
	if (run->info.has_bloom)
		bloom_destroy(&run->info.bloom, runtime.quota);
	free(run->info.min_key);
	free(run->info.max_key);
	TRASH(run);
	free(run);
}

/**
 * Set first_page_no member and possibly throw away slice->begin tuple.
 * Search a page in a run that may contain the beginning (since one particular
 * key can spread through several pages) of slice->begin key
 *  and save it in first_page_no member. If the slice->begin key is definitely
 *  not present in the run then unref and nullify slice->begin.
 * Normally searches the highest (with maximal index) page that
 *  has min_key < slice->begin (not '<=' like in last page search).
 */
static void
vy_slice_set_up_beginning(struct vy_slice *slice, const struct key_def *key_def)
{
	if (slice->begin == NULL || slice->run->info.count == 0) {
		slice->first_page_no = 0;
		return;
	}
	uint32_t beg = 0;
	uint32_t end = slice->run->info.count;
	while (beg != end) {
		uint32_t mid = beg + (end - beg) / 2;
		struct vy_page_info *page_info = vy_run_page_info(slice->run,
								  mid);
		int cmp = key_compare(page_info->min_key,
				      tuple_data(slice->begin), key_def);
		if (cmp < 0)
			beg = mid + 1;
		else
			end = mid;
	}
	if (end == 0) {
		/**
		 * The first page's min key is >= slice->begin.
		 * Thus slice->begin is useless and could be omitted.
		 */
		tuple_unref(slice->begin);
		slice->begin = NULL;
		slice->first_page_no = 0;
		return;
	}
	/**
	 * end page's min key is >= slice->begin (or end is out of bounds),
	 * and the previous page is the goal.
	 */
	slice->first_page_no = end - 1;
}

/**
 * Set last_page_no member and possibly throw away slice->end tuple.
 * Search a page in a run that may contain the end (since one particular
 * key can spread through several pages) of slice->end key
 *  and save it in last_page_no member. If the slice->end key is definitely
 *  not present in the run then unref and nullify slice->end.
 * Normally searches the highest (with maximal index) page that
 *  has min_key <= slice->end (not '<' like in first page search).
 */
static void
vy_slice_set_up_end(struct vy_slice *slice, const struct key_def *key_def)
{
	if (slice->run->info.count == 0) {
		slice->last_page_no = 0;
		return;
	} else if (slice->end == NULL) {
		slice->last_page_no = slice->run->info.count - 1;
		return;
	}
	uint32_t beg = 0;
	uint32_t end = slice->run->info.count;
	while (beg != end) {
		uint32_t mid = beg + (end - beg) / 2;
		struct vy_page_info *page_info = vy_run_page_info(slice->run,
								  mid);
		int cmp = key_compare(page_info->min_key,
				      tuple_data(slice->end), key_def);
		if (cmp <= 0)
			beg = mid + 1;
		else
			end = mid;
	}
	if (end == 0) {
		/**
		 * The first page's min key is > slice->end.
		 * Actually that means that the slice is empty, but there's
		 *  no way to mark it as empty in this unlikely case.
		 */
		assert(slice->first_page_no  == 0); /* begin's less than end */
		assert(slice->begin == NULL); /* begin's less than end */
		slice->last_page_no = 0;
		return;
	}
	/**
	 * end page's min key is > slice->end (or end is out of bounds),
	 * and the previous page is the goal.
	 */
	slice->last_page_no = end - 1;
}

struct vy_slice *
vy_slice_new(int64_t id, struct vy_run *run,
	     struct tuple *begin, struct tuple *end,
	     const struct key_def *key_def)
{
	struct vy_slice *slice = malloc(sizeof(*slice));
	if (slice == NULL) {
		diag_set(OutOfMemory, sizeof(*slice),
			 "malloc", "struct vy_slice");
		return NULL;
	}
	memset(slice, 0, sizeof(*slice));
	slice->id = id;
	slice->run = run;
	vy_run_ref(run);
	if (begin != NULL)
		tuple_ref(begin);
	slice->begin = begin;
	if (end != NULL)
		tuple_ref(end);
	slice->end = end;
	rlist_create(&slice->in_range);
	ipc_cond_create(&slice->pin_cond);
	/*
	 * Lookup the first and the last page of this slice
	 * in the run and estimate the slice size.
	 */
	vy_slice_set_up_beginning(slice, key_def);
	vy_slice_set_up_end(slice, key_def);
	assert(slice->last_page_no >= slice->first_page_no);
	uint64_t count = slice->last_page_no - slice->first_page_no + 1;
	slice->keys = DIV_ROUND_UP(run->info.keys * count,
				   run->info.count);
	slice->size = DIV_ROUND_UP(run->info.size * count,
				   run->info.count);
	return slice;
}

void
vy_slice_delete(struct vy_slice *slice)
{
	assert(slice->pin_count == 0);
	vy_run_unref(slice->run);
	if (slice->begin != NULL)
		tuple_unref(slice->begin);
	if (slice->end != NULL)
		tuple_unref(slice->end);
	ipc_cond_destroy(&slice->pin_cond);
	TRASH(slice);
	free(slice);
}

int
vy_slice_cut(struct vy_slice *slice, int64_t id,
	     struct tuple *begin, struct tuple *end,
	     const struct key_def *key_def,
	     struct vy_slice **result)
{
	*result = NULL;

	if (begin != NULL && slice->end != NULL &&
	    vy_key_compare(begin, slice->end, key_def) >= 0)
		return 0; /* no intersection: begin >= slice->end */

	if (end != NULL && slice->begin != NULL &&
	    vy_key_compare(end, slice->begin, key_def) <= 0)
		return 0; /* no intersection: end <= slice->end */

	/* begin = MAX(begin, slice->begin) */
	if (slice->begin != NULL &&
	    (begin == NULL || vy_key_compare(begin, slice->begin, key_def) < 0))
		begin = slice->begin;

	/* end = MIN(end, slice->end) */
	if (slice->end != NULL &&
	    (end == NULL || vy_key_compare(end, slice->end, key_def) > 0))
		end = slice->end;

	*result = vy_slice_new(id, slice->run, begin, end, key_def);
	if (*result == NULL)
		return -1; /* OOM */

	return 0;
}

/**
 * Decode page information from xrow.
 *
 * @param[out] page Page information.
 * @param xrow      Xrow to decode.
 * @param filename  Filename for error reporting.
 *
 * @retval  0 Success.
 * @retval -1 Error.
 */
int
vy_page_info_decode(struct vy_page_info *page, const struct xrow_header *xrow,
		    const char *filename)
{
	assert(xrow->type == VY_INDEX_PAGE_INFO);
	const char *pos = xrow->body->iov_base;
	memset(page, 0, sizeof(*page));
	uint64_t key_map = vy_page_info_key_map;
	uint32_t map_size = mp_decode_map(&pos);
	uint32_t map_item;
	const char *key_beg;
	for (map_item = 0; map_item < map_size; ++map_item) {
		uint32_t key = mp_decode_uint(&pos);
		key_map &= ~(1ULL << key);
		switch (key) {
		case VY_PAGE_INFO_OFFSET:
			page->offset = mp_decode_uint(&pos);
			break;
		case VY_PAGE_INFO_SIZE:
			page->size = mp_decode_uint(&pos);
			break;
		case VY_PAGE_INFO_ROW_COUNT:
			page->count = mp_decode_uint(&pos);
			break;
		case VY_PAGE_INFO_MIN_KEY:
			key_beg = pos;
			mp_next(&pos);
			page->min_key = vy_key_dup(key_beg);
			if (page->min_key == NULL)
				return -1;
			break;
		case VY_PAGE_INFO_UNPACKED_SIZE:
			page->unpacked_size = mp_decode_uint(&pos);
			break;
		case VY_PAGE_INFO_PAGE_INDEX_OFFSET:
			page->page_index_offset = mp_decode_uint(&pos);
			break;
		default:
			diag_set(ClientError, ER_INVALID_INDEX_FILE, filename,
				 tt_sprintf("Can't decode page info: "
					    "unknown key %u", (unsigned)key));
			return -1;
		}
	}
	if (key_map) {
		enum vy_page_info_key key = bit_ctz_u64(key_map);
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filename,
			 tt_sprintf("Can't decode page info: "
				    "missing mandatory key %s",
				    vy_page_info_key_name(key)));
		return -1;
	}

	return 0;
}

/**
 * Read bloom filter from given buffer.
 * @param bloom - a bloom filter to read.
 * @param buffer[in/out] - a buffer to read from.
 *  The pointer is incremented on the number of bytes read.
 * @param filename Filename for error reporting.
 * @return - 0 on success or -1 on format/memory error
 */
int
vy_run_bloom_decode(struct bloom *bloom, const char **buffer,
		    const char *filename)
{
	const char **pos = buffer;
	memset(bloom, 0, sizeof(*bloom));
	uint32_t array_size = mp_decode_array(pos);
	if (array_size != 4) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filename,
			 tt_sprintf("Can't decode bloom meta: "
				    "wrong array size (expected %d, got %u)",
				    4, (unsigned)array_size));
		return -1;
	}
	uint64_t version = mp_decode_uint(pos);
	if (version != VY_BLOOM_VERSION) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filename,
			 tt_sprintf("Can't decode bloom meta: "
				    "wrong version (expected %d, got %u)",
				    VY_BLOOM_VERSION, (unsigned)version));
	}
	bloom->table_size = mp_decode_uint(pos);
	bloom->hash_count = mp_decode_uint(pos);
	size_t table_size = mp_decode_binl(pos);
	if (table_size != bloom_store_size(bloom)) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filename,
			 tt_sprintf("Can't decode bloom meta: "
				    "wrong table size (expected %zu, got %zu)",
				    bloom_store_size(bloom), table_size));
		return -1;
	}
	if (bloom_load_table(bloom, *pos, runtime.quota) != 0) {
		diag_set(OutOfMemory, bloom_store_size(bloom), "mmap", "bloom");
		return -1;
	}
	*pos += table_size;
	return 0;
}

/**
 * Decode the run metadata from xrow.
 *
 * @param xrow xrow to decode
 * @param[out] run_info the run information
 * @param filename File name for error reporting.
 *
 * @retval  0 success
 * @retval -1 error (check diag)
 */
int
vy_run_info_decode(struct vy_run_info *run_info,
		   const struct xrow_header *xrow,
		   const char *filename)
{
	assert(xrow->type == VY_INDEX_RUN_INFO);
	/* decode run */
	const char *pos = xrow->body->iov_base;
	memset(run_info, 0, sizeof(*run_info));
	uint64_t key_map = vy_run_info_key_map;
	uint32_t map_size = mp_decode_map(&pos);
	uint32_t map_item;
	const char *tmp;
	/* decode run values */
	for (map_item = 0; map_item < map_size; ++map_item) {
		uint32_t key = mp_decode_uint(&pos);
		key_map &= ~(1ULL << key);
		switch (key) {
		case VY_RUN_INFO_MIN_KEY:
			tmp = pos;
			mp_next(&pos);
			run_info->min_key = vy_key_dup(tmp);
			if (run_info->min_key == NULL)
				return -1;
			break;
		case VY_RUN_INFO_MAX_KEY:
			tmp = pos;
			mp_next(&pos);
			run_info->max_key = vy_key_dup(tmp);
			if (run_info->max_key == NULL)
				return -1;
			break;
		case VY_RUN_INFO_MIN_LSN:
			run_info->min_lsn = mp_decode_uint(&pos);
			break;
		case VY_RUN_INFO_MAX_LSN:
			run_info->max_lsn = mp_decode_uint(&pos);
			break;
		case VY_RUN_INFO_PAGE_COUNT:
			run_info->count = mp_decode_uint(&pos);
			break;
		case VY_RUN_INFO_BLOOM:
			if (vy_run_bloom_decode(&run_info->bloom, &pos,
						filename) == 0)
				run_info->has_bloom = true;
			else
				return -1;
			break;
		default:
			diag_set(ClientError, ER_INVALID_INDEX_FILE, filename,
				"Can't decode run info: unknown key %u",
				(unsigned)key);
			return -1;
		}
	}
	if (key_map) {
		enum vy_run_info_key key = bit_ctz_u64(key_map);
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filename,
			 tt_sprintf("Can't decode run info: "
				    "missing mandatory key %s",
				    vy_run_info_key_name(key)));
		return -1;
	}
	return 0;
}

struct vy_page *
vy_page_new(const struct vy_page_info *page_info)
{
	struct vy_page *page = malloc(sizeof(*page));
	if (page == NULL) {
		diag_set(OutOfMemory, sizeof(*page),
			 "load_page", "page cache");
		return NULL;
	}
	page->count = page_info->count;
	page->unpacked_size = page_info->unpacked_size;
	page->page_index = calloc(page_info->count, sizeof(uint32_t));
	if (page->page_index == NULL) {
		diag_set(OutOfMemory, page_info->count * sizeof(uint32_t),
			 "malloc", "page->page_index");
		free(page);
		return NULL;
	}

	page->data = (char *)malloc(page_info->unpacked_size);
	if (page->data == NULL) {
		diag_set(OutOfMemory, page_info->unpacked_size,
			 "malloc", "page->data");
		free(page->page_index);
		free(page);
		return NULL;
	}
	return page;
}

void
vy_page_delete(struct vy_page *page)
{
	uint32_t *page_index = page->page_index;
	char *data = page->data;
#if !defined(NDEBUG)
	memset(page->page_index, '#', sizeof(uint32_t) * page->count);
	memset(page->data, '#', page->unpacked_size);
	memset(page, '#', sizeof(*page));
#endif /* !defined(NDEBUG) */
	free(page_index);
	free(data);
	free(page);
}

int
vy_page_xrow(struct vy_page *page, uint32_t stmt_no,
	     struct xrow_header *xrow)
{
	assert(stmt_no < page->count);
	const char *data = page->data + page->page_index[stmt_no];
	const char *data_end = stmt_no + 1 < page->count ?
			       page->data + page->page_index[stmt_no + 1] :
			       page->data + page->unpacked_size;
	return xrow_header_decode(xrow, &data, data_end);
}

/* {{{ vy_run_iterator vy_run_iterator support functions */

/**
 * Read raw stmt data from the page
 * @param page          Page.
 * @param stmt_no       Statement position in the page.
 * @param key_def       Key definition of an index.
 * @param format        Format for REPLACE/DELETE tuples.
 * @param upsert_format Format for UPSERT tuples.
 * @param is_primary    True if the index is primary.
 *
 * @retval not NULL Statement read from page.
 * @retval     NULL Memory error.
 */
static struct tuple *
vy_page_stmt(struct vy_page *page, uint32_t stmt_no,
	     const struct key_def *key_def, struct tuple_format *format,
	     struct tuple_format *upsert_format, bool is_primary)
{
	struct xrow_header xrow;
	if (vy_page_xrow(page, stmt_no, &xrow) != 0)
		return NULL;
	struct tuple_format *format_to_use = (xrow.type == IPROTO_UPSERT)
					     ? upsert_format : format;
	return vy_stmt_decode(&xrow, key_def, format_to_use, is_primary);
}

/**
 * Get page from LRU cache
 * @retval page if found
 * @retval NULL otherwise
 */
static struct vy_page *
vy_run_iterator_cache_get(struct vy_run_iterator *itr, uint32_t page_no)
{
	if (itr->curr_page != NULL) {
		if (itr->curr_page->page_no == page_no)
			return itr->curr_page;
		if (itr->prev_page != NULL &&
		    itr->prev_page->page_no == page_no) {
			struct vy_page *result = itr->prev_page;
			itr->prev_page = itr->curr_page;
			itr->curr_page = result;
			return result;
		}
	}
	return NULL;
}

/**
 * Touch page in LRU cache.
 * The cache is at least two pages. Ensure that subsequent read keeps
 * the page_no in the cache by moving it to the start of LRU list.
 * @pre page must be in the cache
 */
static void
vy_run_iterator_cache_touch(struct vy_run_iterator *itr, uint32_t page_no)
{
	struct vy_page *page = vy_run_iterator_cache_get(itr, page_no);
	assert(page != NULL);
	(void) page;
}

/**
 * Put page to LRU cache
 */
static void
vy_run_iterator_cache_put(struct vy_run_iterator *itr, struct vy_page *page,
			  uint32_t page_no)
{
	if (itr->prev_page != NULL)
		vy_page_delete(itr->prev_page);
	itr->prev_page = itr->curr_page;
	itr->curr_page = page;
	page->page_no = page_no;
}

/**
 * Clear LRU cache
 */
static void
vy_run_iterator_cache_clean(struct vy_run_iterator *itr)
{
	if (itr->curr_stmt != NULL) {
		tuple_unref(itr->curr_stmt);
		itr->curr_stmt = NULL;
		itr->curr_stmt_pos.page_no = UINT32_MAX;
	}
	if (itr->curr_page != NULL) {
		vy_page_delete(itr->curr_page);
		if (itr->prev_page != NULL)
			vy_page_delete(itr->prev_page);
		itr->curr_page = itr->prev_page = NULL;
	}
}

static int
vy_page_index_decode(uint32_t *page_index, uint32_t count,
		     struct xrow_header *xrow)
{
	assert(xrow->type == VY_RUN_PAGE_INDEX);
	const char *pos = xrow->body->iov_base;
	uint32_t map_size = mp_decode_map(&pos);
	uint32_t map_item;
	uint32_t size = 0;
	for (map_item = 0; map_item < map_size; ++map_item) {
		uint32_t key = mp_decode_uint(&pos);
		switch (key) {
		case VY_PAGE_INDEX_INDEX:
			size = mp_decode_binl(&pos);
			break;
		}
	}
	if (size != sizeof(uint32_t) * count) {
		/* TODO: report filename */
		diag_set(ClientError, ER_INVALID_RUN_FILE,
			 tt_sprintf("Wrong page index size "
				    "(expected %zu, got %u",
				    sizeof(uint32_t) * count, (unsigned)size));
		return -1;
	}
	for (uint32_t i = 0; i < count; ++i) {
		page_index[i] = mp_load_u32(&pos);
	}
	assert(pos == xrow->body->iov_base + xrow->body->iov_len);
	return 0;
}

/**
 * Read a page requests from vinyl xlog data file.
 *
 * @retval 0 on success
 * @retval -1 on error, check diag
 */
int
vy_page_read(struct vy_page *page, const struct vy_page_info *page_info, int fd,
	     ZSTD_DStream *zdctx)
{
	/* read xlog tx from xlog file */
	size_t region_svp = region_used(&fiber()->gc);
	char *data = (char *)region_alloc(&fiber()->gc, page_info->size);
	if (data == NULL) {
		diag_set(OutOfMemory, page_info->size, "region gc", "page");
		return -1;
	}
	ssize_t readen = fio_pread(fd, data, page_info->size,
				   page_info->offset);
	if (readen < 0) {
		/* TODO: report filename */
		diag_set(SystemError, "failed to read from file");
		goto error;
	}
	if (readen != (ssize_t)page_info->size) {
		/* TODO: replace with XlogError, report filename */
		diag_set(ClientError, ER_INVALID_RUN_FILE,
			 "Unexpected end of file");
		goto error;
	}
	ERROR_INJECT(ERRINJ_VY_READ_PAGE_TIMEOUT, {usleep(50000);});

	/* decode xlog tx */
	const char *data_pos = data;
	const char *data_end = data + readen;
	char *rows = page->data;
	char *rows_end = rows + page_info->unpacked_size;
	if (xlog_tx_decode(data, data_end, rows, rows_end, zdctx) != 0)
		goto error;

	struct xrow_header xrow;
	data_pos = page->data + page_info->page_index_offset;
	data_end = page->data + page_info->unpacked_size;
	if (xrow_header_decode(&xrow, &data_pos, data_end) == -1)
		goto error;
	if (xrow.type != VY_RUN_PAGE_INDEX) {
		/* TODO: report filename */
		diag_set(ClientError, ER_INVALID_RUN_FILE,
			 tt_sprintf("Wrong page index type "
				    "(expected %d, got %u)",
				    VY_RUN_PAGE_INDEX, (unsigned)xrow.type));
		goto error;
	}
	if (vy_page_index_decode(page->page_index, page->count, &xrow) != 0)
		goto error;
	region_truncate(&fiber()->gc, region_svp);
	ERROR_INJECT(ERRINJ_VY_READ_PAGE, {
		diag_set(ClientError, ER_INJECTION, "vinyl page read");
		return -1;});
	return 0;
	error:
	region_truncate(&fiber()->gc, region_svp);
	return -1;
}

/**
 * Get thread local zstd decompression context
 */
ZSTD_DStream *
vy_env_get_zdctx(struct vy_run_env *env)
{
	ZSTD_DStream *zdctx = tt_pthread_getspecific(env->zdctx_key);
	if (zdctx == NULL) {
		zdctx = ZSTD_createDStream();
		if (zdctx == NULL) {
			diag_set(OutOfMemory, sizeof(zdctx), "malloc",
				 "zstd context");
			return NULL;
		}
		tt_pthread_setspecific(env->zdctx_key, zdctx);
	}
	return zdctx;
}

/**
 * vinyl read task callback
 */
static int
vy_page_read_cb(struct coio_task *base)
{
	struct vy_page_read_task *task = (struct vy_page_read_task *)base;
	ZSTD_DStream *zdctx = vy_env_get_zdctx(task->run_env);
	if (zdctx == NULL)
		return -1;
	task->rc = vy_page_read(task->page, &task->page_info,
				task->slice->run->fd, zdctx);
	return task->rc;
}

/**
 * vinyl read task cleanup callback
 */
static int
vy_page_read_cb_free(struct coio_task *base)
{
	struct vy_page_read_task *task = (struct vy_page_read_task *)base;
	vy_page_delete(task->page);
	vy_slice_unpin(task->slice);
	coio_task_destroy(&task->base);
	mempool_free(&task->run_env->read_task_pool, task);
	return 0;
}

/**
 * Get a page by the given number the cache or load it from the disk.
 *
 * @retval 0 success
 * @retval -1 critical error
 */
static NODISCARD int
vy_run_iterator_load_page(struct vy_run_iterator *itr, uint32_t page_no,
			  struct vy_page **result)
{
	struct vy_slice *slice = itr->slice;

	/* Check cache */
	*result = vy_run_iterator_cache_get(itr, page_no);
	if (*result != NULL)
		return 0;

	/* Allocate buffers */
	struct vy_page_info *page_info = vy_run_page_info(slice->run, page_no);
	struct vy_page *page = vy_page_new(page_info);
	if (page == NULL)
		return -1;

	/* Read page data from the disk */
	int rc;
	if (itr->coio_read) {
		/*
		 * Use coeio for TX thread **after recovery**.
		 */

		/* Allocate a coio task */
		struct vy_page_read_task *task =
			(struct vy_page_read_task *)mempool_alloc(&itr->run_env->read_task_pool);
		if (task == NULL) {
			diag_set(OutOfMemory, sizeof(*task), "malloc",
				 "vy_page_read_task");
			vy_page_delete(page);
			return -1;
		}
		coio_task_create(&task->base, vy_page_read_cb,
				 vy_page_read_cb_free);

		/*
		 * Make sure the run file descriptor won't be closed
		 * (even worse, reopened) while a coeio thread is
		 * reading it.
		 */
		vy_slice_pin(slice);

		task->slice = slice;
		task->page_info = *page_info;
		task->run_env = itr->run_env;
		task->page = page;

		/* Post task to coeio */
		rc = coio_task_post(&task->base, TIMEOUT_INFINITY);
		if (rc < 0)
			return -1; /* timed out or cancelled */

		if (task->rc != 0) {
			/* posted, but failed */
			diag_move(&task->base.diag, &fiber()->diag);
			vy_page_read_cb_free(&task->base);
			return -1;
		}

		coio_task_destroy(&task->base);
		mempool_free(&task->run_env->read_task_pool, task);

		vy_slice_unpin(slice);
	} else {
		/*
		 * Optimization: use blocked I/O for non-TX threads or
		 * during WAL recovery (env->status != VINYL_ONLINE).
		 */
		ZSTD_DStream *zdctx = vy_env_get_zdctx(itr->run_env);
		if (zdctx == NULL) {
			vy_page_delete(page);
			return -1;
		}
		if (vy_page_read(page, page_info, slice->run->fd, zdctx) != 0) {
			vy_page_delete(page);
			return -1;
		}
	}

	/* Iterator is never used from multiple fibers */
	assert(vy_run_iterator_cache_get(itr, page_no) == NULL);

	/* Update cache */
	vy_run_iterator_cache_put(itr, page, page_no);

	*result = page;
	return 0;
}

/**
 * Read key and lsn by a given wide position.
 * For the first record in a page reads the result from the page
 * index instead of fetching it from disk.
 *
 * @retval 0 success
 * @retval -1 read error or out of memory.
 */
static NODISCARD int
vy_run_iterator_read(struct vy_run_iterator *itr,
		     struct vy_run_iterator_pos pos,
		     struct tuple **stmt)
{
	struct vy_page *page;
	int rc = vy_run_iterator_load_page(itr, pos.page_no, &page);
	if (rc != 0)
		return rc;
	*stmt = vy_page_stmt(page, pos.pos_in_page, itr->key_def,
			     itr->format, itr->upsert_format, itr->is_primary);
	if (*stmt == NULL)
		return -1;
	return 0;
}

/**
 * Binary search in page index
 * In terms of STL, makes lower_bound for EQ,GE,LT and upper_bound for GT,LE
 * Additionally *equal_key argument is set to true if the found value is
 * equal to given key (untouched otherwise)
 * @retval page number
 */
static uint32_t
vy_run_iterator_search_page(struct vy_run_iterator *itr,
			    enum iterator_type iterator_type,
			    const struct tuple *key, bool *equal_key)
{
	struct vy_run *run = itr->slice->run;
	uint32_t beg = 0;
	uint32_t end = run->info.count;
	/* for upper bound we change zero comparison result to -1 */
	int zero_cmp = (iterator_type == ITER_GT ||
			iterator_type == ITER_LE ? -1 : 0);
	while (beg != end) {
		uint32_t mid = beg + (end - beg) / 2;
		struct vy_page_info *page_info;
		page_info = vy_run_page_info(run, mid);
		int cmp;
		cmp = -vy_stmt_compare_with_raw_key(key, page_info->min_key,
						    itr->key_def);
		cmp = cmp ? cmp : zero_cmp;
		*equal_key = *equal_key || cmp == 0;
		if (cmp < 0)
			beg = mid + 1;
		else
			end = mid;
	}
	return end;
}

/**
 * Binary search in page
 * In terms of STL, makes lower_bound for EQ,GE,LT and upper_bound for GT,LE
 * Additionally *equal_key argument is set to true if the found value is
 * equal to given key (untouched otherwise)
 * @retval position in the page
 */
static uint32_t
vy_run_iterator_search_in_page(struct vy_run_iterator *itr,
			       enum iterator_type iterator_type,
			       const struct tuple *key,
			       struct vy_page *page, bool *equal_key)
{
	uint32_t beg = 0;
	uint32_t end = page->count;
	/* for upper bound we change zero comparison result to -1 */
	int zero_cmp = (iterator_type == ITER_GT ||
			iterator_type == ITER_LE ? -1 : 0);
	while (beg != end) {
		uint32_t mid = beg + (end - beg) / 2;
		struct tuple *fnd_key = vy_page_stmt(page, mid, itr->key_def,
						     itr->format, itr->upsert_format,
						     itr->is_primary);
		if (fnd_key == NULL)
			return end;
		int cmp = vy_stmt_compare(fnd_key, key, itr->key_def);
		cmp = cmp ? cmp : zero_cmp;
		*equal_key = *equal_key || cmp == 0;
		if (cmp < 0)
			beg = mid + 1;
		else
			end = mid;
		tuple_unref(fnd_key);
	}
	return end;
}

/**
 * Binary search in a run for the given key.
 * In terms of STL, makes lower_bound for EQ,GE,LT and upper_bound for GT,LE
 * Resulting wide position is stored it *pos argument
 * Additionally *equal_key argument is set to true if the found value is
 * equal to given key (untouched otherwise)
 *
 * @retval 0 success
 * @retval -1 read or memory error
 */
static NODISCARD int
vy_run_iterator_search(struct vy_run_iterator *itr,
		       enum iterator_type iterator_type,
		       const struct tuple *key,
		       struct vy_run_iterator_pos *pos, bool *equal_key)
{
	pos->page_no = vy_run_iterator_search_page(itr, iterator_type, key,
						   equal_key);
	if (pos->page_no == 0) {
		pos->pos_in_page = 0;
		return 0;
	}
	pos->page_no--;
	struct vy_page *page;
	int rc = vy_run_iterator_load_page(itr, pos->page_no, &page);
	if (rc != 0)
		return rc;
	bool equal_in_page = false;
	pos->pos_in_page = vy_run_iterator_search_in_page(itr, iterator_type,
							  key, page,
							  &equal_in_page);
	if (pos->pos_in_page == page->count) {
		pos->page_no++;
		pos->pos_in_page = 0;
	} else {
		*equal_key = equal_in_page;
	}
	return 0;
}

/**
 * Increment (or decrement, depending on the order) the current
 * wide position.
 * @retval 0 success, set *pos to new value
 * @retval 1 EOF
 * Affects: curr_loaded_page
 */
static NODISCARD int
vy_run_iterator_next_pos(struct vy_run_iterator *itr,
			 enum iterator_type iterator_type,
			 struct vy_run_iterator_pos *pos)
{
	struct vy_run *run = itr->slice->run;
	itr->stat->step_count++;
	*pos = itr->curr_pos;
	assert(pos->page_no < run->info.count);
	if (iterator_type == ITER_LE || iterator_type == ITER_LT) {
		if (pos->pos_in_page > 0) {
			pos->pos_in_page--;
		} else {
			if (pos->page_no == 0)
				return 1;
			pos->page_no--;
			struct vy_page_info *page_info =
				vy_run_page_info(run, pos->page_no);
			assert(page_info->count > 0);
			pos->pos_in_page = page_info->count - 1;
		}
	} else {
		assert(iterator_type == ITER_GE || iterator_type == ITER_GT ||
		       iterator_type == ITER_EQ);
		struct vy_page_info *page_info =
			vy_run_page_info(run, pos->page_no);
		assert(page_info->count > 0);
		pos->pos_in_page++;
		if (pos->pos_in_page >= page_info->count) {
			pos->page_no++;
			pos->pos_in_page = 0;
			if (pos->page_no == run->info.count)
				return 1;
		}
	}
	return 0;
}

static NODISCARD int
vy_run_iterator_get(struct vy_run_iterator *itr, struct tuple **result);

/**
 * Find the next record with lsn <= itr->lsn record.
 * The current position must be at the beginning of a series of
 * records with the same key it terms of direction of iterator
 * (i.e. left for GE, right for LE).
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 read or memory error
 * Affects: curr_loaded_page, curr_pos, search_ended
 */
static NODISCARD int
vy_run_iterator_find_lsn(struct vy_run_iterator *itr,
			 enum iterator_type iterator_type,
			 const struct tuple *key, struct tuple **ret)
{
	struct vy_slice *slice = itr->slice;
	assert(itr->curr_pos.page_no < slice->run->info.count);
	struct tuple *stmt;
	const struct key_def *key_def = itr->key_def;
	*ret = NULL;
	int rc = vy_run_iterator_read(itr, itr->curr_pos, &stmt);
	if (rc != 0)
		return rc;
	while (vy_stmt_lsn(stmt) > (**itr->read_view).vlsn) {
		tuple_unref(stmt);
		stmt = NULL;
		rc = vy_run_iterator_next_pos(itr, iterator_type,
					      &itr->curr_pos);
		if (rc > 0) {
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			return 0;
		}
		assert(rc == 0);
		rc = vy_run_iterator_read(itr, itr->curr_pos, &stmt);
		if (rc != 0)
			return rc;
		if (iterator_type == ITER_EQ &&
		    vy_stmt_compare(stmt, key, key_def)) {
			tuple_unref(stmt);
			stmt = NULL;
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			return 0;
		}
	}
	if (iterator_type == ITER_LE || iterator_type == ITER_LT) {
		/* Remember the page_no of stmt */
		uint32_t cur_key_page_no = itr->curr_pos.page_no;

		struct vy_run_iterator_pos test_pos;
		rc = vy_run_iterator_next_pos(itr, iterator_type, &test_pos);
		while (rc == 0) {
			/*
			 * The cache is at least two pages. Ensure that
			 * subsequent read keeps the stmt in the cache
			 * by moving its page to the start of LRU list.
			 */
			vy_run_iterator_cache_touch(itr, cur_key_page_no);

			struct tuple *test_stmt;
			rc = vy_run_iterator_read(itr, test_pos, &test_stmt);
			if (rc != 0)
				return rc;
			if (vy_stmt_lsn(test_stmt) > (**itr->read_view).vlsn ||
			    vy_tuple_compare(stmt, test_stmt, key_def) != 0) {
				tuple_unref(test_stmt);
				test_stmt = NULL;
				break;
			}
			tuple_unref(test_stmt);
			test_stmt = NULL;
			itr->curr_pos = test_pos;

			/* See above */
			vy_run_iterator_cache_touch(itr, cur_key_page_no);

			rc = vy_run_iterator_next_pos(itr, iterator_type,
						      &test_pos);
		}

		rc = rc > 0 ? 0 : rc;
	}
	tuple_unref(stmt);
	if (!rc) /* If next_pos() found something then get it. */
		rc = vy_run_iterator_get(itr, ret);
	if (rc != 0 || *ret == NULL)
		return rc;
	/* Check if the result is within the slice boundaries. */
	if (iterator_type == ITER_LE || iterator_type == ITER_LT) {
		if (slice->begin != NULL &&
		    vy_stmt_compare_with_key(*ret, slice->begin, key_def) < 0) {
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			*ret = NULL;
			return 0;
		}
	} else {
		assert(iterator_type == ITER_GE || iterator_type == ITER_GT ||
		       iterator_type == ITER_EQ);
		if (slice->end != NULL &&
		    vy_stmt_compare_with_key(*ret, slice->end, key_def) >= 0) {
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			*ret = NULL;
			return 0;
		}
	}
	return 0;
}

/*
 * FIXME: vy_run_iterator_next_key() calls vy_run_iterator_start() which
 * recursivly calls vy_run_iterator_next_key().
 */
static NODISCARD int
vy_run_iterator_next_key(struct vy_stmt_iterator *vitr, struct tuple **ret,
			 bool *stop);
/**
 * Start iteration for a given key and direction.
 * Note, this function doesn't check slice boundaries.
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 read or memory error
 * Affects: curr_loaded_page, curr_pos, search_ended
 */
static NODISCARD int
vy_run_iterator_start_from(struct vy_run_iterator *itr,
			   enum iterator_type iterator_type,
			   const struct tuple *key, struct tuple **ret)
{
	struct vy_run *run = itr->slice->run;

	assert(!itr->search_started);
	itr->search_started = true;
	*ret = NULL;

	const struct key_def *user_key_def = itr->user_key_def;
	if (run->info.has_bloom && iterator_type == ITER_EQ &&
	    tuple_field_count(key) >= user_key_def->part_count) {
		uint32_t hash;
		if (vy_stmt_type(key) == IPROTO_SELECT) {
			const char *data = tuple_data(key);
			mp_decode_array(&data);
			hash = key_hash(data, user_key_def);
		} else {
			hash = tuple_hash(key, user_key_def);
		}
		if (!bloom_possible_has(&run->info.bloom, hash)) {
			itr->search_ended = true;
			itr->stat->bloom_reflections++;
			return 0;
		}
	}

	itr->stat->lookup_count++;

	if (run->info.count == 1) {
		/* there can be a stupid bootstrap run in which it's EOF */
		struct vy_page_info *page_info = run->info.page_infos;

		if (!page_info->count) {
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			return 0;
		}
		struct vy_page *page;
		int rc = vy_run_iterator_load_page(itr, 0, &page);
		if (rc != 0)
			return rc;
	} else if (run->info.count == 0) {
		vy_run_iterator_cache_clean(itr);
		itr->search_ended = true;
		return 0;
	}

	struct vy_run_iterator_pos end_pos = {run->info.count, 0};
	bool equal_found = false;
	int rc;
	if (tuple_field_count(key) > 0) {
		rc = vy_run_iterator_search(itr, iterator_type, key,
					    &itr->curr_pos, &equal_found);
		if (rc != 0)
			return rc;
	} else if (iterator_type == ITER_LE) {
		itr->curr_pos = end_pos;
	} else {
		assert(iterator_type == ITER_GE);
		itr->curr_pos.page_no = 0;
		itr->curr_pos.pos_in_page = 0;
	}
	if (iterator_type == ITER_EQ && !equal_found) {
		vy_run_iterator_cache_clean(itr);
		itr->search_ended = true;
		return 0;
	}
	if ((iterator_type == ITER_GE || iterator_type == ITER_GT) &&
	    itr->curr_pos.page_no == end_pos.page_no) {
		vy_run_iterator_cache_clean(itr);
		itr->search_ended = true;
		return 0;
	}
	if (iterator_type == ITER_LT || iterator_type == ITER_LE) {
		/**
		 * 1) in case of ITER_LT we now positioned on the value >= than
		 * given, so we need to make a step on previous key
		 * 2) in case if ITER_LE we now positioned on the value > than
		 * given (special branch of code in vy_run_iterator_search),
		 * so we need to make a step on previous key
		 */
		return vy_run_iterator_next_key(&itr->base, ret, NULL);
	} else {
		assert(iterator_type == ITER_GE || iterator_type == ITER_GT ||
		       iterator_type == ITER_EQ);
		/**
		 * 1) in case of ITER_GT we now positioned on the value > than
		 * given (special branch of code in vy_run_iterator_search),
		 * so we need just to find proper lsn
		 * 2) in case if ITER_GE or ITER_EQ we now positioned on the
		 * value >= given, so we need just to find proper lsn
		 */
		return vy_run_iterator_find_lsn(itr, iterator_type, key, ret);
	}
}

/**
 * Start iteration in a run taking into account slice boundaries.
 * This function is a wrapper around vy_run_iterator_start_from()
 * which passes a contrived search key and the iterator
 * direction to make sure the result falls in the given slice.
 */
static NODISCARD int
vy_run_iterator_start(struct vy_run_iterator *itr, struct tuple **ret)
{
	enum iterator_type iterator_type = itr->iterator_type;
	const struct tuple *key = itr->key;
	const struct key_def *key_def = itr->key_def;
	struct vy_slice *slice = itr->slice;
	int cmp;

	if (slice->begin != NULL &&
	    (iterator_type == ITER_GT || iterator_type == ITER_GE ||
	     iterator_type == ITER_EQ)) {
		/*
		 *    original   |     start
		 * --------------+-------+-----+
		 *   KEY   | DIR |  KEY  | DIR |
		 * --------+-----+-------+-----+
		 * > begin | *   | key   | *   |
		 * = begin | gt  | key   | gt  |
		 *         | ge  | begin | ge  |
		 *         | eq  | begin | ge  |
		 * < begin | gt  | begin | ge  |
		 *         | ge  | begin | ge  |
		 *         | eq  |    stop     |
		 */
		cmp = vy_stmt_compare_with_key(key, slice->begin, key_def);
		if (cmp < 0 && iterator_type == ITER_EQ) {
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			return 0;
		}
		if (cmp < 0 || (cmp == 0 && iterator_type != ITER_GT)) {
			iterator_type = ITER_GE;
			key = slice->begin;
		}
	}

	if (slice->end != NULL &&
	    (iterator_type == ITER_LT || iterator_type == ITER_LE)) {
		/*
		 *    original   |     start
		 * --------------+-------+-----+
		 *   KEY   | DIR |  KEY  | DIR |
		 * --------+-----+-------+-----+
		 * < end   | *   | key   | *   |
		 * = end   | lt  | key   | lt  |
		 *         | le  | end   | lt  |
		 * > end   | lt  | end   | lt  |
		 *         | le  | end   | lt  |
		 */
		cmp = vy_stmt_compare_with_key(key, slice->end, key_def);
		if (cmp > 0 || (cmp == 0 && iterator_type != ITER_LT)) {
			iterator_type = ITER_LT;
			key = slice->end;
		}
	}

	return vy_run_iterator_start_from(itr, iterator_type, key, ret);
}

/* }}} vy_run_iterator vy_run_iterator support functions */

/* {{{ vy_run_iterator API implementation */

/** Vtable for vy_stmt_iterator - declared below */
static struct vy_stmt_iterator_iface vy_run_iterator_iface;

/**
 * Open the iterator.
 */
void
vy_run_iterator_open(struct vy_run_iterator *itr, bool coio_read,
		     struct vy_iterator_stat *stat, struct vy_run_env *run_env,
		     struct vy_slice *slice, enum iterator_type iterator_type,
		     const struct tuple *key, const struct vy_read_view **rv,
		     const struct key_def *key_def,
		     const struct key_def *user_key_def,
		     struct tuple_format *format,
		     struct tuple_format *upsert_format,
		     bool is_primary)
{
	itr->base.iface = &vy_run_iterator_iface;
	itr->stat = stat;
	itr->key_def = key_def;
	itr->user_key_def = user_key_def;
	itr->format = format;
	itr->upsert_format = upsert_format;
	itr->is_primary = is_primary;
	itr->run_env = run_env;
	itr->slice = slice;
	itr->coio_read = coio_read;

	itr->iterator_type = iterator_type;
	itr->key = key;
	itr->read_view = rv;
	if (tuple_field_count(key) == 0) {
		/* NULL key. change itr->iterator_type for simplification */
		itr->iterator_type = iterator_type == ITER_LT ||
				     iterator_type == ITER_LE ?
				     ITER_LE : ITER_GE;
	}

	itr->curr_stmt = NULL;
	itr->curr_pos.page_no = slice->run->info.count;
	itr->curr_stmt_pos.page_no = UINT32_MAX;
	itr->curr_page = NULL;
	itr->prev_page = NULL;

	itr->search_started = false;
	itr->search_ended = false;
}

/**
 * Create a stmt object from a its impression on a run page.
 * Uses the current iterator position in the page.
 *
 * @retval 0 success or EOF (*result == NULL)
 * @retval -1 memory or read error
 */
static NODISCARD int
vy_run_iterator_get(struct vy_run_iterator *itr, struct tuple **result)
{
	assert(itr->search_started);
	*result = NULL;
	if (itr->search_ended)
		return 0;
	if (itr->curr_stmt != NULL) {
		if (itr->curr_stmt_pos.page_no == itr->curr_pos.page_no &&
		    itr->curr_stmt_pos.pos_in_page == itr->curr_pos.pos_in_page) {
			*result = itr->curr_stmt;
			return 0;
		}
		tuple_unref(itr->curr_stmt);
		itr->curr_stmt = NULL;
		itr->curr_stmt_pos.page_no = UINT32_MAX;
	}
	int rc = vy_run_iterator_read(itr, itr->curr_pos, result);
	if (rc == 0) {
		itr->curr_stmt_pos = itr->curr_pos;
		itr->curr_stmt = *result;
	}
	return rc;
}

/**
 * Find the next stmt in a page, i.e. a stmt with a different key
 * and fresh enough LSN (i.e. skipping the keys
 * too old for the current transaction).
 *
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 memory or read error
 */
static NODISCARD int
vy_run_iterator_next_key(struct vy_stmt_iterator *vitr, struct tuple **ret,
			 bool *stop)
{
	(void)stop;
	assert(vitr->iface->next_key == vy_run_iterator_next_key);
	struct vy_run_iterator *itr = (struct vy_run_iterator *) vitr;
	*ret = NULL;
	int rc;

	if (itr->search_ended)
		return 0;
	if (!itr->search_started)
		return vy_run_iterator_start(itr, ret);
	uint32_t end_page = itr->slice->run->info.count;
	assert(itr->curr_pos.page_no <= end_page);
	const struct key_def *key_def = itr->key_def;
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT) {
		if (itr->curr_pos.page_no == 0 &&
		    itr->curr_pos.pos_in_page == 0) {
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			return 0;
		}
		if (itr->curr_pos.page_no == end_page) {
			/* A special case for reverse iterators */
			uint32_t page_no = end_page - 1;
			struct vy_page *page;
			int rc = vy_run_iterator_load_page(itr, page_no, &page);
			if (rc != 0)
				return rc;
			if (page->count == 0) {
				vy_run_iterator_cache_clean(itr);
				itr->search_ended = true;
				return 0;
			}
			itr->curr_pos.page_no = page_no;
			itr->curr_pos.pos_in_page = page->count - 1;
			return vy_run_iterator_find_lsn(itr, itr->iterator_type,
							itr->key, ret);
		}
	}
	assert(itr->curr_pos.page_no < end_page);

	struct tuple *cur_key;
	rc = vy_run_iterator_read(itr, itr->curr_pos, &cur_key);
	if (rc != 0)
		return rc;
	uint32_t cur_key_page_no = itr->curr_pos.page_no;

	struct tuple *next_key = NULL;
	do {
		if (next_key != NULL)
			tuple_unref(next_key);
		next_key = NULL;
		int rc = vy_run_iterator_next_pos(itr, itr->iterator_type,
						  &itr->curr_pos);
		if (rc > 0) {
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			tuple_unref(cur_key);
			cur_key = NULL;
			return 0;
		}

		/*
		 * The cache is at least two pages. Ensure that
		 * subsequent read keeps the cur_key in the cache
		 * by moving its page to the start of LRU list.
		 */
		vy_run_iterator_cache_touch(itr, cur_key_page_no);

		rc = vy_run_iterator_read(itr, itr->curr_pos, &next_key);
		if (rc != 0) {
			tuple_unref(cur_key);
			cur_key = NULL;
			return rc;
		}

		/* See above */
		vy_run_iterator_cache_touch(itr, cur_key_page_no);
	} while (vy_tuple_compare(cur_key, next_key, key_def) == 0);
	tuple_unref(cur_key);
	cur_key = NULL;
	if (itr->iterator_type == ITER_EQ &&
	    vy_stmt_compare(next_key, itr->key, key_def) != 0) {
		vy_run_iterator_cache_clean(itr);
		itr->search_ended = true;
		tuple_unref(next_key);
		next_key = NULL;
		return 0;
	}
	tuple_unref(next_key);
	next_key = NULL;
	return vy_run_iterator_find_lsn(itr, itr->iterator_type, itr->key, ret);
}

/**
 * Find next (lower, older) record with the same key as current
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 memory or read error
 */
static NODISCARD int
vy_run_iterator_next_lsn(struct vy_stmt_iterator *vitr, struct tuple **ret)
{
	assert(vitr->iface->next_lsn == vy_run_iterator_next_lsn);
	struct vy_run_iterator *itr = (struct vy_run_iterator *) vitr;
	*ret = NULL;
	int rc;

	if (itr->search_ended)
		return 0;
	if (!itr->search_started)
		return vy_run_iterator_start(itr, ret);
	assert(itr->curr_pos.page_no < itr->slice->run->info.count);

	struct vy_run_iterator_pos next_pos;
	rc = vy_run_iterator_next_pos(itr, ITER_GE, &next_pos);
	if (rc > 0)
		return 0;

	struct tuple *cur_key;
	rc = vy_run_iterator_read(itr, itr->curr_pos, &cur_key);
	if (rc != 0)
		return rc;

	struct tuple *next_key;
	rc = vy_run_iterator_read(itr, next_pos, &next_key);
	if (rc != 0) {
		tuple_unref(cur_key);
		return rc;
	}

	/**
	 * One can think that we had to lock page of itr->curr_pos,
	 *  to prevent freeing cur_key with entire page and avoid
	 *  segmentation fault in vy_stmt_compare_raw.
	 * But in fact the only case when curr_pos and next_pos
	 *  point to different pages is the case when next_pos points
	 *  to the beginning of the next page, and in this case
	 *  vy_run_iterator_read will read data from page index, not the page.
	 *  So in the case no page will be unloaded and we don't need
	 *  page lock
	 */
	int cmp = vy_tuple_compare(cur_key, next_key, itr->key_def);
	tuple_unref(cur_key);
	cur_key = NULL;
	tuple_unref(next_key);
	next_key = NULL;
	itr->curr_pos = cmp == 0 ? next_pos : itr->curr_pos;
	rc = cmp != 0;
	if (rc != 0)
		return 0;
	return vy_run_iterator_get(itr, ret);
}

/**
 * Restore the current position (if necessary) after a change in the set of
 * runs or ranges and check if the position was changed.
 * @sa struct vy_stmt_iterator comments.
 *
 * @pre the iterator is not started
 *
 * @param last_stmt the last key on which the iterator was
 *		      positioned
 *
 * @retval 0	if position did not change (iterator started)
 * @retval 1	if position changed
 * @retval -1	a read or memory error
 */
static NODISCARD int
vy_run_iterator_restore(struct vy_stmt_iterator *vitr,
			const struct tuple *last_stmt, struct tuple **ret,

			bool *stop)
{
	(void)stop;
	assert(vitr->iface->restore == vy_run_iterator_restore);
	struct vy_run_iterator *itr = (struct vy_run_iterator *) vitr;
	*ret = NULL;
	int rc;

	if (itr->search_started || last_stmt == NULL) {
		if (!itr->search_started) {
			rc = vy_run_iterator_start(itr, ret);
		} else {
			rc = vy_run_iterator_get(itr, ret);
		}
		if (rc < 0)
			return rc;
		return 0;
	}
	/* Restoration is very similar to first search so we'll use that */
	enum iterator_type iterator_type = itr->iterator_type;
	if (iterator_type == ITER_GT || iterator_type == ITER_EQ)
		iterator_type = ITER_GE;
	else if (iterator_type == ITER_LT)
		iterator_type = ITER_LE;
	struct tuple *next;
	rc = vy_run_iterator_start_from(itr, iterator_type, last_stmt, &next);
	if (rc != 0)
		return rc;
	else if (next == NULL)
		return 0;
	const struct key_def *def = itr->key_def;
	bool position_changed = true;
	if (vy_stmt_compare(next, last_stmt, def) == 0) {
		position_changed = false;
		if (vy_stmt_lsn(next) >= vy_stmt_lsn(last_stmt)) {
			/* skip the same stmt to next stmt or older version */
			do {
				rc = vy_run_iterator_next_lsn(vitr, &next);
				if (rc != 0)
					return rc;
				if (next == NULL) {
					rc = vy_run_iterator_next_key(vitr,
								      &next,
								      NULL);
					if (rc != 0)
						return rc;
					break;
				}
			} while (vy_stmt_lsn(next) >= vy_stmt_lsn(last_stmt));
			if (next != NULL)
				position_changed = true;
		}
	} else if (itr->iterator_type == ITER_EQ &&
		   vy_stmt_compare(itr->key, next, def) != 0) {

		itr->search_ended = true;
		vy_run_iterator_cache_clean(itr);
		return position_changed;
	}
	*ret = next;
	return position_changed;
}

/**
 * Free all allocated resources in a worker thread.
 */
static void
vy_run_iterator_cleanup(struct vy_stmt_iterator *vitr)
{
	assert(vitr->iface->cleanup == vy_run_iterator_cleanup);
	vy_run_iterator_cache_clean((struct vy_run_iterator *) vitr);
}

/**
 * Close the iterator and free resources.
 * Can be called only after cleanup().
 */
static void
vy_run_iterator_close(struct vy_stmt_iterator *vitr)
{
	assert(vitr->iface->close == vy_run_iterator_close);
	struct vy_run_iterator *itr = (struct vy_run_iterator *) vitr;
	/* cleanup() must be called before */
	assert(itr->curr_stmt == NULL && itr->curr_page == NULL);
	TRASH(itr);
	(void) itr;
}

static struct vy_stmt_iterator_iface vy_run_iterator_iface = {
	.next_key = vy_run_iterator_next_key,
	.next_lsn = vy_run_iterator_next_lsn,
	.restore = vy_run_iterator_restore,
	.cleanup = vy_run_iterator_cleanup,
	.close = vy_run_iterator_close,
};

/* }}} vy_run_iterator API implementation */

/**
 * Load run from disk
 * @param run - run to laod
 * @param index_path - path to index part of the run
 * @param run_path - path to run part of the run
 * @return - 0 on sucess, -1 on fail
 */
int
vy_run_recover(struct vy_run *run, const char *index_path,
	       const char *run_path)
{
	struct xlog_cursor cursor;
	if (xlog_cursor_open(&cursor, index_path))
		goto fail;

	struct xlog_meta *meta = &cursor.meta;
	if (strcmp(meta->filetype, XLOG_META_TYPE_INDEX) != 0) {
		diag_set(ClientError, ER_INVALID_XLOG_TYPE,
			 XLOG_META_TYPE_INDEX, meta->filetype);
		goto fail_close;
	}

	/* Read run header. */
	struct xrow_header xrow;
	/* all rows should be in one tx */
	int rc = xlog_cursor_next_tx(&cursor);
	if (rc != 0) {
		if (rc > 0)
			diag_set(ClientError, ER_INVALID_INDEX_FILE,
				 index_path, "Unexpected end of file");
		goto fail_close;
	}
	rc = xlog_cursor_next_row(&cursor, &xrow);
	if (rc != 0) {
		if (rc > 0)
			diag_set(ClientError, ER_INVALID_INDEX_FILE,
				 index_path, "Unexpected end of file");
		goto fail_close;
	}

	if (xrow.type != VY_INDEX_RUN_INFO) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, index_path,
			 tt_sprintf("Wrong xrow type (expected %d, got %u)",
				    VY_INDEX_RUN_INFO, (unsigned)xrow.type));
		return -1;
	}
	if (vy_run_info_decode(&run->info, &xrow, index_path) != 0)
		goto fail_close;

	/* Allocate buffer for page info. */
	run->info.page_infos = calloc(run->info.count,
				      sizeof(struct vy_page_info));
	if (run->info.page_infos == NULL) {
		diag_set(OutOfMemory,
			 run->info.count * sizeof(struct vy_page_info),
			 "malloc", "struct vy_page_info");
		goto fail_close;
	}

	for (uint32_t page_no = 0; page_no < run->info.count; page_no++) {
		int rc = xlog_cursor_next_row(&cursor, &xrow);
		if (rc != 0) {
			if (rc > 0) {
				/** To few pages in file */
				diag_set(ClientError, ER_INVALID_INDEX_FILE,
					 index_path, "Unexpected end of file");
			}
			/*
			 * Limit the count of pages to
			 * successfully created pages.
			 */
			run->info.count = page_no;
			goto fail_close;
		}
		if (xrow.type != VY_INDEX_PAGE_INFO) {
			diag_set(ClientError, ER_INVALID_INDEX_FILE,
				 tt_sprintf("Wrong xrow type "
					    "(expected %d, got %u)",
					    VY_INDEX_PAGE_INFO,
					    (unsigned)xrow.type));
			goto fail_close;
		}
		struct vy_page_info *page = run->info.page_infos + page_no;
		if (vy_page_info_decode(page, &xrow, index_path) < 0) {
			/**
			 * Limit the count of pages to successfully
			 * created pages
			 */
			run->info.count = page_no;
			goto fail_close;
		}
		run->info.size += page->size;
		run->info.keys += page->count;
	}

	/* We don't need to keep metadata file open any longer. */
	xlog_cursor_close(&cursor, false);

	/* Prepare data file for reading. */
	if (xlog_cursor_open(&cursor, run_path))
		goto fail;
	meta = &cursor.meta;
	if (strcmp(meta->filetype, XLOG_META_TYPE_RUN) != 0) {
		diag_set(ClientError, ER_INVALID_XLOG_TYPE,
			 XLOG_META_TYPE_RUN, meta->filetype);
		goto fail_close;
	}
	run->fd = cursor.fd;
	xlog_cursor_close(&cursor, true);
	return 0;

	fail_close:
	xlog_cursor_close(&cursor, false);
	fail:
	return -1;
}

/**
 * Read a page with stream->page_no from the run and save it in stream->page.
 * Support function of slice stream.
 * @param stream - the stream.
 * @return 0 on success, -1 of memory or read error (diag is set).
 */
static NODISCARD int
vy_slice_stream_read_page(struct vy_slice_stream *stream)
{
	assert(stream->page == NULL);
	ZSTD_DStream *zdctx = vy_env_get_zdctx(stream->run_env);
	if (zdctx == NULL)
		return -1;

	struct vy_page_info *page_info = vy_run_page_info(stream->slice->run,
							  stream->page_no);
	stream->page = vy_page_new(page_info);
	if (stream->page == NULL)
		return -1;

	if (vy_page_read(stream->page, page_info,
			 stream->slice->run->fd, zdctx) != 0) {
		vy_page_delete(stream->page);
		stream->page = NULL;
		return -1;
	}
	return 0;
}

/**
 * Binary search in a run for the given key. Find the first position with
 * a tuple greater or equal to slice
 * @retval 0 success
 * @retval -1 read or memory error
 */
static NODISCARD int
vy_slice_stream_search(struct vy_slice_stream *stream)
{
	assert(stream->page == NULL);
	if (stream->slice->begin == NULL) {
		/* Already at the beginning */
		assert(stream->page_no == 0);
		assert(stream->pos_in_page == 0);
		return 0;
	}

	if (vy_slice_stream_read_page(stream) != 0)
		return -1;

	/**
	 * Binary search in page. Find the first position in page with
	 * tuple >= stream->slice->begin.
	 */
	uint32_t beg = 0;
	uint32_t end = stream->page->count;
	while (beg != end) {
		uint32_t mid = beg + (end - beg) / 2;
		struct tuple *fnd_key =
			vy_page_stmt(stream->page, mid, stream->key_def,
				     stream->format, stream->upsert_format,
				     stream->is_primary);
		if (fnd_key == NULL)
			return -1;
		int cmp = vy_stmt_compare(fnd_key, stream->slice->begin,
					  stream->key_def);
		if (cmp < 0)
			beg = mid + 1;
		else
			end = mid;
		tuple_unref(fnd_key);
	}
	stream->pos_in_page = end;

	if (stream->pos_in_page == stream->page->count) {
		/* The first tuple is in the beginning of the next page */
		vy_page_delete(stream->page);
		stream->page = NULL;
		stream->page_no++;
		stream->pos_in_page = 0;
	}
	return 0;
}

/**
 * Get the value from the stream and move to the next position.
 * Set *ret to the value or NULL if EOF.
 * @param virt_stream - virtual stream.
 * @param ret - pointer to pointer to the result.
 * @return 0 on success, -1 on memory or read error.
 */
static NODISCARD int
vy_slice_stream_next(struct vy_stmt_stream *virt_stream, struct tuple **ret)
{
	assert(virt_stream->iface->next == vy_slice_stream_next);
	struct vy_slice_stream *stream = (struct vy_slice_stream *)virt_stream;
	*ret = NULL;

	/* If it's the first call, run binary search on slice->begin */
	if (stream->tuple == NULL && vy_slice_stream_search(stream) != 0)
		return -1;

	/* If the slice is ended, return EOF */
	if (stream->page_no >= stream->slice->run->info.count)
		return 0;

	/* If current page is not already read, read it */
	if (stream->page == NULL && vy_slice_stream_read_page(stream) != 0)
		return -1;

	/* Read current tuple from the page */
	struct tuple *tuple =
		vy_page_stmt(stream->page, stream->pos_in_page,
			     stream->key_def, stream->format,
			     stream->upsert_format, stream->is_primary);
	if (tuple == NULL) /* Read or memory error */
		return -1;

	/* Check that the tuple is not out of slice bounds = */
	if (stream->slice->end != NULL &&
	    stream->page_no == stream->slice->last_page_no &&
	    vy_stmt_compare_with_key(tuple, stream->slice->end,
				     stream->key_def) >= 0)
		return 0;

	/* We definitely has the next non-null tuple. Save it in stream */
	if (stream->tuple != NULL)
		tuple_unref(stream->tuple);
	stream->tuple = tuple;
	*ret = tuple;

	/* Increment position */
	stream->pos_in_page++;

	/* Check whether the position is out of page */
	struct vy_page_info *page_info = vy_run_page_info(stream->slice->run,
							  stream->page_no);
	if (stream->pos_in_page >= page_info->count) {
		/**
		 * Out of page. Free page, move the position to the next page
		 * and * nullify page pointer to read it on the next iteration.
		 */
		vy_page_delete(stream->page);
		stream->page = NULL;
		stream->page_no++;
		stream->pos_in_page = 0;
	}

	return 0;
}

/**
 * Free resources.
 */
static void
vy_slice_stream_close(struct vy_stmt_stream *virt_stream)
{
	assert(virt_stream->iface->close == vy_slice_stream_close);
	struct vy_slice_stream *stream = (struct vy_slice_stream *)virt_stream;
	if (stream->page != NULL) {
		vy_page_delete(stream->page);
		stream->page = NULL;
	}
	if (stream->tuple != NULL) {
		tuple_unref(stream->tuple);
		stream->tuple = NULL;
	}
}

static const struct vy_stmt_stream_iface vy_slice_stream_iface = {
	.next = vy_slice_stream_next,
	.close = vy_slice_stream_close
};

void
vy_slice_stream_open(struct vy_slice_stream *stream, struct vy_slice *slice,
		   const struct key_def *key_def, struct tuple_format *format,
		   struct tuple_format *upsert_format,
		   struct vy_run_env *run_env, bool is_primary)
{
	stream->base.iface = &vy_slice_stream_iface;

	stream->page_no = slice->first_page_no;
	stream->pos_in_page = 0; /* We'll find it later */
	stream->page = NULL;
	stream->tuple = NULL;

	stream->slice = slice;
	stream->key_def = key_def;
	stream->format = format;
	stream->upsert_format = upsert_format;
	stream->run_env = run_env;
	stream->is_primary = is_primary;
}
