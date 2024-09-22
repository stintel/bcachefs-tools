// SPDX-License-Identifier: GPL-2.0

#include "bcachefs.h"
#include "bcachefs_ioctl.h"
#include "btree_cache.h"
#include "btree_journal_iter.h"
#include "btree_update.h"
#include "btree_write_buffer.h"
#include "buckets.h"
#include "compress.h"
#include "disk_accounting.h"
#include "error.h"
#include "journal_io.h"
#include "replicas.h"

/*
 * Notes on disk accounting:
 *
 * We have two parallel sets of counters to be concerned with, and both must be
 * kept in sync.
 *
 *  - Persistent/on disk accounting, stored in the accounting btree and updated
 *    via btree write buffer updates that treat new accounting keys as deltas to
 *    apply to existing values. But reading from a write buffer btree is
 *    expensive, so we also have
 *
 *  - In memory accounting, where accounting is stored as an array of percpu
 *    counters, indexed by an eytzinger array of disk acounting keys/bpos (which
 *    are the same thing, excepting byte swabbing on big endian).
 *
 *    Cheap to read, but non persistent.
 *
 * Disk accounting updates are generated by transactional triggers; these run as
 * keys enter and leave the btree, and can compare old and new versions of keys;
 * the output of these triggers are deltas to the various counters.
 *
 * Disk accounting updates are done as btree write buffer updates, where the
 * counters in the disk accounting key are deltas that will be applied to the
 * counter in the btree when the key is flushed by the write buffer (or journal
 * replay).
 *
 * To do a disk accounting update:
 * - initialize a disk_accounting_pos, to specify which counter is being update
 * - initialize counter deltas, as an array of 1-3 s64s
 * - call bch2_disk_accounting_mod()
 *
 * This queues up the accounting update to be done at transaction commit time.
 * Underneath, it's a normal btree write buffer update.
 *
 * The transaction commit path is responsible for propagating updates to the in
 * memory counters, with bch2_accounting_mem_mod().
 *
 * The commit path also assigns every disk accounting update a unique version
 * number, based on the journal sequence number and offset within that journal
 * buffer; this is used by journal replay to determine which updates have been
 * done.
 *
 * The transaction commit path also ensures that replicas entry accounting
 * updates are properly marked in the superblock (so that we know whether we can
 * mount without data being unavailable); it will update the superblock if
 * bch2_accounting_mem_mod() tells it to.
 */

static const char * const disk_accounting_type_strs[] = {
#define x(t, n, ...) [n] = #t,
	BCH_DISK_ACCOUNTING_TYPES()
#undef x
	NULL
};

static inline void accounting_key_init(struct bkey_i *k, struct disk_accounting_pos *pos,
				       s64 *d, unsigned nr)
{
	struct bkey_i_accounting *acc = bkey_accounting_init(k);

	acc->k.p = disk_accounting_pos_to_bpos(pos);
	set_bkey_val_u64s(&acc->k, sizeof(struct bch_accounting) / sizeof(u64) + nr);

	memcpy_u64s_small(acc->v.d, d, nr);
}

int bch2_disk_accounting_mod(struct btree_trans *trans,
			     struct disk_accounting_pos *k,
			     s64 *d, unsigned nr, bool gc)
{
	/* Normalize: */
	switch (k->type) {
	case BCH_DISK_ACCOUNTING_replicas:
		bubble_sort(k->replicas.devs, k->replicas.nr_devs, u8_cmp);
		break;
	}

	BUG_ON(nr > BCH_ACCOUNTING_MAX_COUNTERS);

	struct { __BKEY_PADDED(k, BCH_ACCOUNTING_MAX_COUNTERS); } k_i;

	accounting_key_init(&k_i.k, k, d, nr);

	return likely(!gc)
		? bch2_trans_update_buffered(trans, BTREE_ID_accounting, &k_i.k)
		: bch2_accounting_mem_add(trans, bkey_i_to_s_c_accounting(&k_i.k), true);
}

