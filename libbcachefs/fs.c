// SPDX-License-Identifier: GPL-2.0
#ifndef NO_BCACHEFS_FS

#include "bcachefs.h"
#include "acl.h"
#include "bkey_buf.h"
#include "btree_update.h"
#include "buckets.h"
#include "chardev.h"
#include "dirent.h"
#include "errcode.h"
#include "extents.h"
#include "fs.h"
#include "fs-common.h"
#include "fs-io.h"
#include "fs-ioctl.h"
#include "fs-io-buffered.h"
#include "fs-io-direct.h"
#include "fs-io-pagecache.h"
#include "fsck.h"
#include "inode.h"
#include "io_read.h"
#include "journal.h"
#include "keylist.h"
#include "quota.h"
#include "snapshot.h"
#include "super.h"
#include "xattr.h"
#include "trace.h"

#include <linux/aio.h>
#include <linux/backing-dev.h>
#include <linux/exportfs.h>
#include <linux/fiemap.h>
#include <linux/fs_context.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/posix_acl.h>
#include <linux/random.h>
#include <linux/seq_file.h>
#include <linux/statfs.h>
#include <linux/string.h>
#include <linux/xattr.h>

static struct kmem_cache *bch2_inode_cache;

static void bch2_vfs_inode_init(struct btree_trans *, subvol_inum,
				struct bch_inode_info *,
				struct bch_inode_unpacked *,
				struct bch_subvolume *);

void bch2_inode_update_after_write(struct btree_trans *trans,
				   struct bch_inode_info *inode,
				   struct bch_inode_unpacked *bi,
				   unsigned fields)
{
	struct bch_fs *c = trans->c;

	BUG_ON(bi->bi_inum != inode->v.i_ino);

	bch2_assert_pos_locked(trans, BTREE_ID_inodes, POS(0, bi->bi_inum));

	set_nlink(&inode->v, bch2_inode_nlink_get(bi));
	i_uid_write(&inode->v, bi->bi_uid);
	i_gid_write(&inode->v, bi->bi_gid);
	inode->v.i_mode	= bi->bi_mode;

	if (fields & ATTR_ATIME)
		inode_set_atime_to_ts(&inode->v, bch2_time_to_timespec(c, bi->bi_atime));
	if (fields & ATTR_MTIME)
		inode_set_mtime_to_ts(&inode->v, bch2_time_to_timespec(c, bi->bi_mtime));
	if (fields & ATTR_CTIME)
		inode_set_ctime_to_ts(&inode->v, bch2_time_to_timespec(c, bi->bi_ctime));

	inode->ei_inode		= *bi;

	bch2_inode_flags_to_vfs(inode);
}

int __must_check bch2_write_inode(struct bch_fs *c,
				  struct bch_inode_info *inode,
				  inode_set_fn set,
				  void *p, unsigned fields)
{
	struct btree_trans *trans = bch2_trans_get(c);
	struct btree_iter iter = { NULL };
	struct bch_inode_unpacked inode_u;
	int ret;
retry:
	bch2_trans_begin(trans);

	ret   = bch2_inode_peek(trans, &iter, &inode_u, inode_inum(inode),
				BTREE_ITER_intent) ?:
		(set ? set(trans, inode, &inode_u, p) : 0) ?:
		bch2_inode_write(trans, &iter, &inode_u) ?:
		bch2_trans_commit(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc);

	/*
	 * the btree node lock protects inode->ei_inode, not ei_update_lock;
	 * this is important for inode updates via bchfs_write_index_update
	 */
	if (!ret)
		bch2_inode_update_after_write(trans, inode, &inode_u, fields);

	bch2_trans_iter_exit(trans, &iter);

	if (bch2_err_matches(ret, BCH_ERR_transaction_restart))
		goto retry;

	bch2_fs_fatal_err_on(bch2_err_matches(ret, ENOENT), c,
			     "%s: inode %llu:%llu not found when updating",
			     bch2_err_str(ret),
			     inode_inum(inode).subvol,
			     inode_inum(inode).inum);

	bch2_trans_put(trans);
	return ret < 0 ? ret : 0;
}

int bch2_fs_quota_transfer(struct bch_fs *c,
			   struct bch_inode_info *inode,
			   struct bch_qid new_qid,
			   unsigned qtypes,
			   enum quota_acct_mode mode)
{
	unsigned i;
	int ret;

	qtypes &= enabled_qtypes(c);

	for (i = 0; i < QTYP_NR; i++)
		if (new_qid.q[i] == inode->ei_qid.q[i])
			qtypes &= ~(1U << i);

	if (!qtypes)
		return 0;

	mutex_lock(&inode->ei_quota_lock);

	ret = bch2_quota_transfer(c, qtypes, new_qid,
				  inode->ei_qid,
				  inode->v.i_blocks +
				  inode->ei_quota_reserved,
				  mode);
	if (!ret)
		for (i = 0; i < QTYP_NR; i++)
			if (qtypes & (1 << i))
				inode->ei_qid.q[i] = new_qid.q[i];

	mutex_unlock(&inode->ei_quota_lock);

	return ret;
}

static bool subvol_inum_eq(subvol_inum a, subvol_inum b)
{
	return a.subvol == b.subvol && a.inum == b.inum;
}

static int bch2_vfs_inode_cmp_fn(struct rhashtable_compare_arg *arg,
				 const void *obj)
{
	const struct bch_inode_info *inode = obj;
	const subvol_inum *v = arg->key;

	return !subvol_inum_eq(inode->ei_inum, *v);
}

static const struct rhashtable_params bch2_vfs_inodes_params = {
	.head_offset		= offsetof(struct bch_inode_info, hash),
	.key_offset		= offsetof(struct bch_inode_info, ei_inum),
	.key_len		= sizeof(subvol_inum),
	.obj_cmpfn		= bch2_vfs_inode_cmp_fn,
	.automatic_shrinking	= true,
};

static void __wait_on_freeing_inode(struct inode *inode)
{
	wait_queue_head_t *wq;
	DEFINE_WAIT_BIT(wait, &inode->i_state, __I_NEW);
	wq = bit_waitqueue(&inode->i_state, __I_NEW);
	prepare_to_wait(wq, &wait.wq_entry, TASK_UNINTERRUPTIBLE);
	spin_unlock(&inode->i_lock);
	schedule();
	finish_wait(wq, &wait.wq_entry);
}

struct bch_inode_info *__bch2_inode_hash_find(struct bch_fs *c, subvol_inum inum)
{
	return rhashtable_lookup_fast(&c->vfs_inodes_table, &inum, bch2_vfs_inodes_params);
}

static struct bch_inode_info *bch2_inode_hash_find(struct bch_fs *c, struct btree_trans *trans,
						   subvol_inum inum)
{
	struct bch_inode_info *inode;
repeat:
	inode = __bch2_inode_hash_find(c, inum);
	if (inode) {
		spin_lock(&inode->v.i_lock);
		if (!test_bit(EI_INODE_HASHED, &inode->ei_flags)) {
			spin_unlock(&inode->v.i_lock);
			return NULL;
		}
		if ((inode->v.i_state & (I_FREEING|I_WILL_FREE))) {
			if (!trans) {
				__wait_on_freeing_inode(&inode->v);
			} else {
				bch2_trans_unlock(trans);
				__wait_on_freeing_inode(&inode->v);
				int ret = bch2_trans_relock(trans);
				if (ret)
					return ERR_PTR(ret);
			}
			goto repeat;
		}
		__iget(&inode->v);
		spin_unlock(&inode->v.i_lock);
	}

	return inode;
}

static void bch2_inode_hash_remove(struct bch_fs *c, struct bch_inode_info *inode)
{
	spin_lock(&inode->v.i_lock);
	bool remove = test_and_clear_bit(EI_INODE_HASHED, &inode->ei_flags);
	spin_unlock(&inode->v.i_lock);

	if (remove) {
		int ret = rhashtable_remove_fast(&c->vfs_inodes_table,
					&inode->hash, bch2_vfs_inodes_params);
		BUG_ON(ret);
		inode->v.i_hash.pprev = NULL;
	}
}

static struct bch_inode_info *bch2_inode_hash_insert(struct bch_fs *c,
						     struct btree_trans *trans,
						     struct bch_inode_info *inode)
{
	struct bch_inode_info *old = inode;

	set_bit(EI_INODE_HASHED, &inode->ei_flags);
retry:
	if (unlikely(rhashtable_lookup_insert_fast(&c->vfs_inodes_table,
					&inode->hash,
					bch2_vfs_inodes_params))) {
		old = bch2_inode_hash_find(c, trans, inode->ei_inum);
		if (!old)
			goto retry;

		clear_bit(EI_INODE_HASHED, &inode->ei_flags);

		/*
		 * bcachefs doesn't use I_NEW; we have no use for it since we
		 * only insert fully created inodes in the inode hash table. But
		 * discard_new_inode() expects it to be set...
		 */
		inode->v.i_state |= I_NEW;
		/*
		 * We don't want bch2_evict_inode() to delete the inode on disk,
		 * we just raced and had another inode in cache. Normally new
		 * inodes don't have nlink == 0 - except tmpfiles do...
		 */
		set_nlink(&inode->v, 1);
		discard_new_inode(&inode->v);
		return old;
	} else {
		inode_fake_hash(&inode->v);

		inode_sb_list_add(&inode->v);

		mutex_lock(&c->vfs_inodes_lock);
		list_add(&inode->ei_vfs_inode_list, &c->vfs_inodes_list);
		mutex_unlock(&c->vfs_inodes_lock);
		return inode;
	}
}

#define memalloc_flags_do(_flags, _do)						\
({										\
	unsigned _saved_flags = memalloc_flags_save(_flags);			\
	typeof(_do) _ret = _do;							\
	memalloc_noreclaim_restore(_saved_flags);				\
	_ret;									\
})

static struct inode *bch2_alloc_inode(struct super_block *sb)
{
	BUG();
}