int bch2_mod_dev_cached_sectors(struct btree_trans *trans,
				unsigned dev, s64 sectors,
				bool gc)
{
	struct disk_accounting_pos acc = {
		.type = BCH_DISK_ACCOUNTING_replicas,
	};

	bch2_replicas_entry_cached(&acc.replicas, dev);

	return bch2_disk_accounting_mod(trans, &acc, &sectors, 1, gc);
}

static inline bool is_zero(char *start, char *end)
{
	BUG_ON(start > end);

	for (; start < end; start++)
		if (*start)
			return false;
	return true;
}

#define field_end(p, member)	(((void *) (&p.member)) + sizeof(p.member))

int bch2_accounting_validate(struct bch_fs *c, struct bkey_s_c k,
			     enum bch_validate_flags flags)
{
	struct disk_accounting_pos acc_k;
	bpos_to_disk_accounting_pos(&acc_k, k.k->p);
	void *end = &acc_k + 1;
	int ret = 0;

	bkey_fsck_err_on(bversion_zero(k.k->bversion),
			 c, accounting_key_version_0,
			 "accounting key with version=0");

	switch (acc_k.type) {
	case BCH_DISK_ACCOUNTING_nr_inodes:
		end = field_end(acc_k, nr_inodes);
		break;
	case BCH_DISK_ACCOUNTING_persistent_reserved:
		end = field_end(acc_k, persistent_reserved);
		break;
	case BCH_DISK_ACCOUNTING_replicas:
		bkey_fsck_err_on(!acc_k.replicas.nr_devs,
				 c, accounting_key_replicas_nr_devs_0,
				 "accounting key replicas entry with nr_devs=0");

		bkey_fsck_err_on(acc_k.replicas.nr_required > acc_k.replicas.nr_devs ||
				 (acc_k.replicas.nr_required > 1 &&
				  acc_k.replicas.nr_required == acc_k.replicas.nr_devs),
				 c, accounting_key_replicas_nr_required_bad,
				 "accounting key replicas entry with bad nr_required");

		for (unsigned i = 0; i + 1 < acc_k.replicas.nr_devs; i++)
			bkey_fsck_err_on(acc_k.replicas.devs[i] >= acc_k.replicas.devs[i + 1],
					 c, accounting_key_replicas_devs_unsorted,
					 "accounting key replicas entry with unsorted devs");

		end = (void *) &acc_k.replicas + replicas_entry_bytes(&acc_k.replicas);
		break;
	case BCH_DISK_ACCOUNTING_dev_data_type:
		end = field_end(acc_k, dev_data_type);
		break;
	case BCH_DISK_ACCOUNTING_compression:
		end = field_end(acc_k, compression);
		break;
	case BCH_DISK_ACCOUNTING_snapshot:
		end = field_end(acc_k, snapshot);
		break;
	case BCH_DISK_ACCOUNTING_btree:
		end = field_end(acc_k, btree);
		break;
	case BCH_DISK_ACCOUNTING_rebalance_work:
		end = field_end(acc_k, rebalance_work);
		break;
	}

	bkey_fsck_err_on(!is_zero(end, (void *) (&acc_k + 1)),
			 c, accounting_key_junk_at_end,
			 "junk at end of accounting key");
fsck_err:
	return ret;
}

void bch2_accounting_key_to_text(struct printbuf *out, struct disk_accounting_pos *k)
{
	if (k->type >= BCH_DISK_ACCOUNTING_TYPE_NR) {
		prt_printf(out, "unknown type %u", k->type);
		return;
	}

	prt_str(out, disk_accounting_type_strs[k->type]);
	prt_str(out, " ");

	switch (k->type) {
	case BCH_DISK_ACCOUNTING_nr_inodes:
		break;
	case BCH_DISK_ACCOUNTING_persistent_reserved:
		prt_printf(out, "replicas=%u", k->persistent_reserved.nr_replicas);
		break;
	case BCH_DISK_ACCOUNTING_replicas:
		bch2_replicas_entry_to_text(out, &k->replicas);
		break;
	case BCH_DISK_ACCOUNTING_dev_data_type:
		prt_printf(out, "dev=%u data_type=", k->dev_data_type.dev);
		bch2_prt_data_type(out, k->dev_data_type.data_type);
		break;
	case BCH_DISK_ACCOUNTING_compression:
		bch2_prt_compression_type(out, k->compression.type);
		break;
	case BCH_DISK_ACCOUNTING_snapshot:
		prt_printf(out, "id=%u", k->snapshot.id);
		break;
	case BCH_DISK_ACCOUNTING_btree:
		prt_printf(out, "btree=%s", bch2_btree_id_str(k->btree.id));
		break;
	}
}