static struct bch_inode_info *__bch2_new_inode(struct bch_fs *c)
{
	struct bch_inode_info *inode = alloc_inode_sb(c->vfs_sb,
						bch2_inode_cache, GFP_NOFS);
	if (!inode)
		return NULL;

	inode_init_once(&inode->v);
	mutex_init(&inode->ei_update_lock);
	two_state_lock_init(&inode->ei_pagecache_lock);
	INIT_LIST_HEAD(&inode->ei_vfs_inode_list);
	inode->ei_flags = 0;
	mutex_init(&inode->ei_quota_lock);
	memset(&inode->ei_devs_need_flush, 0, sizeof(inode->ei_devs_need_flush));

	if (unlikely(inode_init_always(c->vfs_sb, &inode->v))) {
		kmem_cache_free(bch2_inode_cache, inode);
		return NULL;
	}

	return inode;
}

/*
 * Allocate a new inode, dropping/retaking btree locks if necessary:
 */
static struct bch_inode_info *bch2_new_inode(struct btree_trans *trans)
{
	struct bch_inode_info *inode =
		memalloc_flags_do(PF_MEMALLOC_NORECLAIM|PF_MEMALLOC_NOWARN,
				  __bch2_new_inode(trans->c));

	if (unlikely(!inode)) {
		int ret = drop_locks_do(trans, (inode = __bch2_new_inode(trans->c)) ? 0 : -ENOMEM);
		if (ret && inode) {
			__destroy_inode(&inode->v);
			kmem_cache_free(bch2_inode_cache, inode);
		}
		if (ret)
			return ERR_PTR(ret);
	}

	return inode;
}

static struct bch_inode_info *bch2_inode_hash_init_insert(struct btree_trans *trans,
							  subvol_inum inum,
							  struct bch_inode_unpacked *bi,
							  struct bch_subvolume *subvol)
{
	struct bch_inode_info *inode = bch2_new_inode(trans);
	if (IS_ERR(inode))
		return inode;

	bch2_vfs_inode_init(trans, inum, inode, bi, subvol);

	return bch2_inode_hash_insert(trans->c, trans, inode);

}

struct inode *bch2_vfs_inode_get(struct bch_fs *c, subvol_inum inum)
{
	struct bch_inode_info *inode = bch2_inode_hash_find(c, NULL, inum);
	if (inode)
		return &inode->v;

	struct btree_trans *trans = bch2_trans_get(c);

	struct bch_inode_unpacked inode_u;
	struct bch_subvolume subvol;
	int ret = lockrestart_do(trans,
		bch2_subvolume_get(trans, inum.subvol, true, 0, &subvol) ?:
		bch2_inode_find_by_inum_trans(trans, inum, &inode_u)) ?:
		PTR_ERR_OR_ZERO(inode = bch2_inode_hash_init_insert(trans, inum, &inode_u, &subvol));
	bch2_trans_put(trans);

	return ret ? ERR_PTR(ret) : &inode->v;
}

struct bch_inode_info *
__bch2_create(struct mnt_idmap *idmap,
	      struct bch_inode_info *dir, struct dentry *dentry,
	      umode_t mode, dev_t rdev, subvol_inum snapshot_src,
	      unsigned flags)
{
	struct bch_fs *c = dir->v.i_sb->s_fs_info;
	struct btree_trans *trans;
	struct bch_inode_unpacked dir_u;
	struct bch_inode_info *inode;
	struct bch_inode_unpacked inode_u;
	struct posix_acl *default_acl = NULL, *acl = NULL;
	subvol_inum inum;
	struct bch_subvolume subvol;
	u64 journal_seq = 0;
	kuid_t kuid;
	kgid_t kgid;
	int ret;

	/*
	 * preallocate acls + vfs inode before btree transaction, so that
	 * nothing can fail after the transaction succeeds:
	 */
#ifdef CONFIG_BCACHEFS_POSIX_ACL
	ret = posix_acl_create(&dir->v, &mode, &default_acl, &acl);
	if (ret)
		return ERR_PTR(ret);
#endif
	inode = __bch2_new_inode(c);
	if (unlikely(!inode)) {
		inode = ERR_PTR(-ENOMEM);
		goto err;
	}

	bch2_inode_init_early(c, &inode_u);

	if (!(flags & BCH_CREATE_TMPFILE))
		mutex_lock(&dir->ei_update_lock);

	trans = bch2_trans_get(c);
retry:
	bch2_trans_begin(trans);

	kuid = mapped_fsuid(idmap, i_user_ns(&dir->v));
	kgid = mapped_fsgid(idmap, i_user_ns(&dir->v));
	ret   = bch2_subvol_is_ro_trans(trans, dir->ei_inum.subvol) ?:
		bch2_create_trans(trans,
				  inode_inum(dir), &dir_u, &inode_u,
				  !(flags & BCH_CREATE_TMPFILE)
				  ? &dentry->d_name : NULL,
				  from_kuid(i_user_ns(&dir->v), kuid),
				  from_kgid(i_user_ns(&dir->v), kgid),
				  mode, rdev,
				  default_acl, acl, snapshot_src, flags) ?:
		bch2_quota_acct(c, bch_qid(&inode_u), Q_INO, 1,
				KEY_TYPE_QUOTA_PREALLOC);
	if (unlikely(ret))
		goto err_before_quota;

	inum.subvol = inode_u.bi_subvol ?: dir->ei_inum.subvol;
	inum.inum = inode_u.bi_inum;

	ret   = bch2_subvolume_get(trans, inum.subvol, true,
				   BTREE_ITER_with_updates, &subvol) ?:
		bch2_trans_commit(trans, NULL, &journal_seq, 0);
	if (unlikely(ret)) {
		bch2_quota_acct(c, bch_qid(&inode_u), Q_INO, -1,
				KEY_TYPE_QUOTA_WARN);
err_before_quota:
		if (bch2_err_matches(ret, BCH_ERR_transaction_restart))
			goto retry;
		goto err_trans;
	}

	if (!(flags & BCH_CREATE_TMPFILE)) {
		bch2_inode_update_after_write(trans, dir, &dir_u,
					      ATTR_MTIME|ATTR_CTIME);
		mutex_unlock(&dir->ei_update_lock);
	}

	bch2_vfs_inode_init(trans, inum, inode, &inode_u, &subvol);

	set_cached_acl(&inode->v, ACL_TYPE_ACCESS, acl);
	set_cached_acl(&inode->v, ACL_TYPE_DEFAULT, default_acl);

	/*
	 * we must insert the new inode into the inode cache before calling
	 * bch2_trans_exit() and dropping locks, else we could race with another
	 * thread pulling the inode in and modifying it:
	 *
	 * also, calling bch2_inode_hash_insert() without passing in the
	 * transaction object is sketchy - if we could ever end up in
	 * __wait_on_freeing_inode(), we'd risk deadlock.
	 *
	 * But that shouldn't be possible, since we still have the inode locked
	 * that we just created, and we _really_ can't take a transaction
	 * restart here.
	 */
	inode = bch2_inode_hash_insert(c, NULL, inode);
	bch2_trans_put(trans);
err:
	posix_acl_release(default_acl);
	posix_acl_release(acl);
	return inode;
err_trans:
	if (!(flags & BCH_CREATE_TMPFILE))
		mutex_unlock(&dir->ei_update_lock);

	bch2_trans_put(trans);
	make_bad_inode(&inode->v);
	iput(&inode->v);
	inode = ERR_PTR(ret);
	goto err;
}

/* methods */

static struct bch_inode_info *bch2_lookup_trans(struct btree_trans *trans,
			subvol_inum dir, struct bch_hash_info *dir_hash_info,
			const struct qstr *name)
{
	struct bch_fs *c = trans->c;
	struct btree_iter dirent_iter = {};
	subvol_inum inum = {};
	struct printbuf buf = PRINTBUF;

	struct bkey_s_c k = bch2_hash_lookup(trans, &dirent_iter, bch2_dirent_hash_desc,
					     dir_hash_info, dir, name, 0);
	int ret = bkey_err(k);
	if (ret)
		return ERR_PTR(ret);

	ret = bch2_dirent_read_target(trans, dir, bkey_s_c_to_dirent(k), &inum);
	if (ret > 0)
		ret = -ENOENT;
	if (ret)
		goto err;

	struct bch_inode_info *inode = bch2_inode_hash_find(c, trans, inum);
	if (inode)
		goto out;

	struct bch_subvolume subvol;
	struct bch_inode_unpacked inode_u;
	ret =   bch2_subvolume_get(trans, inum.subvol, true, 0, &subvol) ?:
		bch2_inode_find_by_inum_nowarn_trans(trans, inum, &inode_u) ?:
		PTR_ERR_OR_ZERO(inode = bch2_inode_hash_init_insert(trans, inum, &inode_u, &subvol));

	bch2_fs_inconsistent_on(bch2_err_matches(ret, ENOENT),
				c, "dirent to missing inode:\n  %s",
				(bch2_bkey_val_to_text(&buf, c, k), buf.buf));
	if (ret)
		goto err;

	/* regular files may have hardlinks: */
	if (bch2_fs_inconsistent_on(bch2_inode_should_have_bp(&inode_u) &&
				    !bkey_eq(k.k->p, POS(inode_u.bi_dir, inode_u.bi_dir_offset)),
				    c,
				    "dirent points to inode that does not point back:\n  %s",
				    (bch2_bkey_val_to_text(&buf, c, k),
				     prt_printf(&buf, "\n  "),
				     bch2_inode_unpacked_to_text(&buf, &inode_u),
				     buf.buf))) {
		ret = -ENOENT;
		goto err;
	}
out:
	bch2_trans_iter_exit(trans, &dirent_iter);
	printbuf_exit(&buf);
	return inode;
err:
	inode = ERR_PTR(ret);
	goto out;
}

static struct dentry *bch2_lookup(struct inode *vdir, struct dentry *dentry,
				  unsigned int flags)
{
	struct bch_fs *c = vdir->i_sb->s_fs_info;
	struct bch_inode_info *dir = to_bch_ei(vdir);
	struct bch_hash_info hash = bch2_hash_info_init(c, &dir->ei_inode);

	struct bch_inode_info *inode;
	bch2_trans_do(c, NULL, NULL, 0,
		PTR_ERR_OR_ZERO(inode = bch2_lookup_trans(trans, inode_inum(dir),
							  &hash, &dentry->d_name)));
	if (IS_ERR(inode))
		inode = NULL;

	return d_splice_alias(&inode->v, dentry);
}

static int bch2_mknod(struct mnt_idmap *idmap,
		      struct inode *vdir, struct dentry *dentry,
		      umode_t mode, dev_t rdev)
{
	struct bch_inode_info *inode =
		__bch2_create(idmap, to_bch_ei(vdir), dentry, mode, rdev,
			      (subvol_inum) { 0 }, 0);

	if (IS_ERR(inode))
		return bch2_err_class(PTR_ERR(inode));

	d_instantiate(dentry, &inode->v);
	return 0;
}

static int bch2_create(struct mnt_idmap *idmap,
		       struct inode *vdir, struct dentry *dentry,
		       umode_t mode, bool excl)
{
	return bch2_mknod(idmap, vdir, dentry, mode|S_IFREG, 0);
}

static int __bch2_link(struct bch_fs *c,
		       struct bch_inode_info *inode,
		       struct bch_inode_info *dir,
		       struct dentry *dentry)
{
	struct bch_inode_unpacked dir_u, inode_u;
	int ret;

	mutex_lock(&inode->ei_update_lock);
	struct btree_trans *trans = bch2_trans_get(c);

	ret = commit_do(trans, NULL, NULL, 0,
			bch2_link_trans(trans,
					inode_inum(dir),   &dir_u,
					inode_inum(inode), &inode_u,
					&dentry->d_name));

	if (likely(!ret)) {
		bch2_inode_update_after_write(trans, dir, &dir_u,
					      ATTR_MTIME|ATTR_CTIME);
		bch2_inode_update_after_write(trans, inode, &inode_u, ATTR_CTIME);
	}

	bch2_trans_put(trans);
	mutex_unlock(&inode->ei_update_lock);
	return ret;
}

static int bch2_link(struct dentry *old_dentry, struct inode *vdir,
		     struct dentry *dentry)
{
	struct bch_fs *c = vdir->i_sb->s_fs_info;
	struct bch_inode_info *dir = to_bch_ei(vdir);
	struct bch_inode_info *inode = to_bch_ei(old_dentry->d_inode);
	int ret;

	lockdep_assert_held(&inode->v.i_rwsem);

	ret   = bch2_subvol_is_ro(c, dir->ei_inum.subvol) ?:
		bch2_subvol_is_ro(c, inode->ei_inum.subvol) ?:
		__bch2_link(c, inode, dir, dentry);
	if (unlikely(ret))
		return bch2_err_class(ret);

	ihold(&inode->v);
	d_instantiate(dentry, &inode->v);
	return 0;
}

int __bch2_unlink(struct inode *vdir, struct dentry *dentry,
		  bool deleting_snapshot)
{
	struct bch_fs *c = vdir->i_sb->s_fs_info;
	struct bch_inode_info *dir = to_bch_ei(vdir);
	struct bch_inode_info *inode = to_bch_ei(dentry->d_inode);
	struct bch_inode_unpacked dir_u, inode_u;
	int ret;

	bch2_lock_inodes(INODE_UPDATE_LOCK, dir, inode);

	struct btree_trans *trans = bch2_trans_get(c);

	ret = commit_do(trans, NULL, NULL,
			BCH_TRANS_COMMIT_no_enospc,
		bch2_unlink_trans(trans,
				  inode_inum(dir), &dir_u,
				  &inode_u, &dentry->d_name,
				  deleting_snapshot));
	if (unlikely(ret))
		goto err;

	bch2_inode_update_after_write(trans, dir, &dir_u,
				      ATTR_MTIME|ATTR_CTIME);
	bch2_inode_update_after_write(trans, inode, &inode_u,
				      ATTR_MTIME);

	if (inode_u.bi_subvol) {
		/*
		 * Subvolume deletion is asynchronous, but we still want to tell
		 * the VFS that it's been deleted here:
		 */
		set_nlink(&inode->v, 0);
	}
err:
	bch2_trans_put(trans);
	bch2_unlock_inodes(INODE_UPDATE_LOCK, dir, inode);

	return ret;
}

static int bch2_unlink(struct inode *vdir, struct dentry *dentry)
{
	struct bch_inode_info *dir= to_bch_ei(vdir);
	struct bch_fs *c = dir->v.i_sb->s_fs_info;

	int ret = bch2_subvol_is_ro(c, dir->ei_inum.subvol) ?:
		__bch2_unlink(vdir, dentry, false);
	return bch2_err_class(ret);
}

static int bch2_symlink(struct mnt_idmap *idmap,
			struct inode *vdir, struct dentry *dentry,
			const char *symname)
{
	struct bch_fs *c = vdir->i_sb->s_fs_info;
	struct bch_inode_info *dir = to_bch_ei(vdir), *inode;
	int ret;

	inode = __bch2_create(idmap, dir, dentry, S_IFLNK|S_IRWXUGO, 0,
			      (subvol_inum) { 0 }, BCH_CREATE_TMPFILE);
	if (IS_ERR(inode))
		return bch2_err_class(PTR_ERR(inode));

	inode_lock(&inode->v);
	ret = page_symlink(&inode->v, symname, strlen(symname) + 1);
	inode_unlock(&inode->v);

	if (unlikely(ret))
		goto err;

	ret = filemap_write_and_wait_range(inode->v.i_mapping, 0, LLONG_MAX);
	if (unlikely(ret))
		goto err;

	ret = __bch2_link(c, inode, dir, dentry);
	if (unlikely(ret))
		goto err;

	d_instantiate(dentry, &inode->v);
	return 0;
err:
	iput(&inode->v);
	return bch2_err_class(ret);
}

static int bch2_mkdir(struct mnt_idmap *idmap,
		      struct inode *vdir, struct dentry *dentry, umode_t mode)
{
	return bch2_mknod(idmap, vdir, dentry, mode|S_IFDIR, 0);
}

static int bch2_rename2(struct mnt_idmap *idmap,
			struct inode *src_vdir, struct dentry *src_dentry,
			struct inode *dst_vdir, struct dentry *dst_dentry,
			unsigned flags)
{
	struct bch_fs *c = src_vdir->i_sb->s_fs_info;
	struct bch_inode_info *src_dir = to_bch_ei(src_vdir);
	struct bch_inode_info *dst_dir = to_bch_ei(dst_vdir);
	struct bch_inode_info *src_inode = to_bch_ei(src_dentry->d_inode);
	struct bch_inode_info *dst_inode = to_bch_ei(dst_dentry->d_inode);
	struct bch_inode_unpacked dst_dir_u, src_dir_u;
	struct bch_inode_unpacked src_inode_u, dst_inode_u, *whiteout_inode_u;
	struct btree_trans *trans;
	enum bch_rename_mode mode = flags & RENAME_EXCHANGE
		? BCH_RENAME_EXCHANGE
		: dst_dentry->d_inode
		? BCH_RENAME_OVERWRITE : BCH_RENAME;
	bool whiteout = !!(flags & RENAME_WHITEOUT);
	int ret;

	if (flags & ~(RENAME_NOREPLACE|RENAME_EXCHANGE|RENAME_WHITEOUT))
		return -EINVAL;

	if (mode == BCH_RENAME_OVERWRITE) {
		ret = filemap_write_and_wait_range(src_inode->v.i_mapping,
						   0, LLONG_MAX);
		if (ret)
			return ret;
	}

	bch2_lock_inodes(INODE_UPDATE_LOCK,
			 src_dir,
			 dst_dir,
			 src_inode,
			 dst_inode);

	trans = bch2_trans_get(c);

	ret   = bch2_subvol_is_ro_trans(trans, src_dir->ei_inum.subvol) ?:
		bch2_subvol_is_ro_trans(trans, dst_dir->ei_inum.subvol);
	if (ret)
		goto err;

	if (inode_attr_changing(dst_dir, src_inode, Inode_opt_project)) {
		ret = bch2_fs_quota_transfer(c, src_inode,
					     dst_dir->ei_qid,
					     1 << QTYP_PRJ,
					     KEY_TYPE_QUOTA_PREALLOC);
		if (ret)
			goto err;
	}

	if (mode == BCH_RENAME_EXCHANGE &&
	    inode_attr_changing(src_dir, dst_inode, Inode_opt_project)) {
		ret = bch2_fs_quota_transfer(c, dst_inode,
					     src_dir->ei_qid,
					     1 << QTYP_PRJ,
					     KEY_TYPE_QUOTA_PREALLOC);
		if (ret)
			goto err;
	}
retry:
	bch2_trans_begin(trans);

	ret = bch2_rename_trans(trans,
				inode_inum(src_dir), &src_dir_u,
				inode_inum(dst_dir), &dst_dir_u,
				&src_inode_u,
				&dst_inode_u,
				&src_dentry->d_name,
				&dst_dentry->d_name,
				mode);
	if (unlikely(ret))
		goto err_tx_restart;

	if (whiteout) {
		whiteout_inode_u = bch2_trans_kmalloc_nomemzero(trans, sizeof(*whiteout_inode_u));
		ret = PTR_ERR_OR_ZERO(whiteout_inode_u);
		if (unlikely(ret))
			goto err_tx_restart;
		bch2_inode_init_early(c, whiteout_inode_u);

		ret = bch2_create_trans(trans,
					inode_inum(src_dir), &src_dir_u,
					whiteout_inode_u,
					&src_dentry->d_name,
					from_kuid(i_user_ns(&src_dir->v), current_fsuid()),
					from_kgid(i_user_ns(&src_dir->v), current_fsgid()),
					S_IFCHR|WHITEOUT_MODE, 0,
					NULL, NULL, (subvol_inum) { 0 }, 0) ?:
		      bch2_quota_acct(c, bch_qid(whiteout_inode_u), Q_INO, 1,
				      KEY_TYPE_QUOTA_PREALLOC);
		if (unlikely(ret))
			goto err_tx_restart;
	}

	ret = bch2_trans_commit(trans, NULL, NULL, 0);
	if (unlikely(ret)) {
err_tx_restart:
		if (bch2_err_matches(ret, BCH_ERR_transaction_restart))
			goto retry;
		goto err;
	}

	BUG_ON(src_inode->v.i_ino != src_inode_u.bi_inum);
	BUG_ON(dst_inode &&
	       dst_inode->v.i_ino != dst_inode_u.bi_inum);

	bch2_inode_update_after_write(trans, src_dir, &src_dir_u,
				      ATTR_MTIME|ATTR_CTIME);

	if (src_dir != dst_dir)
		bch2_inode_update_after_write(trans, dst_dir, &dst_dir_u,
					      ATTR_MTIME|ATTR_CTIME);

	bch2_inode_update_after_write(trans, src_inode, &src_inode_u,
				      ATTR_CTIME);

	if (dst_inode)
		bch2_inode_update_after_write(trans, dst_inode, &dst_inode_u,
					      ATTR_CTIME);
err:
	bch2_trans_put(trans);

	bch2_fs_quota_transfer(c, src_inode,
			       bch_qid(&src_inode->ei_inode),
			       1 << QTYP_PRJ,
			       KEY_TYPE_QUOTA_NOCHECK);
	if (dst_inode)
		bch2_fs_quota_transfer(c, dst_inode,
				       bch_qid(&dst_inode->ei_inode),
				       1 << QTYP_PRJ,
				       KEY_TYPE_QUOTA_NOCHECK);

	bch2_unlock_inodes(INODE_UPDATE_LOCK,
			   src_dir,
			   dst_dir,
			   src_inode,
			   dst_inode);

	return bch2_err_class(ret);
}