void bch2_accounting_to_text(struct printbuf *out, struct bch_fs *c, struct bkey_s_c k)
{
	struct bkey_s_c_accounting acc = bkey_s_c_to_accounting(k);
	struct disk_accounting_pos acc_k;
	bpos_to_disk_accounting_pos(&acc_k, k.k->p);

	bch2_accounting_key_to_text(out, &acc_k);

	for (unsigned i = 0; i < bch2_accounting_counters(k.k); i++)
		prt_printf(out, " %lli", acc.v->d[i]);
}

void bch2_accounting_swab(struct bkey_s k)
{
	for (u64 *p = (u64 *) k.v;
	     p < (u64 *) bkey_val_end(k);
	     p++)
		*p = swab64(*p);
}

static inline bool accounting_to_replicas(struct bch_replicas_entry_v1 *r, struct bpos p)
{
	struct disk_accounting_pos acc_k;
	bpos_to_disk_accounting_pos(&acc_k, p);

	switch (acc_k.type) {
	case BCH_DISK_ACCOUNTING_replicas:
		unsafe_memcpy(r, &acc_k.replicas,
			      replicas_entry_bytes(&acc_k.replicas),
			      "variable length struct");
		return true;
	default:
		return false;
	}
}

static int bch2_accounting_update_sb_one(struct bch_fs *c, struct bpos p)
{
	struct bch_replicas_padded r;
	return accounting_to_replicas(&r.e, p)
		? bch2_mark_replicas(c, &r.e)
		: 0;
}

/*
 * Ensure accounting keys being updated are present in the superblock, when
 * applicable (i.e. replicas updates)
 */
int bch2_accounting_update_sb(struct btree_trans *trans)
{
	for (struct jset_entry *i = trans->journal_entries;
	     i != (void *) ((u64 *) trans->journal_entries + trans->journal_entries_u64s);
	     i = vstruct_next(i))
		if (jset_entry_is_key(i) && i->start->k.type == KEY_TYPE_accounting) {
			int ret = bch2_accounting_update_sb_one(trans->c, i->start->k.p);
			if (ret)
				return ret;
		}

	return 0;
}

static int __bch2_accounting_mem_insert(struct bch_fs *c, struct bkey_s_c_accounting a)
{
	struct bch_accounting_mem *acc = &c->accounting;

	/* raced with another insert, already present: */
	if (eytzinger0_find(acc->k.data, acc->k.nr, sizeof(acc->k.data[0]),
			    accounting_pos_cmp, &a.k->p) < acc->k.nr)
		return 0;

	struct accounting_mem_entry n = {
		.pos		= a.k->p,
		.bversion	= a.k->bversion,
		.nr_counters	= bch2_accounting_counters(a.k),
		.v[0]		= __alloc_percpu_gfp(n.nr_counters * sizeof(u64),
						     sizeof(u64), GFP_KERNEL),
	};

	if (!n.v[0])
		goto err;

	if (acc->gc_running) {
		n.v[1] = __alloc_percpu_gfp(n.nr_counters * sizeof(u64),
					    sizeof(u64), GFP_KERNEL);
		if (!n.v[1])
			goto err;
	}

	if (darray_push(&acc->k, n))
		goto err;

	eytzinger0_sort(acc->k.data, acc->k.nr, sizeof(acc->k.data[0]),
			accounting_pos_cmp, NULL);
	return 0;
err:
	free_percpu(n.v[1]);
	free_percpu(n.v[0]);
	return -BCH_ERR_ENOMEM_disk_accounting;
}

int bch2_accounting_mem_insert(struct bch_fs *c, struct bkey_s_c_accounting a,
			       enum bch_accounting_mode mode)
{
	struct bch_replicas_padded r;