static void bch2_setattr_copy(struct mnt_idmap *idmap,
			      struct bch_inode_info *inode,
			      struct bch_inode_unpacked *bi,
			      struct iattr *attr)
{
	struct bch_fs *c = inode->v.i_sb->s_fs_info;
	unsigned int ia_valid = attr->ia_valid;
	kuid_t kuid;
	kgid_t kgid;

	if (ia_valid & ATTR_UID) {
		kuid = from_vfsuid(idmap, i_user_ns(&inode->v), attr->ia_vfsuid);
		bi->bi_uid = from_kuid(i_user_ns(&inode->v), kuid);
	}
	if (ia_valid & ATTR_GID) {
		kgid = from_vfsgid(idmap, i_user_ns(&inode->v), attr->ia_vfsgid);
		bi->bi_gid = from_kgid(i_user_ns(&inode->v), kgid);
	}

	if (ia_valid & ATTR_SIZE)
		bi->bi_size = attr->ia_size;

	if (ia_valid & ATTR_ATIME)
		bi->bi_atime = timespec_to_bch2_time(c, attr->ia_atime);
	if (ia_valid & ATTR_MTIME)
		bi->bi_mtime = timespec_to_bch2_time(c, attr->ia_mtime);
	if (ia_valid & ATTR_CTIME)
		bi->bi_ctime = timespec_to_bch2_time(c, attr->ia_ctime);

	if (ia_valid & ATTR_MODE) {
		umode_t mode = attr->ia_mode;
		kgid_t gid = ia_valid & ATTR_GID
			? kgid
			: inode->v.i_gid;

		if (!in_group_or_capable(idmap, &inode->v,
			make_vfsgid(idmap, i_user_ns(&inode->v), gid)))
			mode &= ~S_ISGID;
		bi->bi_mode = mode;
	}
}

int bch2_setattr_nonsize(struct mnt_idmap *idmap,
			 struct bch_inode_info *inode,
			 struct iattr *attr)
{
	struct bch_fs *c = inode->v.i_sb->s_fs_info;
	struct bch_qid qid;
	struct btree_trans *trans;
	struct btree_iter inode_iter = { NULL };
	struct bch_inode_unpacked inode_u;
	struct posix_acl *acl = NULL;
	kuid_t kuid;
	kgid_t kgid;
	int ret;

	mutex_lock(&inode->ei_update_lock);

	qid = inode->ei_qid;

	if (attr->ia_valid & ATTR_UID) {
		kuid = from_vfsuid(idmap, i_user_ns(&inode->v), attr->ia_vfsuid);
		qid.q[QTYP_USR] = from_kuid(i_user_ns(&inode->v), kuid);
	}

	if (attr->ia_valid & ATTR_GID) {
		kgid = from_vfsgid(idmap, i_user_ns(&inode->v), attr->ia_vfsgid);
		qid.q[QTYP_GRP] = from_kgid(i_user_ns(&inode->v), kgid);
	}

	ret = bch2_fs_quota_transfer(c, inode, qid, ~0,
				     KEY_TYPE_QUOTA_PREALLOC);
	if (ret)
		goto err;

	trans = bch2_trans_get(c);
retry:
	bch2_trans_begin(trans);
	kfree(acl);
	acl = NULL;

	ret = bch2_inode_peek(trans, &inode_iter, &inode_u, inode_inum(inode),
			      BTREE_ITER_intent);
	if (ret)
		goto btree_err;

	bch2_setattr_copy(idmap, inode, &inode_u, attr);

	if (attr->ia_valid & ATTR_MODE) {
		ret = bch2_acl_chmod(trans, inode_inum(inode), &inode_u,
				     inode_u.bi_mode, &acl);
		if (ret)
			goto btree_err;
	}

	ret =   bch2_inode_write(trans, &inode_iter, &inode_u) ?:
		bch2_trans_commit(trans, NULL, NULL,
				  BCH_TRANS_COMMIT_no_enospc);
btree_err:
	bch2_trans_iter_exit(trans, &inode_iter);

	if (bch2_err_matches(ret, BCH_ERR_transaction_restart))
		goto retry;
	if (unlikely(ret))
		goto err_trans;

	bch2_inode_update_after_write(trans, inode, &inode_u, attr->ia_valid);

	if (acl)
		set_cached_acl(&inode->v, ACL_TYPE_ACCESS, acl);
err_trans:
	bch2_trans_put(trans);
err:
	mutex_unlock(&inode->ei_update_lock);

	return bch2_err_class(ret);
}

static int bch2_getattr(struct mnt_idmap *idmap,
			const struct path *path, struct kstat *stat,
			u32 request_mask, unsigned query_flags)
{
	struct bch_inode_info *inode = to_bch_ei(d_inode(path->dentry));
	struct bch_fs *c = inode->v.i_sb->s_fs_info;
	vfsuid_t vfsuid = i_uid_into_vfsuid(idmap, &inode->v);
	vfsgid_t vfsgid = i_gid_into_vfsgid(idmap, &inode->v);

	stat->dev	= inode->v.i_sb->s_dev;
	stat->ino	= inode->v.i_ino;
	stat->mode	= inode->v.i_mode;
	stat->nlink	= inode->v.i_nlink;
	stat->uid	= vfsuid_into_kuid(vfsuid);
	stat->gid	= vfsgid_into_kgid(vfsgid);
	stat->rdev	= inode->v.i_rdev;
	stat->size	= i_size_read(&inode->v);
	stat->atime	= inode_get_atime(&inode->v);
	stat->mtime	= inode_get_mtime(&inode->v);
	stat->ctime	= inode_get_ctime(&inode->v);
	stat->blksize	= block_bytes(c);
	stat->blocks	= inode->v.i_blocks;

	stat->subvol	= inode->ei_inum.subvol;
	stat->result_mask |= STATX_SUBVOL;

	if ((request_mask & STATX_DIOALIGN) && S_ISREG(inode->v.i_mode)) {
		stat->result_mask |= STATX_DIOALIGN;
		/*
		 * this is incorrect; we should be tracking this in superblock,
		 * and checking the alignment of open devices
		 */
		stat->dio_mem_align = SECTOR_SIZE;
		stat->dio_offset_align = block_bytes(c);
	}

	if (request_mask & STATX_BTIME) {
		stat->result_mask |= STATX_BTIME;
		stat->btime = bch2_time_to_timespec(c, inode->ei_inode.bi_otime);
	}

	if (inode->ei_inode.bi_flags & BCH_INODE_immutable)
		stat->attributes |= STATX_ATTR_IMMUTABLE;
	stat->attributes_mask	 |= STATX_ATTR_IMMUTABLE;

	if (inode->ei_inode.bi_flags & BCH_INODE_append)
		stat->attributes |= STATX_ATTR_APPEND;
	stat->attributes_mask	 |= STATX_ATTR_APPEND;

	if (inode->ei_inode.bi_flags & BCH_INODE_nodump)
		stat->attributes |= STATX_ATTR_NODUMP;
	stat->attributes_mask	 |= STATX_ATTR_NODUMP;

	return 0;
}

static int bch2_setattr(struct mnt_idmap *idmap,
			struct dentry *dentry, struct iattr *iattr)
{
	struct bch_inode_info *inode = to_bch_ei(dentry->d_inode);
	struct bch_fs *c = inode->v.i_sb->s_fs_info;
	int ret;

	lockdep_assert_held(&inode->v.i_rwsem);

	ret   = bch2_subvol_is_ro(c, inode->ei_inum.subvol) ?:
		setattr_prepare(idmap, dentry, iattr);
	if (ret)
		return ret;

	return iattr->ia_valid & ATTR_SIZE
		? bchfs_truncate(idmap, inode, iattr)
		: bch2_setattr_nonsize(idmap, inode, iattr);
}

static int bch2_tmpfile(struct mnt_idmap *idmap,
			struct inode *vdir, struct file *file, umode_t mode)
{
	struct bch_inode_info *inode =
		__bch2_create(idmap, to_bch_ei(vdir),
			      file->f_path.dentry, mode, 0,
			      (subvol_inum) { 0 }, BCH_CREATE_TMPFILE);

	if (IS_ERR(inode))
		return bch2_err_class(PTR_ERR(inode));

	d_mark_tmpfile(file, &inode->v);
	d_instantiate(file->f_path.dentry, &inode->v);
	return finish_open_simple(file, 0);
}

static int bch2_fill_extent(struct bch_fs *c,
			    struct fiemap_extent_info *info,
			    struct bkey_s_c k, unsigned flags)
{
	if (bkey_extent_is_direct_data(k.k)) {
		struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
		const union bch_extent_entry *entry;
		struct extent_ptr_decoded p;
		int ret;

		if (k.k->type == KEY_TYPE_reflink_v)
			flags |= FIEMAP_EXTENT_SHARED;

		bkey_for_each_ptr_decode(k.k, ptrs, p, entry) {
			int flags2 = 0;
			u64 offset = p.ptr.offset;

			if (p.ptr.unwritten)
				flags2 |= FIEMAP_EXTENT_UNWRITTEN;

			if (p.crc.compression_type)
				flags2 |= FIEMAP_EXTENT_ENCODED;
			else
				offset += p.crc.offset;

			if ((offset & (block_sectors(c) - 1)) ||
			    (k.k->size & (block_sectors(c) - 1)))
				flags2 |= FIEMAP_EXTENT_NOT_ALIGNED;

			ret = fiemap_fill_next_extent(info,
						bkey_start_offset(k.k) << 9,
						offset << 9,
						k.k->size << 9, flags|flags2);
			if (ret)
				return ret;
		}

		return 0;
	} else if (bkey_extent_is_inline_data(k.k)) {
		return fiemap_fill_next_extent(info,
					       bkey_start_offset(k.k) << 9,
					       0, k.k->size << 9,
					       flags|
					       FIEMAP_EXTENT_DATA_INLINE);
	} else if (k.k->type == KEY_TYPE_reservation) {
		return fiemap_fill_next_extent(info,
					       bkey_start_offset(k.k) << 9,
					       0, k.k->size << 9,
					       flags|
					       FIEMAP_EXTENT_DELALLOC|
					       FIEMAP_EXTENT_UNWRITTEN);
	} else {
		BUG();
	}
}

static int bch2_fiemap(struct inode *vinode, struct fiemap_extent_info *info,
		       u64 start, u64 len)
{
	struct bch_fs *c = vinode->i_sb->s_fs_info;
	struct bch_inode_info *ei = to_bch_ei(vinode);
	struct btree_trans *trans;
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bkey_buf cur, prev;
	unsigned offset_into_extent, sectors;
	bool have_extent = false;
	int ret = 0;

	ret = fiemap_prep(&ei->v, info, start, &len, FIEMAP_FLAG_SYNC);
	if (ret)
		return ret;

	struct bpos end = POS(ei->v.i_ino, (start + len) >> 9);
	if (start + len < start)
		return -EINVAL;

	start >>= 9;

	bch2_bkey_buf_init(&cur);
	bch2_bkey_buf_init(&prev);
	trans = bch2_trans_get(c);

	bch2_trans_iter_init(trans, &iter, BTREE_ID_extents,
			     POS(ei->v.i_ino, start), 0);

	while (true) {
		enum btree_id data_btree = BTREE_ID_extents;

		bch2_trans_begin(trans);

		u32 snapshot;
		ret = bch2_subvolume_get_snapshot(trans, ei->ei_inum.subvol, &snapshot);
		if (ret)
			goto err;

		bch2_btree_iter_set_snapshot(&iter, snapshot);

		k = bch2_btree_iter_peek_upto(&iter, end);
		ret = bkey_err(k);
		if (ret)
			goto err;

		if (!k.k)
			break;

		if (!bkey_extent_is_data(k.k) &&
		    k.k->type != KEY_TYPE_reservation) {
			bch2_btree_iter_advance(&iter);
			continue;
		}

		offset_into_extent	= iter.pos.offset -
			bkey_start_offset(k.k);
		sectors			= k.k->size - offset_into_extent;

		bch2_bkey_buf_reassemble(&cur, c, k);

		ret = bch2_read_indirect_extent(trans, &data_btree,
					&offset_into_extent, &cur);
		if (ret)
			break;

		k = bkey_i_to_s_c(cur.k);
		bch2_bkey_buf_realloc(&prev, c, k.k->u64s);

		sectors = min(sectors, k.k->size - offset_into_extent);

		bch2_cut_front(POS(k.k->p.inode,
				   bkey_start_offset(k.k) +
				   offset_into_extent),
			       cur.k);
		bch2_key_resize(&cur.k->k, sectors);
		cur.k->k.p = iter.pos;
		cur.k->k.p.offset += cur.k->k.size;

		if (have_extent) {
			bch2_trans_unlock(trans);
			ret = bch2_fill_extent(c, info,
					bkey_i_to_s_c(prev.k), 0);
			if (ret)
				break;
		}

		bkey_copy(prev.k, cur.k);
		have_extent = true;

		bch2_btree_iter_set_pos(&iter,
			POS(iter.pos.inode, iter.pos.offset + sectors));
err:
		if (ret &&
		    !bch2_err_matches(ret, BCH_ERR_transaction_restart))
			break;
	}
	bch2_trans_iter_exit(trans, &iter);

	if (!ret && have_extent) {
		bch2_trans_unlock(trans);
		ret = bch2_fill_extent(c, info, bkey_i_to_s_c(prev.k),
				       FIEMAP_EXTENT_LAST);
	}

	bch2_trans_put(trans);
	bch2_bkey_buf_exit(&cur, c);
	bch2_bkey_buf_exit(&prev, c);
	return ret < 0 ? ret : 0;
}

static const struct vm_operations_struct bch_vm_ops = {
	.fault		= bch2_page_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite   = bch2_page_mkwrite,
};

static int bch2_mmap(struct file *file, struct vm_area_struct *vma)
{
	file_accessed(file);

	vma->vm_ops = &bch_vm_ops;
	return 0;
}

/* Directories: */

static loff_t bch2_dir_llseek(struct file *file, loff_t offset, int whence)
{
	return generic_file_llseek_size(file, offset, whence,
					S64_MAX, S64_MAX);
}

static int bch2_vfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct bch_inode_info *inode = file_bch_inode(file);
	struct bch_fs *c = inode->v.i_sb->s_fs_info;

	if (!dir_emit_dots(file, ctx))
		return 0;

	int ret = bch2_readdir(c, inode_inum(inode), ctx);

	bch_err_fn(c, ret);
	return bch2_err_class(ret);
}

static int bch2_open(struct inode *vinode, struct file *file)
{
	if (file->f_flags & (O_WRONLY|O_RDWR)) {
		struct bch_inode_info *inode = to_bch_ei(vinode);
		struct bch_fs *c = inode->v.i_sb->s_fs_info;

		int ret = bch2_subvol_is_ro(c, inode->ei_inum.subvol);
		if (ret)
			return ret;
	}

	file->f_mode |= FMODE_CAN_ODIRECT;

	return generic_file_open(vinode, file);
}

static const struct file_operations bch_file_operations = {
	.open		= bch2_open,
	.llseek		= bch2_llseek,
	.read_iter	= bch2_read_iter,
	.write_iter	= bch2_write_iter,
	.mmap		= bch2_mmap,
	.get_unmapped_area = thp_get_unmapped_area,
	.fsync		= bch2_fsync,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.fallocate	= bch2_fallocate_dispatch,
	.unlocked_ioctl = bch2_fs_file_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= bch2_compat_fs_ioctl,
#endif
	.remap_file_range = bch2_remap_file_range,
};

static const struct inode_operations bch_file_inode_operations = {
	.getattr	= bch2_getattr,
	.setattr	= bch2_setattr,
	.fiemap		= bch2_fiemap,
	.listxattr	= bch2_xattr_list,
#ifdef CONFIG_BCACHEFS_POSIX_ACL
	.get_inode_acl	= bch2_get_acl,
	.set_acl	= bch2_set_acl,
#endif
};

static const struct inode_operations bch_dir_inode_operations = {
	.lookup		= bch2_lookup,
	.create		= bch2_create,
	.link		= bch2_link,
	.unlink		= bch2_unlink,
	.symlink	= bch2_symlink,
	.mkdir		= bch2_mkdir,
	.rmdir		= bch2_unlink,
	.mknod		= bch2_mknod,
	.rename		= bch2_rename2,
	.getattr	= bch2_getattr,
	.setattr	= bch2_setattr,
	.tmpfile	= bch2_tmpfile,
	.listxattr	= bch2_xattr_list,
#ifdef CONFIG_BCACHEFS_POSIX_ACL
	.get_inode_acl	= bch2_get_acl,
	.set_acl	= bch2_set_acl,
#endif
};

static const struct file_operations bch_dir_file_operations = {
	.llseek		= bch2_dir_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= bch2_vfs_readdir,
	.fsync		= bch2_fsync,
	.unlocked_ioctl = bch2_fs_file_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= bch2_compat_fs_ioctl,
#endif
};

static const struct inode_operations bch_symlink_inode_operations = {
	.get_link	= page_get_link,
	.getattr	= bch2_getattr,
	.setattr	= bch2_setattr,
	.listxattr	= bch2_xattr_list,
#ifdef CONFIG_BCACHEFS_POSIX_ACL
	.get_inode_acl	= bch2_get_acl,
	.set_acl	= bch2_set_acl,
#endif
};

static const struct inode_operations bch_special_inode_operations = {
	.getattr	= bch2_getattr,
	.setattr	= bch2_setattr,
	.listxattr	= bch2_xattr_list,
#ifdef CONFIG_BCACHEFS_POSIX_ACL
	.get_inode_acl	= bch2_get_acl,
	.set_acl	= bch2_set_acl,
#endif
};