	if (mode != BCH_ACCOUNTING_read &&
	    accounting_to_replicas(&r.e, a.k->p) &&
	    !bch2_replicas_marked_locked(c, &r.e))
		return -BCH_ERR_btree_insert_need_mark_replicas;

	percpu_up_read(&c->mark_lock);
	percpu_down_write(&c->mark_lock);
	int ret = __bch2_accounting_mem_insert(c, a);
	percpu_up_write(&c->mark_lock);
	percpu_down_read(&c->mark_lock);
	return ret;
}

static bool accounting_mem_entry_is_zero(struct accounting_mem_entry *e)
{
	for (unsigned i = 0; i < e->nr_counters; i++)
		if (percpu_u64_get(e->v[0] + i) ||
		    (e->v[1] &&
		     percpu_u64_get(e->v[1] + i)))
			return false;
	return true;
}

void bch2_accounting_mem_gc(struct bch_fs *c)
{
	struct bch_accounting_mem *acc = &c->accounting;

	percpu_down_write(&c->mark_lock);
	struct accounting_mem_entry *dst = acc->k.data;

	darray_for_each(acc->k, src) {
		if (accounting_mem_entry_is_zero(src)) {
			free_percpu(src->v[0]);
			free_percpu(src->v[1]);
		} else {
			*dst++ = *src;
		}
	}

	acc->k.nr = dst - acc->k.data;
	eytzinger0_sort(acc->k.data, acc->k.nr, sizeof(acc->k.data[0]),
			accounting_pos_cmp, NULL);
	percpu_up_write(&c->mark_lock);
}

/*
 * Read out accounting keys for replicas entries, as an array of
 * bch_replicas_usage entries.
 *
 * Note: this may be deprecated/removed at smoe point in the future and replaced
 * with something more general, it exists to support the ioctl used by the
 * 'bcachefs fs usage' command.
 */
int bch2_fs_replicas_usage_read(struct bch_fs *c, darray_char *usage)
{
	struct bch_accounting_mem *acc = &c->accounting;
	int ret = 0;

	darray_init(usage);

	percpu_down_read(&c->mark_lock);
	darray_for_each(acc->k, i) {
		struct {
			struct bch_replicas_usage r;
			u8 pad[BCH_BKEY_PTRS_MAX];
		} u;

		if (!accounting_to_replicas(&u.r.r, i->pos))
			continue;

		u64 sectors;
		bch2_accounting_mem_read_counters(acc, i - acc->k.data, &sectors, 1, false);
		u.r.sectors = sectors;

		ret = darray_make_room(usage, replicas_usage_bytes(&u.r));
		if (ret)
			break;

		memcpy(&darray_top(*usage), &u.r, replicas_usage_bytes(&u.r));
		usage->nr += replicas_usage_bytes(&u.r);
	}
	percpu_up_read(&c->mark_lock);

	if (ret)
		darray_exit(usage);
	return ret;
}

int bch2_fs_accounting_read(struct bch_fs *c, darray_char *out_buf, unsigned accounting_types_mask)
{

	struct bch_accounting_mem *acc = &c->accounting;
	int ret = 0;

	darray_init(out_buf);

	percpu_down_read(&c->mark_lock);
	darray_for_each(acc->k, i) {
		struct disk_accounting_pos a_p;
		bpos_to_disk_accounting_pos(&a_p, i->pos);

		if (!(accounting_types_mask & BIT(a_p.type)))
			continue;

		ret = darray_make_room(out_buf, sizeof(struct bkey_i_accounting) +
				       sizeof(u64) * i->nr_counters);
		if (ret)
			break;

		struct bkey_i_accounting *a_out =
			bkey_accounting_init((void *) &darray_top(*out_buf));
		set_bkey_val_u64s(&a_out->k, i->nr_counters);
		a_out->k.p = i->pos;
		bch2_accounting_mem_read_counters(acc, i - acc->k.data,
						  a_out->v.d, i->nr_counters, false);

		if (!bch2_accounting_key_is_zero(accounting_i_to_s_c(a_out)))
			out_buf->nr += bkey_bytes(&a_out->k);
	}

	percpu_up_read(&c->mark_lock);

	if (ret)
		darray_exit(out_buf);
	return ret;
}