static const struct address_space_operations bch_address_space_operations = {
	.read_folio	= bch2_read_folio,
	.writepages	= bch2_writepages,
	.readahead	= bch2_readahead,
	.dirty_folio	= filemap_dirty_folio,
	.write_begin	= bch2_write_begin,
	.write_end	= bch2_write_end,
	.invalidate_folio = bch2_invalidate_folio,
	.release_folio	= bch2_release_folio,
#ifdef CONFIG_MIGRATION
	.migrate_folio	= filemap_migrate_folio,
#endif
	.error_remove_folio = generic_error_remove_folio,
};

struct bcachefs_fid {
	u64		inum;
	u32		subvol;
	u32		gen;
} __packed;

struct bcachefs_fid_with_parent {
	struct bcachefs_fid	fid;
	struct bcachefs_fid	dir;
} __packed;

static int bcachefs_fid_valid(int fh_len, int fh_type)
{
	switch (fh_type) {
	case FILEID_BCACHEFS_WITHOUT_PARENT:
		return fh_len == sizeof(struct bcachefs_fid) / sizeof(u32);
	case FILEID_BCACHEFS_WITH_PARENT:
		return fh_len == sizeof(struct bcachefs_fid_with_parent) / sizeof(u32);
	default:
		return false;
	}
}

static struct bcachefs_fid bch2_inode_to_fid(struct bch_inode_info *inode)
{
	return (struct bcachefs_fid) {
		.inum	= inode->ei_inum.inum,
		.subvol	= inode->ei_inum.subvol,
		.gen	= inode->ei_inode.bi_generation,
	};
}

static int bch2_encode_fh(struct inode *vinode, u32 *fh, int *len,
			  struct inode *vdir)
{
	struct bch_inode_info *inode	= to_bch_ei(vinode);
	struct bch_inode_info *dir	= to_bch_ei(vdir);
	int min_len;

	if (!S_ISDIR(inode->v.i_mode) && dir) {
		struct bcachefs_fid_with_parent *fid = (void *) fh;

		min_len = sizeof(*fid) / sizeof(u32);
		if (*len < min_len) {
			*len = min_len;
			return FILEID_INVALID;
		}

		fid->fid = bch2_inode_to_fid(inode);
		fid->dir = bch2_inode_to_fid(dir);

		*len = min_len;
		return FILEID_BCACHEFS_WITH_PARENT;
	} else {
		struct bcachefs_fid *fid = (void *) fh;

		min_len = sizeof(*fid) / sizeof(u32);
		if (*len < min_len) {
			*len = min_len;
			return FILEID_INVALID;
		}
		*fid = bch2_inode_to_fid(inode);

		*len = min_len;
		return FILEID_BCACHEFS_WITHOUT_PARENT;
	}
}

static struct inode *bch2_nfs_get_inode(struct super_block *sb,
					struct bcachefs_fid fid)
{
	struct bch_fs *c = sb->s_fs_info;
	struct inode *vinode = bch2_vfs_inode_get(c, (subvol_inum) {
				    .subvol = fid.subvol,
				    .inum = fid.inum,
	});
	if (!IS_ERR(vinode) && vinode->i_generation != fid.gen) {
		iput(vinode);
		vinode = ERR_PTR(-ESTALE);
	}
	return vinode;
}

static struct dentry *bch2_fh_to_dentry(struct super_block *sb, struct fid *_fid,
		int fh_len, int fh_type)
{
	struct bcachefs_fid *fid = (void *) _fid;

	if (!bcachefs_fid_valid(fh_len, fh_type))
		return NULL;

	return d_obtain_alias(bch2_nfs_get_inode(sb, *fid));
}

static struct dentry *bch2_fh_to_parent(struct super_block *sb, struct fid *_fid,
		int fh_len, int fh_type)
{
	struct bcachefs_fid_with_parent *fid = (void *) _fid;

	if (!bcachefs_fid_valid(fh_len, fh_type) ||
	    fh_type != FILEID_BCACHEFS_WITH_PARENT)
		return NULL;

	return d_obtain_alias(bch2_nfs_get_inode(sb, fid->dir));
}

static struct dentry *bch2_get_parent(struct dentry *child)
{
	struct bch_inode_info *inode = to_bch_ei(child->d_inode);
	struct bch_fs *c = inode->v.i_sb->s_fs_info;
	subvol_inum parent_inum = {
		.subvol = inode->ei_inode.bi_parent_subvol ?:
			inode->ei_inum.subvol,
		.inum = inode->ei_inode.bi_dir,
	};

	return d_obtain_alias(bch2_vfs_inode_get(c, parent_inum));
}

static int bch2_get_name(struct dentry *parent, char *name, struct dentry *child)
{
	struct bch_inode_info *inode	= to_bch_ei(child->d_inode);
	struct bch_inode_info *dir	= to_bch_ei(parent->d_inode);
	struct bch_fs *c = inode->v.i_sb->s_fs_info;
	struct btree_trans *trans;
	struct btree_iter iter1;
	struct btree_iter iter2;
	struct bkey_s_c k;
	struct bkey_s_c_dirent d;
	struct bch_inode_unpacked inode_u;
	subvol_inum target;
	u32 snapshot;
	struct qstr dirent_name;
	unsigned name_len = 0;
	int ret;

	if (!S_ISDIR(dir->v.i_mode))
		return -EINVAL;

	trans = bch2_trans_get(c);

	bch2_trans_iter_init(trans, &iter1, BTREE_ID_dirents,
			     POS(dir->ei_inode.bi_inum, 0), 0);
	bch2_trans_iter_init(trans, &iter2, BTREE_ID_dirents,
			     POS(dir->ei_inode.bi_inum, 0), 0);
retry:
	bch2_trans_begin(trans);

	ret = bch2_subvolume_get_snapshot(trans, dir->ei_inum.subvol, &snapshot);
	if (ret)
		goto err;

	bch2_btree_iter_set_snapshot(&iter1, snapshot);
	bch2_btree_iter_set_snapshot(&iter2, snapshot);

	ret = bch2_inode_find_by_inum_trans(trans, inode_inum(inode), &inode_u);
	if (ret)
		goto err;

	if (inode_u.bi_dir == dir->ei_inode.bi_inum) {
		bch2_btree_iter_set_pos(&iter1, POS(inode_u.bi_dir, inode_u.bi_dir_offset));

		k = bch2_btree_iter_peek_slot(&iter1);
		ret = bkey_err(k);
		if (ret)
			goto err;

		if (k.k->type != KEY_TYPE_dirent) {
			ret = -BCH_ERR_ENOENT_dirent_doesnt_match_inode;
			goto err;
		}

		d = bkey_s_c_to_dirent(k);
		ret = bch2_dirent_read_target(trans, inode_inum(dir), d, &target);
		if (ret > 0)
			ret = -BCH_ERR_ENOENT_dirent_doesnt_match_inode;
		if (ret)
			goto err;

		if (subvol_inum_eq(target, inode->ei_inum))
			goto found;
	} else {
		/*
		 * File with multiple hardlinks and our backref is to the wrong
		 * directory - linear search:
		 */
		for_each_btree_key_continue_norestart(iter2, 0, k, ret) {
			if (k.k->p.inode > dir->ei_inode.bi_inum)
				break;

			if (k.k->type != KEY_TYPE_dirent)
				continue;

			d = bkey_s_c_to_dirent(k);
			ret = bch2_dirent_read_target(trans, inode_inum(dir), d, &target);
			if (ret < 0)
				break;
			if (ret)
				continue;

			if (subvol_inum_eq(target, inode->ei_inum))
				goto found;
		}
	}

	ret = -ENOENT;
	goto err;
found:
	dirent_name = bch2_dirent_get_name(d);

	name_len = min_t(unsigned, dirent_name.len, NAME_MAX);
	memcpy(name, dirent_name.name, name_len);
	name[name_len] = '\0';
err:
	if (bch2_err_matches(ret, BCH_ERR_transaction_restart))
		goto retry;

	bch2_trans_iter_exit(trans, &iter1);
	bch2_trans_iter_exit(trans, &iter2);
	bch2_trans_put(trans);

	return ret;
}

static const struct export_operations bch_export_ops = {
	.encode_fh	= bch2_encode_fh,
	.fh_to_dentry	= bch2_fh_to_dentry,
	.fh_to_parent	= bch2_fh_to_parent,
	.get_parent	= bch2_get_parent,
	.get_name	= bch2_get_name,
};

static void bch2_vfs_inode_init(struct btree_trans *trans,
				subvol_inum inum,
				struct bch_inode_info *inode,
				struct bch_inode_unpacked *bi,
				struct bch_subvolume *subvol)
{
	inode->v.i_ino		= inum.inum;
	inode->ei_inum		= inum;
	inode->ei_inode.bi_inum	= inum.inum;
	bch2_inode_update_after_write(trans, inode, bi, ~0);

	inode->v.i_blocks	= bi->bi_sectors;
	inode->v.i_ino		= bi->bi_inum;
	inode->v.i_rdev		= bi->bi_dev;
	inode->v.i_generation	= bi->bi_generation;
	inode->v.i_size		= bi->bi_size;

	inode->ei_flags		= 0;
	inode->ei_quota_reserved = 0;
	inode->ei_qid		= bch_qid(bi);

	if (BCH_SUBVOLUME_SNAP(subvol))
		set_bit(EI_INODE_SNAPSHOT, &inode->ei_flags);

	inode->v.i_mapping->a_ops = &bch_address_space_operations;