void bch2_fs_accounting_to_text(struct printbuf *out, struct bch_fs *c)
{
	struct bch_accounting_mem *acc = &c->accounting;

	percpu_down_read(&c->mark_lock);
	out->atomic++;

	eytzinger0_for_each(i, acc->k.nr) {
		struct disk_accounting_pos acc_k;
		bpos_to_disk_accounting_pos(&acc_k, acc->k.data[i].pos);

		bch2_accounting_key_to_text(out, &acc_k);

		u64 v[BCH_ACCOUNTING_MAX_COUNTERS];
		bch2_accounting_mem_read_counters(acc, i, v, ARRAY_SIZE(v), false);

		prt_str(out, ":");
		for (unsigned j = 0; j < acc->k.data[i].nr_counters; j++)
			prt_printf(out, " %llu", v[j]);
		prt_newline(out);
	}

	--out->atomic;
	percpu_up_read(&c->mark_lock);
}

static void bch2_accounting_free_counters(struct bch_accounting_mem *acc, bool gc)
{
	darray_for_each(acc->k, e) {
		free_percpu(e->v[gc]);
		e->v[gc] = NULL;
	}
}

int bch2_gc_accounting_start(struct bch_fs *c)
{
	struct bch_accounting_mem *acc = &c->accounting;
	int ret = 0;

	percpu_down_write(&c->mark_lock);
	darray_for_each(acc->k, e) {
		e->v[1] = __alloc_percpu_gfp(e->nr_counters * sizeof(u64),
					     sizeof(u64), GFP_KERNEL);
		if (!e->v[1]) {
			bch2_accounting_free_counters(acc, true);
			ret = -BCH_ERR_ENOMEM_disk_accounting;
			break;
		}
	}

	acc->gc_running = !ret;
	percpu_up_write(&c->mark_lock);

	return ret;
}

int bch2_gc_accounting_done(struct bch_fs *c)
{
	struct bch_accounting_mem *acc = &c->accounting;
	struct btree_trans *trans = bch2_trans_get(c);
	struct printbuf buf = PRINTBUF;
	struct bpos pos = POS_MIN;
	int ret = 0;

	percpu_down_write(&c->mark_lock);
	while (1) {
		unsigned idx = eytzinger0_find_ge(acc->k.data, acc->k.nr, sizeof(acc->k.data[0]),
						  accounting_pos_cmp, &pos);

		if (idx >= acc->k.nr)
			break;

		struct accounting_mem_entry *e = acc->k.data + idx;
		pos = bpos_successor(e->pos);

		struct disk_accounting_pos acc_k;
		bpos_to_disk_accounting_pos(&acc_k, e->pos);

		if (acc_k.type >= BCH_DISK_ACCOUNTING_TYPE_NR)
			continue;

		u64 src_v[BCH_ACCOUNTING_MAX_COUNTERS];
		u64 dst_v[BCH_ACCOUNTING_MAX_COUNTERS];

		unsigned nr = e->nr_counters;
		bch2_accounting_mem_read_counters(acc, idx, dst_v, nr, false);
		bch2_accounting_mem_read_counters(acc, idx, src_v, nr, true);

		if (memcmp(dst_v, src_v, nr * sizeof(u64))) {
			printbuf_reset(&buf);
			prt_str(&buf, "accounting mismatch for ");
			bch2_accounting_key_to_text(&buf, &acc_k);

			prt_str(&buf, ": got");
			for (unsigned j = 0; j < nr; j++)
				prt_printf(&buf, " %llu", dst_v[j]);

			prt_str(&buf, " should be");
			for (unsigned j = 0; j < nr; j++)
				prt_printf(&buf, " %llu", src_v[j]);

			for (unsigned j = 0; j < nr; j++)
				src_v[j] -= dst_v[j];

			if (fsck_err(trans, accounting_mismatch, "%s", buf.buf)) {
				percpu_up_write(&c->mark_lock);
				ret = commit_do(trans, NULL, NULL, 0,
						bch2_disk_accounting_mod(trans, &acc_k, src_v, nr, false));
				percpu_down_write(&c->mark_lock);
				if (ret)
					goto err;

				if (!test_bit(BCH_FS_may_go_rw, &c->flags)) {
					memset(&trans->fs_usage_delta, 0, sizeof(trans->fs_usage_delta));
					struct { __BKEY_PADDED(k, BCH_ACCOUNTING_MAX_COUNTERS); } k_i;

					accounting_key_init(&k_i.k, &acc_k, src_v, nr);
					bch2_accounting_mem_mod_locked(trans,
								bkey_i_to_s_c_accounting(&k_i.k),
								BCH_ACCOUNTING_normal);

					preempt_disable();
					struct bch_fs_usage_base *dst = this_cpu_ptr(c->usage);
					struct bch_fs_usage_base *src = &trans->fs_usage_delta;
					acc_u64s((u64 *) dst, (u64 *) src, sizeof(*src) / sizeof(u64));
					preempt_enable();
				}
			}
		}
	}
err:
fsck_err:
	percpu_up_write(&c->mark_lock);
	printbuf_exit(&buf);
	bch2_trans_put(trans);
	bch_err_fn(c, ret);
	return ret;
}