	switch (inode->v.i_mode & S_IFMT) {
	case S_IFREG:
		inode->v.i_op	= &bch_file_inode_operations;
		inode->v.i_fop	= &bch_file_operations;
		break;
	case S_IFDIR:
		inode->v.i_op	= &bch_dir_inode_operations;
		inode->v.i_fop	= &bch_dir_file_operations;
		break;
	case S_IFLNK:
		inode_nohighmem(&inode->v);
		inode->v.i_op	= &bch_symlink_inode_operations;
		break;
	default:
		init_special_inode(&inode->v, inode->v.i_mode, inode->v.i_rdev);
		inode->v.i_op	= &bch_special_inode_operations;
		break;
	}

	mapping_set_large_folios(inode->v.i_mapping);
}

static void bch2_free_inode(struct inode *vinode)
{
	kmem_cache_free(bch2_inode_cache, to_bch_ei(vinode));
}

static int inode_update_times_fn(struct btree_trans *trans,
				 struct bch_inode_info *inode,
				 struct bch_inode_unpacked *bi,
				 void *p)
{
	struct bch_fs *c = inode->v.i_sb->s_fs_info;

	bi->bi_atime	= timespec_to_bch2_time(c, inode_get_atime(&inode->v));
	bi->bi_mtime	= timespec_to_bch2_time(c, inode_get_mtime(&inode->v));
	bi->bi_ctime	= timespec_to_bch2_time(c, inode_get_ctime(&inode->v));

	return 0;
}

static int bch2_vfs_write_inode(struct inode *vinode,
				struct writeback_control *wbc)
{
	struct bch_fs *c = vinode->i_sb->s_fs_info;
	struct bch_inode_info *inode = to_bch_ei(vinode);
	int ret;

	mutex_lock(&inode->ei_update_lock);
	ret = bch2_write_inode(c, inode, inode_update_times_fn, NULL,
			       ATTR_ATIME|ATTR_MTIME|ATTR_CTIME);
	mutex_unlock(&inode->ei_update_lock);

	return bch2_err_class(ret);
}

static void bch2_evict_inode(struct inode *vinode)
{
	struct bch_fs *c = vinode->i_sb->s_fs_info;
	struct bch_inode_info *inode = to_bch_ei(vinode);
	bool delete = !inode->v.i_nlink && !is_bad_inode(&inode->v);

	/*
	 * evict() has waited for outstanding writeback, we'll do no more IO
	 * through this inode: it's safe to remove from VFS inode hashtable here
	 *
	 * Do that now so that other threads aren't blocked from pulling it back
	 * in, there's no reason for them to be:
	 */
	if (!delete)
		bch2_inode_hash_remove(c, inode);

	truncate_inode_pages_final(&inode->v.i_data);

	clear_inode(&inode->v);

	BUG_ON(!is_bad_inode(&inode->v) && inode->ei_quota_reserved);

	if (delete) {
		bch2_quota_acct(c, inode->ei_qid, Q_SPC, -((s64) inode->v.i_blocks),
				KEY_TYPE_QUOTA_WARN);
		bch2_quota_acct(c, inode->ei_qid, Q_INO, -1,
				KEY_TYPE_QUOTA_WARN);
		bch2_inode_rm(c, inode_inum(inode));

		/*
		 * If we are deleting, we need it present in the vfs hash table
		 * so that fsck can check if unlinked inodes are still open:
		 */
		bch2_inode_hash_remove(c, inode);
	}

	mutex_lock(&c->vfs_inodes_lock);
	list_del_init(&inode->ei_vfs_inode_list);
	mutex_unlock(&c->vfs_inodes_lock);
}

void bch2_evict_subvolume_inodes(struct bch_fs *c, snapshot_id_list *s)
{
	struct bch_inode_info *inode;
	DARRAY(struct bch_inode_info *) grabbed;
	bool clean_pass = false, this_pass_clean;

	/*
	 * Initially, we scan for inodes without I_DONTCACHE, then mark them to
	 * be pruned with d_mark_dontcache().
	 *
	 * Once we've had a clean pass where we didn't find any inodes without
	 * I_DONTCACHE, we wait for them to be freed:
	 */

	darray_init(&grabbed);
	darray_make_room(&grabbed, 1024);
again:
	cond_resched();
	this_pass_clean = true;

	mutex_lock(&c->vfs_inodes_lock);
	list_for_each_entry(inode, &c->vfs_inodes_list, ei_vfs_inode_list) {
		if (!snapshot_list_has_id(s, inode->ei_inum.subvol))
			continue;

		if (!(inode->v.i_state & I_DONTCACHE) &&
		    !(inode->v.i_state & I_FREEING) &&
		    igrab(&inode->v)) {
			this_pass_clean = false;

			if (darray_push_gfp(&grabbed, inode, GFP_ATOMIC|__GFP_NOWARN)) {
				iput(&inode->v);
				break;
			}
		} else if (clean_pass && this_pass_clean) {
			wait_queue_head_t *wq = bit_waitqueue(&inode->v.i_state, __I_NEW);
			DEFINE_WAIT_BIT(wait, &inode->v.i_state, __I_NEW);

			prepare_to_wait(wq, &wait.wq_entry, TASK_UNINTERRUPTIBLE);
			mutex_unlock(&c->vfs_inodes_lock);

			schedule();
			finish_wait(wq, &wait.wq_entry);
			goto again;
		}
	}
	mutex_unlock(&c->vfs_inodes_lock);

	darray_for_each(grabbed, i) {
		inode = *i;
		d_mark_dontcache(&inode->v);
		d_prune_aliases(&inode->v);
		iput(&inode->v);
	}
	grabbed.nr = 0;

	if (!clean_pass || !this_pass_clean) {
		clean_pass = this_pass_clean;
		goto again;
	}

	darray_exit(&grabbed);
}

static int bch2_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct bch_fs *c = sb->s_fs_info;
	struct bch_fs_usage_short usage = bch2_fs_usage_read_short(c);
	unsigned shift = sb->s_blocksize_bits - 9;
	/*
	 * this assumes inodes take up 64 bytes, which is a decent average
	 * number:
	 */
	u64 avail_inodes = ((usage.capacity - usage.used) << 3);

	buf->f_type	= BCACHEFS_STATFS_MAGIC;
	buf->f_bsize	= sb->s_blocksize;
	buf->f_blocks	= usage.capacity >> shift;
	buf->f_bfree	= usage.free >> shift;
	buf->f_bavail	= avail_factor(usage.free) >> shift;

	buf->f_files	= usage.nr_inodes + avail_inodes;
	buf->f_ffree	= avail_inodes;

	buf->f_fsid	= uuid_to_fsid(c->sb.user_uuid.b);
	buf->f_namelen	= BCH_NAME_MAX;

	return 0;
}

static int bch2_sync_fs(struct super_block *sb, int wait)
{
	struct bch_fs *c = sb->s_fs_info;
	int ret;

	trace_bch2_sync_fs(sb, wait);

	if (c->opts.journal_flush_disabled)
		return 0;

	if (!wait) {
		bch2_journal_flush_async(&c->journal, NULL);
		return 0;
	}

	ret = bch2_journal_flush(&c->journal);
	return bch2_err_class(ret);
}

static struct bch_fs *bch2_path_to_fs(const char *path)
{
	struct bch_fs *c;
	dev_t dev;
	int ret;

	ret = lookup_bdev(path, &dev);
	if (ret)
		return ERR_PTR(ret);

	c = bch2_dev_to_fs(dev);
	if (c)
		closure_put(&c->cl);
	return c ?: ERR_PTR(-ENOENT);
}

static int bch2_remount(struct super_block *sb, int *flags,
			struct bch_opts opts)
{
	struct bch_fs *c = sb->s_fs_info;
	int ret = 0;

	opt_set(opts, read_only, (*flags & SB_RDONLY) != 0);

	if (opts.read_only != c->opts.read_only) {
		down_write(&c->state_lock);

		if (opts.read_only) {
			bch2_fs_read_only(c);

			sb->s_flags |= SB_RDONLY;
		} else {
			ret = bch2_fs_read_write(c);
			if (ret) {
				bch_err(c, "error going rw: %i", ret);
				up_write(&c->state_lock);
				ret = -EINVAL;
				goto err;
			}

			sb->s_flags &= ~SB_RDONLY;
		}

		c->opts.read_only = opts.read_only;

		up_write(&c->state_lock);
	}

	if (opt_defined(opts, errors))
		c->opts.errors = opts.errors;
err:
	return bch2_err_class(ret);
}

static int bch2_show_devname(struct seq_file *seq, struct dentry *root)
{
	struct bch_fs *c = root->d_sb->s_fs_info;
	bool first = true;

	for_each_online_member(c, ca) {
		if (!first)
			seq_putc(seq, ':');
		first = false;
		seq_puts(seq, ca->disk_sb.sb_name);
	}

	return 0;
}

static int bch2_show_options(struct seq_file *seq, struct dentry *root)
{
	struct bch_fs *c = root->d_sb->s_fs_info;
	struct printbuf buf = PRINTBUF;

	bch2_opts_to_text(&buf, c->opts, c, c->disk_sb.sb,
			  OPT_MOUNT, OPT_HIDDEN, OPT_SHOW_MOUNT_STYLE);
	printbuf_nul_terminate(&buf);
	seq_puts(seq, buf.buf);

	int ret = buf.allocation_failure ? -ENOMEM : 0;
	printbuf_exit(&buf);
	return ret;
}

static void bch2_put_super(struct super_block *sb)
{
	struct bch_fs *c = sb->s_fs_info;

	__bch2_fs_stop(c);
}

/*
 * bcachefs doesn't currently integrate intwrite freeze protection but the
 * internal write references serve the same purpose. Therefore reuse the
 * read-only transition code to perform the quiesce. The caveat is that we don't
 * currently have the ability to block tasks that want a write reference while
 * the superblock is frozen. This is fine for now, but we should either add
 * blocking support or find a way to integrate sb_start_intwrite() and friends.
 */
static int bch2_freeze(struct super_block *sb)
{
	struct bch_fs *c = sb->s_fs_info;

	down_write(&c->state_lock);
	bch2_fs_read_only(c);
	up_write(&c->state_lock);
	return 0;
}