static int accounting_read_key(struct btree_trans *trans, struct bkey_s_c k)
{
	struct bch_fs *c = trans->c;

	if (k.k->type != KEY_TYPE_accounting)
		return 0;

	percpu_down_read(&c->mark_lock);
	int ret = bch2_accounting_mem_mod_locked(trans, bkey_s_c_to_accounting(k),
						 BCH_ACCOUNTING_read);
	percpu_up_read(&c->mark_lock);
	return ret;
}

/*
 * At startup time, initialize the in memory accounting from the btree (and
 * journal)
 */
int bch2_accounting_read(struct bch_fs *c)
{
	struct bch_accounting_mem *acc = &c->accounting;
	struct btree_trans *trans = bch2_trans_get(c);
	struct printbuf buf = PRINTBUF;

	int ret = for_each_btree_key(trans, iter,
				BTREE_ID_accounting, POS_MIN,
				BTREE_ITER_prefetch|BTREE_ITER_all_snapshots, k, ({
			struct bkey u;
			struct bkey_s_c k = bch2_btree_path_peek_slot_exact(btree_iter_path(trans, &iter), &u);
			accounting_read_key(trans, k);
		}));
	if (ret)
		goto err;

	struct journal_keys *keys = &c->journal_keys;
	struct journal_key *dst = keys->data;
	move_gap(keys, keys->nr);

	darray_for_each(*keys, i) {
		if (i->k->k.type == KEY_TYPE_accounting) {
			struct bkey_s_c k = bkey_i_to_s_c(i->k);
			unsigned idx = eytzinger0_find(acc->k.data, acc->k.nr,
						sizeof(acc->k.data[0]),
						accounting_pos_cmp, &k.k->p);

			bool applied = idx < acc->k.nr &&
				bversion_cmp(acc->k.data[idx].bversion, k.k->bversion) >= 0;

			if (applied)
				continue;

			if (i + 1 < &darray_top(*keys) &&
			    i[1].k->k.type == KEY_TYPE_accounting &&
			    !journal_key_cmp(i, i + 1)) {
				WARN_ON(bversion_cmp(i[0].k->k.bversion, i[1].k->k.bversion) >= 0);

				i[1].journal_seq = i[0].journal_seq;

				bch2_accounting_accumulate(bkey_i_to_accounting(i[1].k),
							   bkey_s_c_to_accounting(k));
				continue;
			}

			ret = accounting_read_key(trans, k);
			if (ret)
				goto err;
		}

		*dst++ = *i;
	}
	keys->gap = keys->nr = dst - keys->data;

	percpu_down_read(&c->mark_lock);
	for (unsigned i = 0; i < acc->k.nr; i++) {
		u64 v[BCH_ACCOUNTING_MAX_COUNTERS];
		bch2_accounting_mem_read_counters(acc, i, v, ARRAY_SIZE(v), false);

		if (bch2_is_zero(v, sizeof(v[0]) * acc->k.data[i].nr_counters))
			continue;

		struct bch_replicas_padded r;
		if (!accounting_to_replicas(&r.e, acc->k.data[i].pos))
			continue;

		/*
		 * If the replicas entry is invalid it'll get cleaned up by
		 * check_allocations:
		 */
		if (bch2_replicas_entry_validate(&r.e, c, &buf))
			continue;

		struct disk_accounting_pos k;
		bpos_to_disk_accounting_pos(&k, acc->k.data[i].pos);

		if (fsck_err_on(!bch2_replicas_marked_locked(c, &r.e),
				trans, accounting_replicas_not_marked,
				"accounting not marked in superblock replicas\n  %s",
				(printbuf_reset(&buf),
				 bch2_accounting_key_to_text(&buf, &k),
				 buf.buf))) {
			/*
			 * We're not RW yet and still single threaded, dropping
			 * and retaking lock is ok:
			 */
			percpu_up_read(&c->mark_lock);
			ret = bch2_mark_replicas(c, &r.e);
			if (ret)
				goto fsck_err;
			percpu_down_read(&c->mark_lock);
		}
	}

	preempt_disable();
	struct bch_fs_usage_base *usage = this_cpu_ptr(c->usage);

	for (unsigned i = 0; i < acc->k.nr; i++) {
		struct disk_accounting_pos k;
		bpos_to_disk_accounting_pos(&k, acc->k.data[i].pos);

		u64 v[BCH_ACCOUNTING_MAX_COUNTERS];
		bch2_accounting_mem_read_counters(acc, i, v, ARRAY_SIZE(v), false);

		switch (k.type) {
		case BCH_DISK_ACCOUNTING_persistent_reserved:
			usage->reserved += v[0] * k.persistent_reserved.nr_replicas;
			break;
		case BCH_DISK_ACCOUNTING_replicas:
			fs_usage_data_type_to_base(usage, k.replicas.data_type, v[0]);
			break;
		case BCH_DISK_ACCOUNTING_dev_data_type:
			rcu_read_lock();
			struct bch_dev *ca = bch2_dev_rcu(c, k.dev_data_type.dev);
			if (ca) {
				struct bch_dev_usage_type __percpu *d = &ca->usage->d[k.dev_data_type.data_type];
				percpu_u64_set(&d->buckets,	v[0]);
				percpu_u64_set(&d->sectors,	v[1]);
				percpu_u64_set(&d->fragmented,	v[2]);

				if (k.dev_data_type.data_type == BCH_DATA_sb ||
				    k.dev_data_type.data_type == BCH_DATA_journal)
					usage->hidden += v[0] * ca->mi.bucket_size;
			}
			rcu_read_unlock();
			break;
		}
	}
	preempt_enable();
fsck_err:
	percpu_up_read(&c->mark_lock);
err:
	printbuf_exit(&buf);
	bch2_trans_put(trans);
	bch_err_fn(c, ret);
	return ret;
}

int bch2_dev_usage_remove(struct bch_fs *c, unsigned dev)
{
	return bch2_trans_run(c,
		bch2_btree_write_buffer_flush_sync(trans) ?:
		for_each_btree_key_commit(trans, iter, BTREE_ID_accounting, POS_MIN,
				BTREE_ITER_all_snapshots, k, NULL, NULL, 0, ({
			struct disk_accounting_pos acc;
			bpos_to_disk_accounting_pos(&acc, k.k->p);

			acc.type == BCH_DISK_ACCOUNTING_dev_data_type &&
			acc.dev_data_type.dev == dev
				? bch2_btree_bit_mod_buffered(trans, BTREE_ID_accounting, k.k->p, 0)
				: 0;
		})) ?:
		bch2_btree_write_buffer_flush_sync(trans));
}

int bch2_dev_usage_init(struct bch_dev *ca, bool gc)
{
	struct bch_fs *c = ca->fs;
	struct disk_accounting_pos acc = {
		.type = BCH_DISK_ACCOUNTING_dev_data_type,
		.dev_data_type.dev = ca->dev_idx,
		.dev_data_type.data_type = BCH_DATA_free,
	};
	u64 v[3] = { ca->mi.nbuckets - ca->mi.first_bucket, 0, 0 };

	int ret = bch2_trans_do(c, NULL, NULL, 0,
			bch2_disk_accounting_mod(trans, &acc, v, ARRAY_SIZE(v), gc));
	bch_err_fn(c, ret);
	return ret;
}