static int bch2_unfreeze(struct super_block *sb)
{
	struct bch_fs *c = sb->s_fs_info;
	int ret;

	if (test_bit(BCH_FS_emergency_ro, &c->flags))
		return 0;

	down_write(&c->state_lock);
	ret = bch2_fs_read_write(c);
	up_write(&c->state_lock);
	return ret;
}

static const struct super_operations bch_super_operations = {
	.alloc_inode	= bch2_alloc_inode,
	.free_inode	= bch2_free_inode,
	.write_inode	= bch2_vfs_write_inode,
	.evict_inode	= bch2_evict_inode,
	.sync_fs	= bch2_sync_fs,
	.statfs		= bch2_statfs,
	.show_devname	= bch2_show_devname,
	.show_options	= bch2_show_options,
	.put_super	= bch2_put_super,
	.freeze_fs	= bch2_freeze,
	.unfreeze_fs	= bch2_unfreeze,
};

static int bch2_set_super(struct super_block *s, void *data)
{
	s->s_fs_info = data;
	return 0;
}

static int bch2_noset_super(struct super_block *s, void *data)
{
	return -EBUSY;
}

typedef DARRAY(struct bch_fs *) darray_fs;

static int bch2_test_super(struct super_block *s, void *data)
{
	struct bch_fs *c = s->s_fs_info;
	darray_fs *d = data;

	if (!c)
		return false;

	darray_for_each(*d, i)
		if (c != *i)
			return false;
	return true;
}

static int bch2_fs_get_tree(struct fs_context *fc)
{
	struct bch_fs *c;
	struct super_block *sb;
	struct inode *vinode;
	struct bch2_opts_parse *opts_parse = fc->fs_private;
	struct bch_opts opts = opts_parse->opts;
	darray_str devs;
	darray_fs devs_to_fs = {};
	int ret;

	opt_set(opts, read_only, (fc->sb_flags & SB_RDONLY) != 0);
	opt_set(opts, nostart, true);

	if (!fc->source || strlen(fc->source) == 0)
		return -EINVAL;

	ret = bch2_split_devs(fc->source, &devs);
	if (ret)
		return ret;

	darray_for_each(devs, i) {
		ret = darray_push(&devs_to_fs, bch2_path_to_fs(*i));
		if (ret)
			goto err;
	}

	sb = sget(fc->fs_type, bch2_test_super, bch2_noset_super, fc->sb_flags|SB_NOSEC, &devs_to_fs);
	if (!IS_ERR(sb))
		goto got_sb;

	c = bch2_fs_open(devs.data, devs.nr, opts);
	ret = PTR_ERR_OR_ZERO(c);
	if (ret)
		goto err;

	/* Some options can't be parsed until after the fs is started: */
	opts = bch2_opts_empty();
	ret = bch2_parse_mount_opts(c, &opts, NULL, opts_parse->parse_later.buf);
	if (ret)
		goto err_stop_fs;

	bch2_opts_apply(&c->opts, opts);

	ret = bch2_fs_start(c);
	if (ret)
		goto err_stop_fs;

	sb = sget(fc->fs_type, NULL, bch2_set_super, fc->sb_flags|SB_NOSEC, c);
	ret = PTR_ERR_OR_ZERO(sb);
	if (ret)
		goto err_stop_fs;
got_sb:
	c = sb->s_fs_info;

	if (sb->s_root) {
		if ((fc->sb_flags ^ sb->s_flags) & SB_RDONLY) {
			ret = -EBUSY;
			goto err_put_super;
		}
		goto out;
	}

	sb->s_blocksize		= block_bytes(c);
	sb->s_blocksize_bits	= ilog2(block_bytes(c));
	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_op		= &bch_super_operations;
	sb->s_export_op		= &bch_export_ops;
#ifdef CONFIG_BCACHEFS_QUOTA
	sb->s_qcop		= &bch2_quotactl_operations;
	sb->s_quota_types	= QTYPE_MASK_USR|QTYPE_MASK_GRP|QTYPE_MASK_PRJ;
#endif
	sb->s_xattr		= bch2_xattr_handlers;
	sb->s_magic		= BCACHEFS_STATFS_MAGIC;
	sb->s_time_gran		= c->sb.nsec_per_time_unit;
	sb->s_time_min		= div_s64(S64_MIN, c->sb.time_units_per_sec) + 1;
	sb->s_time_max		= div_s64(S64_MAX, c->sb.time_units_per_sec);
	sb->s_uuid		= c->sb.user_uuid;
	sb->s_shrink->seeks	= 0;
	c->vfs_sb		= sb;
	strscpy(sb->s_id, c->name, sizeof(sb->s_id));

	ret = super_setup_bdi(sb);
	if (ret)
		goto err_put_super;

	sb->s_bdi->ra_pages		= VM_READAHEAD_PAGES;

	for_each_online_member(c, ca) {
		struct block_device *bdev = ca->disk_sb.bdev;

		/* XXX: create an anonymous device for multi device filesystems */
		sb->s_bdev	= bdev;
		sb->s_dev	= bdev->bd_dev;
		percpu_ref_put(&ca->io_ref);
		break;
	}

	c->dev = sb->s_dev;

#ifdef CONFIG_BCACHEFS_POSIX_ACL
	if (c->opts.acl)
		sb->s_flags	|= SB_POSIXACL;
#endif

	sb->s_shrink->seeks = 0;

	vinode = bch2_vfs_inode_get(c, BCACHEFS_ROOT_SUBVOL_INUM);
	ret = PTR_ERR_OR_ZERO(vinode);
	bch_err_msg(c, ret, "mounting: error getting root inode");
	if (ret)
		goto err_put_super;

	sb->s_root = d_make_root(vinode);
	if (!sb->s_root) {
		bch_err(c, "error mounting: error allocating root dentry");
		ret = -ENOMEM;
		goto err_put_super;
	}

	sb->s_flags |= SB_ACTIVE;
out:
	fc->root = dget(sb->s_root);
err:
	darray_exit(&devs_to_fs);
	bch2_darray_str_exit(&devs);
	if (ret)
		pr_err("error: %s", bch2_err_str(ret));
	/*
	 * On an inconsistency error in recovery we might see an -EROFS derived
	 * errorcode (from the journal), but we don't want to return that to
	 * userspace as that causes util-linux to retry the mount RO - which is
	 * confusing:
	 */
	if (bch2_err_matches(ret, EROFS) && ret != -EROFS)
		ret = -EIO;
	return bch2_err_class(ret);

err_stop_fs:
	bch2_fs_stop(c);
	goto err;

err_put_super:
	__bch2_fs_stop(c);
	deactivate_locked_super(sb);
	goto err;
}

static void bch2_kill_sb(struct super_block *sb)
{
	struct bch_fs *c = sb->s_fs_info;

	generic_shutdown_super(sb);
	bch2_fs_free(c);
}

static void bch2_fs_context_free(struct fs_context *fc)
{
	struct bch2_opts_parse *opts = fc->fs_private;

	if (opts) {
		printbuf_exit(&opts->parse_later);
		kfree(opts);
	}
}

static int bch2_fs_parse_param(struct fs_context *fc,
			       struct fs_parameter *param)
{
	/*
	 * the "source" param, i.e., the name of the device(s) to mount,
	 * is handled by the VFS layer.
	 */
	if (!strcmp(param->key, "source"))
		return -ENOPARAM;

	struct bch2_opts_parse *opts = fc->fs_private;
	struct bch_fs *c = NULL;

	/* for reconfigure, we already have a struct bch_fs */
	if (fc->root)
		c = fc->root->d_sb->s_fs_info;

	int ret = bch2_parse_one_mount_opt(c, &opts->opts,
					   &opts->parse_later, param->key,
					   param->string);

	return bch2_err_class(ret);
}

static int bch2_fs_reconfigure(struct fs_context *fc)
{
	struct super_block *sb = fc->root->d_sb;
	struct bch2_opts_parse *opts = fc->fs_private;

	return bch2_remount(sb, &fc->sb_flags, opts->opts);
}

static const struct fs_context_operations bch2_context_ops = {
	.free        = bch2_fs_context_free,
	.parse_param = bch2_fs_parse_param,
	.get_tree    = bch2_fs_get_tree,
	.reconfigure = bch2_fs_reconfigure,
};

static int bch2_init_fs_context(struct fs_context *fc)
{
	struct bch2_opts_parse *opts = kzalloc(sizeof(*opts), GFP_KERNEL);

	if (!opts)
		return -ENOMEM;

	opts->parse_later = PRINTBUF;

	fc->ops = &bch2_context_ops;
	fc->fs_private = opts;

	return 0;
}

void bch2_fs_vfs_exit(struct bch_fs *c)
{
	if (c->vfs_inodes_table.tbl)
		rhashtable_destroy(&c->vfs_inodes_table);
}

int bch2_fs_vfs_init(struct bch_fs *c)
{
	return rhashtable_init(&c->vfs_inodes_table, &bch2_vfs_inodes_params);
}

static struct file_system_type bcache_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "bcachefs",
	.init_fs_context	= bch2_init_fs_context,
	.kill_sb		= bch2_kill_sb,
	.fs_flags		= FS_REQUIRES_DEV | FS_ALLOW_IDMAP,
};

MODULE_ALIAS_FS("bcachefs");

void bch2_vfs_exit(void)
{
	unregister_filesystem(&bcache_fs_type);
	kmem_cache_destroy(bch2_inode_cache);
}

int __init bch2_vfs_init(void)
{
	int ret = -ENOMEM;

	bch2_inode_cache = KMEM_CACHE(bch_inode_info, SLAB_RECLAIM_ACCOUNT |
				      SLAB_ACCOUNT);
	if (!bch2_inode_cache)
		goto err;

	ret = register_filesystem(&bcache_fs_type);
	if (ret)
		goto err;

	return 0;
err:
	bch2_vfs_exit();
	return ret;
}

#endif /* NO_BCACHEFS_FS */