void bch2_verify_accounting_clean(struct bch_fs *c)
{
	bool mismatch = false;
	struct bch_fs_usage_base base = {}, base_inmem = {};

	bch2_trans_run(c,
		for_each_btree_key(trans, iter,
				   BTREE_ID_accounting, POS_MIN,
				   BTREE_ITER_all_snapshots, k, ({
			u64 v[BCH_ACCOUNTING_MAX_COUNTERS];
			struct bkey_s_c_accounting a = bkey_s_c_to_accounting(k);
			unsigned nr = bch2_accounting_counters(k.k);

			struct disk_accounting_pos acc_k;
			bpos_to_disk_accounting_pos(&acc_k, k.k->p);

			if (acc_k.type >= BCH_DISK_ACCOUNTING_TYPE_NR)
				continue;

			if (acc_k.type == BCH_DISK_ACCOUNTING_inum)
				continue;

			bch2_accounting_mem_read(c, k.k->p, v, nr);

			if (memcmp(a.v->d, v, nr * sizeof(u64))) {
				struct printbuf buf = PRINTBUF;

				bch2_bkey_val_to_text(&buf, c, k);
				prt_str(&buf, " !=");
				for (unsigned j = 0; j < nr; j++)
					prt_printf(&buf, " %llu", v[j]);

				pr_err("%s", buf.buf);
				printbuf_exit(&buf);
				mismatch = true;
			}

			switch (acc_k.type) {
			case BCH_DISK_ACCOUNTING_persistent_reserved:
				base.reserved += acc_k.persistent_reserved.nr_replicas * a.v->d[0];
				break;
			case BCH_DISK_ACCOUNTING_replicas:
				fs_usage_data_type_to_base(&base, acc_k.replicas.data_type, a.v->d[0]);
				break;
			case BCH_DISK_ACCOUNTING_dev_data_type: {
				rcu_read_lock();
				struct bch_dev *ca = bch2_dev_rcu(c, acc_k.dev_data_type.dev);
				if (!ca) {
					rcu_read_unlock();
					continue;
				}

				v[0] = percpu_u64_get(&ca->usage->d[acc_k.dev_data_type.data_type].buckets);
				v[1] = percpu_u64_get(&ca->usage->d[acc_k.dev_data_type.data_type].sectors);
				v[2] = percpu_u64_get(&ca->usage->d[acc_k.dev_data_type.data_type].fragmented);
				rcu_read_unlock();

				if (memcmp(a.v->d, v, 3 * sizeof(u64))) {
					struct printbuf buf = PRINTBUF;

					bch2_bkey_val_to_text(&buf, c, k);
					prt_str(&buf, " in mem");
					for (unsigned j = 0; j < nr; j++)
						prt_printf(&buf, " %llu", v[j]);

					pr_err("dev accounting mismatch: %s", buf.buf);
					printbuf_exit(&buf);
					mismatch = true;
				}
			}
			}

			0;
		})));

	acc_u64s_percpu(&base_inmem.hidden, &c->usage->hidden, sizeof(base_inmem) / sizeof(u64));

#define check(x)										\
	if (base.x != base_inmem.x) {								\
		pr_err("fs_usage_base.%s mismatch: %llu != %llu", #x, base.x, base_inmem.x);	\
		mismatch = true;								\
	}

	//check(hidden);
	check(btree);
	check(data);
	check(cached);
	check(reserved);
	check(nr_inodes);

	WARN_ON(mismatch);
}

void bch2_accounting_gc_free(struct bch_fs *c)
{
	lockdep_assert_held(&c->mark_lock);

	struct bch_accounting_mem *acc = &c->accounting;

	bch2_accounting_free_counters(acc, true);
	acc->gc_running = false;
}

void bch2_fs_accounting_exit(struct bch_fs *c)
{
	struct bch_accounting_mem *acc = &c->accounting;

	bch2_accounting_free_counters(acc, false);
	darray_exit(&acc->k);
}
