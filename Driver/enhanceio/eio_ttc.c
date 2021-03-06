/*
 * True Transparent Caching (TTC) code.
 *  eio_ttc.c
 *
 *  Copyright (C) 2012 STEC, Inc. All rights not specifically granted
 *   under a license included herein are reserved
 *
 *  Made EIO fully transparent with respect to applications. A cache can be
 *  created or deleted while a filesystem or applications are online
 *   Amit Kale <akale@stec-inc.com>
 *   Ramprasad Chinthekindi <rchinthekindi@stec-inc.com>
 *   Akhil Bhansali <abhansali@stec-inc.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include "eio.h"
#include "eio_ttc.h"

static struct rw_semaphore eio_ttc_lock[EIO_HASHTBL_SIZE];
static struct list_head eio_ttc_list[EIO_HASHTBL_SIZE];

int eio_reboot_notified;

static MAKE_REQUEST_FN_TYPE eio_make_request_fn(struct request_queue *, struct bio *);
static void eio_cache_rec_fill(struct cache_c *, struct cache_rec_short *);
static void eio_bio_end_empty_barrier(struct bio *, int);
static void eio_issue_empty_barrier_flush(struct block_device *, struct bio *,
					  int, make_request_fn *, unsigned op, unsigned op_flags);
static int eio_finish_nrdirty(struct cache_c *);
static int eio_mode_switch(struct cache_c *, u_int32_t);
static int eio_policy_switch(struct cache_c *, u_int32_t);

static int eio_overlap_split_bio(struct request_queue *, struct bio *);
static struct bio *eio_split_new_bio(struct bio *, struct bio_container *,
				     unsigned *, unsigned *, sector_t);
static void eio_split_endio(struct bio *, int);

static int eio_open(struct inode *ip, struct file *filp)
{
	__module_get(THIS_MODULE);
	return 0;
}

static int eio_release(struct inode *ip, struct file *filp)
{
	module_put(THIS_MODULE);
	return 0;
}

static const struct file_operations eio_fops = {
	.open		= eio_open,
	.release	= eio_release,
	.unlocked_ioctl = eio_ioctl,
	.compat_ioctl	= eio_compact_ioctl,
	.owner		= THIS_MODULE,
};

static struct miscdevice eio_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= MISC_DEVICE,
	.fops	= &eio_fops,
};

int eio_create_misc_device()
{
	return misc_register(&eio_misc);
}

void eio_delete_misc_device()
{
	misc_deregister(&eio_misc);
}


/* In newer kernels, calling the queue's make_request_fn() always submits
 * the IO. In older kernels however, there is a possibility for the request
 * function to return 1 and expect us to handle the IO redirection (see raid0
 * implementation in kernel 2.6.32). We must check the return value
 * and potentially resubmit the IO.
 */
static inline
void hdd_make_request(make_request_fn *origmfn, struct bio *bio)
{
	struct request_queue *q = NULL;
	int __maybe_unused ret;
	q = bdev_get_queue(bio->bi_bdev);
	if (unlikely(!q)) {
		pr_err("EIO: Trying to access nonexistent block-device\n");
		EIO_BIO_ENDIO(bio, -EIO);
		return;
	}

#ifdef COMPAT_MAKE_REQUEST_FN_SUBMITS_IO
	origmfn(q, bio);
#else
	ret = origmfn(q, bio);
	if (ret) {
		generic_make_request(bio);
	}
#endif
}


int eio_ttc_get_device(const char *path, fmode_t mode, struct eio_bdev **result)
{
	struct block_device *bdev;
	struct eio_bdev *eio_bdev;

	static char *eio_holder = "EnhanceIO";
	bdev = blkdev_get_by_path(path, mode, eio_holder);
	if (IS_ERR(bdev))
		return PTR_ERR(bdev);

	eio_bdev = kzalloc(sizeof(*eio_bdev), GFP_KERNEL);
	if (eio_bdev == NULL) {
		blkdev_put(bdev, mode);
		return -ENOMEM;
	}

	eio_bdev->bdev = bdev;
	eio_bdev->mode = mode;
	*result = eio_bdev;
	return 0;
}

void eio_ttc_put_device(struct eio_bdev **d)
{
	struct eio_bdev *eio_bdev;

	eio_bdev = *d;
	blkdev_put(eio_bdev->bdev, eio_bdev->mode);
	kfree(eio_bdev);
	*d = NULL;
	return;
}

struct cache_c *eio_cache_lookup(char *name)
{
	struct cache_c *dmc = NULL;
	int i;

	for (i = 0; i < EIO_HASHTBL_SIZE; i++) {
		down_read(&eio_ttc_lock[i]);
		list_for_each_entry(dmc, &eio_ttc_list[i], cachelist) {
			if (!strcmp(name, dmc->cache_name)) {
				up_read(&eio_ttc_lock[i]);
				return dmc;
			}
		}
		up_read(&eio_ttc_lock[i]);
	}
	return NULL;
}

int eio_ttc_activate(struct cache_c *dmc)
{
	struct block_device *bdev;
	struct request_queue *rq;
	make_request_fn *origmfn;
	struct cache_c *dmc1;
	int wholedisk;
	int error;
	int index;

	bdev = dmc->disk_dev->bdev;
	if (bdev == NULL) {
		pr_err("cache_create: Source device not found\n");
		return -ENODEV;
	}
	rq = bdev->bd_disk->queue;

	wholedisk = 0;
	if (bdev == bdev->bd_contains)
		wholedisk = 1;

	dmc->dev_start_sect = bdev->bd_part->start_sect;
	dmc->dev_end_sect =
		bdev->bd_part->start_sect + bdev->bd_part->nr_sects - 1;

	pr_debug("eio_ttc_activate: Device/Partition" \
		 " sector_start: %llu, end: %llu\n",
		 (uint64_t)dmc->dev_start_sect, (uint64_t)dmc->dev_end_sect);

	error = 0;
	origmfn = NULL;
	index = EIO_HASH_BDEV(bdev->bd_contains->bd_dev);

	down_write(&eio_ttc_lock[index]);
	list_for_each_entry(dmc1, &eio_ttc_list[index], cachelist) {
		if (dmc1->disk_dev->bdev->bd_contains != bdev->bd_contains)
			continue;

		if ((wholedisk) || (dmc1->dev_info == EIO_DEV_WHOLE_DISK) ||
		    (dmc1->disk_dev->bdev == bdev)) {
			error = -EINVAL;
			up_write(&eio_ttc_lock[index]);
			goto out;
		}

		/* some partition of same device already cached */
		EIO_ASSERT(dmc1->dev_info == EIO_DEV_PARTITION);
		origmfn = dmc1->origmfn;
		break;
	}

	/*
	 * Save original make_request_fn. Switch make_request_fn only once.
	 */

	if (origmfn) {
		dmc->origmfn = origmfn;
		dmc->dev_info = EIO_DEV_PARTITION;
		EIO_ASSERT(wholedisk == 0);
	} else {
		dmc->origmfn = rq->make_request_fn;
		rq->make_request_fn = eio_make_request_fn;
		dmc->dev_info =
			(wholedisk) ? EIO_DEV_WHOLE_DISK : EIO_DEV_PARTITION;
	}

	list_add_tail(&dmc->cachelist, &eio_ttc_list[index]);

	/*
	 * Sleep for sometime, to allow previous I/Os to hit
	 * Issue a barrier I/O on Source device.
	 */

	msleep(1);
	eio_issue_empty_barrier_flush(dmc->disk_dev->bdev, NULL, EIO_HDD_DEVICE,
	                              dmc->origmfn, REQ_OP_FLUSH, WRITE_FLUSH);
	up_write(&eio_ttc_lock[index]);

out:
	if (error == -EINVAL) {
		if (wholedisk)
			pr_err
				("cache_create: A partition of this device is already cached.\n");
		else
			pr_err("cache_create: Device is already cached.\n");
	}
	return error;
}

int eio_ttc_deactivate(struct cache_c *dmc, int force)
{
	struct block_device *bdev;
	struct request_queue *rq;
	struct cache_c *dmc1;
	int found_partitions;
	int index;
	int ret;

	ret = 0;
	bdev = dmc->disk_dev->bdev;
	rq = bdev->bd_disk->queue;

	if (force)
		goto deactivate;

	/* Process and wait for nr_dirty to drop to zero */
	if (dmc->mode == CACHE_MODE_WB) {
		if (!CACHE_FAILED_IS_SET(dmc)) {
			ret = eio_finish_nrdirty(dmc);
			if (ret) {
				pr_err
					("ttc_deactivate: nrdirty failed to finish for cache \"%s\".",
					dmc->cache_name);
				return ret;
			}
		} else
			pr_debug
				("ttc_deactivate: Cache \"%s\" failed is already set. Continue with cache delete.",
				dmc->cache_name);
	}

	/*
	 * Traverse the list and see if other partitions of this device are
	 * cached. Switch mfn if this is the only partition of the device
	 * in the list.
	 */
deactivate:
	index = EIO_HASH_BDEV(bdev->bd_contains->bd_dev);
	found_partitions = 0;

	/* check if barrier QUEUE is empty or not */
	down_write(&eio_ttc_lock[index]);

	if (dmc->dev_info != EIO_DEV_WHOLE_DISK)
		list_for_each_entry(dmc1, &eio_ttc_list[index], cachelist) {
			if (dmc == dmc1)
				continue;

			if (dmc1->disk_dev->bdev->bd_contains !=
				bdev->bd_contains)
				continue;

			EIO_ASSERT(dmc1->dev_info == EIO_DEV_PARTITION);

			/*
			 * There are still other partitions which are cached.
			 * Do not switch the make_request_fn.
			 */

			found_partitions = 1;
			break;
		}

		if ((dmc->dev_info == EIO_DEV_WHOLE_DISK) ||
			(found_partitions == 0))
			rq->make_request_fn = dmc->origmfn;

	list_del_init(&dmc->cachelist);
	up_write(&eio_ttc_lock[index]);

	/* wait for nr_ios to drain-out */
	while (atomic64_read(&dmc->nr_ios) != 0)
		schedule_timeout(msecs_to_jiffies(100));

	return ret;
}

void eio_ttc_init(void)
{
	int i;

	for (i = 0; i < EIO_HASHTBL_SIZE; i++) {
		init_rwsem(&eio_ttc_lock[i]);
		INIT_LIST_HEAD(&eio_ttc_list[i]);
	}
}

/*
 * Cases:-
 * 1. Full device cached.
 *	if (ENQUEUE || barrier(bio))
 *		enqueue (dmc, bio) and return
 *      else
 *		call eio_map(dmc, bio)
 * 2. Some partitions of the device cached.
 *	if (ENQUEUE || barrier(bio))
 *		All I/Os (both on cached and uncached partitions) are enqueued.
 *      else
 *		if (I/O on cached partition)
 *			call eio_map(dmc, bio)
 *		else
 *			origmfn(bio);	// uncached partition
 * 3. q->mfn got switched back to original
 *	call origmfn(q, bio)
 * 4. Race condition:
 */

static MAKE_REQUEST_FN_TYPE eio_make_request_fn(struct request_queue *q,
                                                struct bio *bio)
{
	int ret;
	int overlap;
	int index;
	make_request_fn *origmfn;
	struct cache_c *dmc, *dmc1;
	struct block_device *bdev;

	bdev = bio->bi_bdev;

re_lookup:
	dmc = NULL;
	origmfn = NULL;
	overlap = ret = 0;

	index = EIO_HASH_BDEV(bdev->bd_contains->bd_dev);

	down_read(&eio_ttc_lock[index]);

	list_for_each_entry(dmc1, &eio_ttc_list[index], cachelist) {
		if (dmc1->disk_dev->bdev->bd_contains != bdev->bd_contains)
			continue;

		if (dmc1->dev_info == EIO_DEV_WHOLE_DISK) {
			dmc = dmc1;     /* found cached device */
			break;
		}

		/* Handle partitions */
		if (!origmfn)
			origmfn = dmc1->origmfn;

		/* I/O perfectly fit within cached partition */
		if ((EIO_BIO_BI_SECTOR(bio) >= dmc1->dev_start_sect) &&
		    ((EIO_BIO_BI_SECTOR(bio) +
		      eio_to_sector(EIO_BIO_BI_SIZE(bio)) - 1) <=
		     dmc1->dev_end_sect)) {
			EIO_ASSERT(overlap == 0);
			dmc = dmc1;     /* found cached partition */
			break;
		}

		/* Check if I/O is overlapping with cached partitions */
		if (((EIO_BIO_BI_SECTOR(bio) >= dmc1->dev_start_sect) &&
		     (EIO_BIO_BI_SECTOR(bio) <= dmc1->dev_end_sect)) ||
		    ((EIO_BIO_BI_SECTOR(bio) + eio_to_sector(EIO_BIO_BI_SIZE(bio)) - 1 >=
		      dmc1->dev_start_sect) &&
		     (EIO_BIO_BI_SECTOR(bio) + eio_to_sector(EIO_BIO_BI_SIZE(bio)) - 1 <=
		      dmc1->dev_end_sect))) {
			overlap = 1;
			pr_err
				("Overlapping I/O detected on %s cache at sector: %llu, size: %u\n",
				dmc1->cache_name, (uint64_t)EIO_BIO_BI_SECTOR(bio),
				EIO_BIO_BI_SIZE(bio));
			break;
		}
	}

	if (unlikely(overlap)) {
		up_read(&eio_ttc_lock[index]);

		if (bio_op(bio) == REQ_OP_DISCARD) {
			pr_err
				("eio_mfn: Overlap I/O with Discard flag." \
				" Discard flag is not supported.\n");
			EIO_BIO_ENDIO(bio, -EOPNOTSUPP);
		} else
			ret = eio_overlap_split_bio(q, bio);
	} else if (dmc) {       /* found cached partition or device */
		/*
		 * Start sector of cached partition may or may not be
		 * aligned with cache blocksize.
		 * Map start of the partition to zero reference.
		 */

		if (EIO_BIO_BI_SECTOR(bio)) {
			EIO_ASSERT(EIO_BIO_BI_SECTOR(bio) >= dmc->dev_start_sect);
			EIO_BIO_BI_SECTOR(bio) -= dmc->dev_start_sect;
		}
		ret = eio_map(dmc, q, bio);
		if (ret)
			/* Error case: restore the start sector of bio */
			EIO_BIO_BI_SECTOR(bio) += dmc->dev_start_sect;
	}

	if (!overlap)
		up_read(&eio_ttc_lock[index]);

	if (overlap || dmc)
		MAKE_REQUEST_FN_RETURN_0;

	/*
	 * Race condition:-
	 * origmfn can be NULL if  all partitions or whole disk got uncached.
	 * We set origmfn = q->mfn if origmfn is NULL.
	 * The origmfn may now again be eio_make_request_fn because
	 * someone else switched the q->mfn because of a new
	 * partition or whole disk being cached.
	 * Since, we cannot protect q->make_request_fn() by any lock,
	 * this situation may occur. However, this is a very rare event.
	 * In this case restart the lookup.
	 */

	if (origmfn == NULL)
		origmfn = q->make_request_fn;
	if (origmfn == eio_make_request_fn)
		goto re_lookup;

	hdd_make_request(origmfn, bio);

	MAKE_REQUEST_FN_RETURN_0;
}

uint64_t eio_get_cache_count(void)
{
	struct cache_c *dmc;
	uint64_t cnt = 0;
	int i;

	for (i = 0; i < EIO_HASHTBL_SIZE; i++) {
		down_read(&eio_ttc_lock[i]);
		list_for_each_entry(dmc, &eio_ttc_list[i], cachelist) {
			cnt++;
		}
		up_read(&eio_ttc_lock[i]);
	}
	return cnt;
}

int eio_get_cache_list(unsigned long *arg)
{
	int error = 0;
	unsigned int size, i, j;
	struct cache_list reclist;
	struct cache_rec_short *cache_recs;
	struct cache_c *dmc;

	if (copy_from_user(&reclist, (struct cache_list __user *)arg,
			   sizeof(struct cache_list))) {
		error = -EFAULT;
		goto out;
	}

	size = reclist.ncaches * sizeof(struct cache_rec_short);
	cache_recs = vmalloc(size);
	if (!cache_recs) {
		error = -ENOMEM;
		goto out;
	}
	memset(cache_recs, 0, size);

	i = 0;
	for (j = 0; j < EIO_HASHTBL_SIZE; j++) {
		down_read(&eio_ttc_lock[j]);
		list_for_each_entry(dmc, &eio_ttc_list[j], cachelist) {
			eio_cache_rec_fill(dmc, &cache_recs[i]);
			i++;

			if (i == reclist.ncaches)
				break;
		}
		up_read(&eio_ttc_lock[j]);

		if (i == reclist.ncaches)
			break;
	}

	if (copy_to_user((char __user *)reclist.cachelist,
			 (char *)cache_recs, size)) {
		error = -EFAULT;
		goto out;
	}

	if (copy_to_user((struct cache_list __user *)arg, &reclist,
			 sizeof(struct cache_list))) {
		error = -EFAULT;
		goto out;
	}

out:
	return error;
}

static void eio_cache_rec_fill(struct cache_c *dmc, struct cache_rec_short *rec)
{
	strncpy(rec->cr_name, dmc->cache_name, sizeof(rec->cr_name) - 1);
	strncpy(rec->cr_src_devname, dmc->disk_devname,
		sizeof(rec->cr_src_devname) - 1);
	strncpy(rec->cr_ssd_devname, dmc->cache_devname,
		sizeof(rec->cr_ssd_devname) - 1);
	rec->cr_src_dev_size = eio_get_device_size(dmc->disk_dev);
	rec->cr_ssd_dev_size = eio_get_device_size(dmc->cache_dev);
	rec->cr_src_sector_size = LOG_BLK_SIZE(dmc->disk_dev->bdev);
	rec->cr_ssd_sector_size = LOG_BLK_SIZE(dmc->cache_dev->bdev);
	rec->cr_flags = dmc->cache_flags;
	rec->cr_policy = dmc->req_policy;
	rec->cr_mode = dmc->mode;
	rec->cr_persistence = dmc->persistence;
	rec->cr_blksize = dmc->block_size;      /* In sectors */
	rec->cr_assoc = dmc->assoc;
	return;
}

/*
 * Few sanity checks before cache creation.
 */

int eio_do_preliminary_checks(struct cache_c *dmc)
{
	struct block_device *bdev, *ssd_bdev;
	struct cache_c *dmc1;
	int error;
	int wholedisk;
	int index;

	error = wholedisk = 0;
	bdev = dmc->disk_dev->bdev;
	ssd_bdev = dmc->cache_dev->bdev;

	/*
	 * Disallow cache creation if source and cache device
	 * belong to same device.
	 */

	if (bdev->bd_contains == ssd_bdev->bd_contains)
		return -EINVAL;

	/*
	 * Check if cache with same name exists.
	 */

	if (eio_cache_lookup(dmc->cache_name))
		return -EEXIST;

	if (bdev == bdev->bd_contains)
		wholedisk = 1;

	index = EIO_HASH_BDEV(bdev->bd_contains->bd_dev);

	down_read(&eio_ttc_lock[index]);
	list_for_each_entry(dmc1, &eio_ttc_list[index], cachelist) {
		if (dmc1->disk_dev->bdev->bd_contains != bdev->bd_contains)
			continue;

		if ((wholedisk) || (dmc1->dev_info == EIO_DEV_WHOLE_DISK) ||
		    (dmc1->disk_dev->bdev == bdev)) {
			error = -EINVAL;
			break;
		}
	}
	up_read(&eio_ttc_lock[index]);
	return error;
}

/* Use mempool_alloc and free for io in sync_io as well */
static void eio_dec_count(struct eio_context *io, int error)
{
	if (error)
		io->error = error;

	if (atomic_dec_and_test(&io->count)) {
		if (io->event)
			complete(io->event);
		else {
			int err = io->error;
			eio_notify_fn fn = io->callback;
			void *context = io->context;

			mempool_free(io, _io_pool);
			io = NULL;
			fn(err, context);
		}
	}
}

static void eio_endio(struct bio *bio, int error)
{
	struct eio_context *io;

	EIO_ENDIO_FN_START;

	io = bio->bi_private;
	EIO_ASSERT(io != NULL);

	bio_put(bio);

	eio_dec_count(io, error);
}

static int eio_dispatch_io_pages(struct cache_c *dmc,
				 struct eio_io_region *where, unsigned op, unsigned op_flags,
				 struct page **pagelist, struct eio_context *io,
				 int hddio, int num_vecs)
{
	struct bio *bio;
	struct page *page;
	unsigned long len;
	unsigned offset;
	int num_bvecs;
	int remaining_bvecs = num_vecs;
	int ret = 0;
	int pindex = 0;

	sector_t remaining = where->count;

	do {
		/* Verify that num_vecs should not cross the threshhold */
		/* Check how many max bvecs bdev supports */
		num_bvecs =
			min_t(int,
                            EIO_BIO_GET_NR_VECS(where->bdev),
                            remaining_bvecs);
		bio = bio_alloc(GFP_NOIO, num_bvecs);
		bio->bi_bdev = where->bdev;
		EIO_BIO_BI_SECTOR(bio) = where->sector + (where->count - remaining);

		/* Remap the start sector of partition */
		if (hddio)
			EIO_BIO_BI_SECTOR(bio) += dmc->dev_start_sect;
		bio_set_op_attrs(bio, op, op_flags);
		bio->bi_end_io = eio_endio;
		bio->bi_private = io;

		while (remaining) {
			page = pagelist[pindex];
			len =
				min_t(unsigned long, PAGE_SIZE,
				      to_bytes(remaining));
			offset = 0;

			if (!bio_add_page(bio, page, len, offset))
				break;

			remaining -= eio_to_sector(len);
			pindex++;
			remaining_bvecs--;
		}

		atomic_inc(&io->count);
		if (hddio)
			hdd_make_request(dmc->origmfn, bio);

		else
			submit_bio(bio);

	} while (remaining);

	EIO_ASSERT(remaining_bvecs == 0);
	return ret;
}

static void end_unaligned_free(struct bio *bio, int error)
{
	EIO_ENDIO_FN_START;
	struct unaligned_bio *un_bio = bio->bi_private;
	struct eio_context *io = un_bio->io;

	put_page(un_bio->page);
	un_bio->page = NULL;
	kfree(un_bio);
	bio_put(bio);
	eio_dec_count(io, error);
}


static void end_unaligned_io(struct bio *bio, int error)
{
	EIO_ENDIO_FN_START;
	unsigned char *loc_mem, *rem_mem;
	int remain, offset;
	struct unaligned_bio *un_bio = bio->bi_private;
	struct bio *write_bio;

	if (error) {
		pr_err("end_unaligned_io: I/O ERROR %d", error);
		goto out;
	}

	loc_mem = kmap_atomic(un_bio->page);
	remain = un_bio->len;
	offset = un_bio->offset;
	while (remain > 0) {
		unsigned rem_offset = un_bio->bvecs->bv_offset;
		unsigned rem_len = un_bio->bvecs->bv_len;
		unsigned bytes = 0;
		if (un_bio->bvecs == un_bio->remain_vec) {
			rem_len -= un_bio->vec_remain;
			EIO_ASSERT (rem_len > 0);
		}
		if (un_bio->bvecs == un_bio->offset_vec) {
			rem_len -= un_bio->vec_offset;
			rem_offset += un_bio->vec_offset;
			EIO_ASSERT (rem_len > 0);
		}
		bytes = min_t(unsigned, rem_len, remain);
		rem_mem = kmap_atomic(un_bio->bvecs->bv_page);
		if (un_bio->op == REQ_OP_READ) {
			memcpy(rem_mem + rem_offset, loc_mem + offset, bytes);
		} else if (un_bio->op == REQ_OP_WRITE) {
			memcpy(loc_mem + offset, rem_mem + rem_offset, bytes);
		}
		kunmap_atomic(rem_mem);
		remain -= bytes;
		if (remain > 0) {
			/*
			 * completed this vec, but there still data to
			 * transfer, move to the next vector in
			 * un_bio->bvecs
			 */
			un_bio->bvecs = un_bio->bvecs + 1;
			offset += bytes;
		}
	}

	kunmap_atomic(loc_mem);

	if (un_bio->op == REQ_OP_WRITE) {
		write_bio = bio_alloc(GFP_NOIO, 1);
		if (unlikely(!write_bio)) {
			pr_err("end_unaligned: Unable to allocate bio.\n");
			error = -ENOMEM;
			goto out;
		}
		if (unlikely(!bio_add_page(write_bio, un_bio->page,
				           LOG_BLK_SIZE(bio->bi_bdev), 0))) {
			pr_err("end_unaligned: Unable to add page to bio.\n");
			bio_put(write_bio);
			error = -ENOMEM;
			goto out;
		}
		EIO_BIO_BI_SECTOR(write_bio) = EIO_BIO_BI_SECTOR(bio);
		write_bio->bi_bdev = bio->bi_bdev;
		write_bio->bi_end_io = end_unaligned_free;
		write_bio->bi_private = un_bio;
		bio_set_op_attrs(write_bio, REQ_OP_WRITE, EIO_REQ_SYNC);
		bio_put(bio);
		submit_bio(write_bio);

		return;
	}

out:
	end_unaligned_free(bio, error);
}

static long do_unaligned_io(struct unaligned_bio *un_bio, sector_t sector,
                           sector_t remaining, struct block_device *bdev,
                           int *compl_vecs, unsigned long *vec_remain)
{
	struct bio *bio;
	struct bio_vec *bvec;
	int remain;
	long len;

	/* alloc page and bio_add_page it */

	bio = bio_alloc(GFP_NOIO, 1);
	if (unlikely(!bio)) {
		pr_err("do_unaligned: Unable to allocate bio.\n");
		goto error_out;
	}
	un_bio->page = alloc_page(GFP_NOIO | __GFP_ZERO);
	if (unlikely(!un_bio->page)) {
		pr_err("do_unaligned: Unable to allocate page.\n");
		goto free_bio;
	}

	if (unlikely(!bio_add_page(bio, un_bio->page, LOG_BLK_SIZE(bdev), 0))) {
		pr_err("do_unaligned: Unable to add page to bio.\n");
		goto free_page;
	}

	bvec = un_bio->bvecs;

	EIO_BIO_BI_SECTOR(bio) =
		EIO_ALIGN_SECTOR(bdev, sector);

	un_bio->offset = to_bytes(sector - EIO_BIO_BI_SECTOR(bio));
	len = min_t(int, (EIO_BIO_BI_SIZE(bio) - un_bio->offset),
	            to_bytes(remaining));
	EIO_ASSERT(len > 0);
	un_bio->len = len;

	if (*vec_remain > 0) {
		remain = un_bio->len - *vec_remain;
		un_bio->offset_vec = bvec;
		un_bio->vec_offset = bvec->bv_len - *vec_remain;
	} else {
		remain = un_bio->len - bvec->bv_len;
	}
	*vec_remain = 0;
	while (remain > 0) {
		/*
		 * completed this vec, move to the next one if
		 * there are still data to transfer (remain > 0)
		 */
		bvec = bvec +1;
		(*compl_vecs)++;
		remain -= bvec->bv_len;
	}

	if (remain < 0) {
		/*
		 * vec has more data to transfer
		 * remain is NEGATIVE here
		 */
		un_bio->remain_vec = bvec;
		un_bio->vec_remain = -remain;
		*vec_remain = -remain;
	}
	if (remain == 0 || remaining == eio_to_sector(un_bio->len)) {
		/*
		 * mark vector as completed if it has no more space (remain==0)
		 * or if it was the last one and we finished with this io
		 */
		(*compl_vecs)++;
	}

	bio->bi_bdev = bdev;
	bio->bi_end_io = end_unaligned_io;
	bio->bi_private = un_bio;
	bio_set_op_attrs(bio, REQ_OP_READ, EIO_REQ_SYNC);
	atomic_inc(&un_bio->io->count);
	submit_bio(bio);
	return eio_to_sector(len);

free_page:
	put_page(un_bio->page);
	un_bio->page = NULL;
free_bio:
	bio_put(bio);
error_out:
	return -ENOMEM;
}

/*
 * This function will dispatch the i/o. It also takes care of
 * splitting the large I/O requets to smaller I/Os which may not
 * fit into single bio.
 */

static int eio_dispatch_io(struct cache_c *dmc, struct eio_io_region *where,
                           unsigned op, unsigned op_flags, struct bio_vec *bvec,
                           struct eio_context *io, int hddio, int num_vecs)
{
	struct bio *bio;
	struct page *page;
	struct unaligned_bio *un_bio;
	unsigned long len, vec_remain;
	unsigned offset;
	int num_bvecs;
	int remaining_bvecs = num_vecs;
	int ret = 0;

	sector_t remaining = where->count;
	sector_t remaining_aligned;

	vec_remain = 0;
	do {
		/* align */
		while (remaining &&
			(EIO_SEC_UNALIGNED
				(where->bdev,
			         where->sector + where->count - remaining) ||
			 remaining < LOG_BLK_SSIZE(where->bdev))) {
			int vecs = 0;
			long r;

			un_bio = kzalloc(sizeof(*un_bio), GFP_NOIO);
			if (unlikely(!un_bio)) {
				pr_err("dispatch_io: Unable to allocate un_bio.\n");
				return -ENOMEM;
			}
			un_bio->bvecs = bvec;
			un_bio->io = io;
			un_bio->op = op;
			pr_debug("dispatch_io: processing unaligned I/O: sector %lu, count %lu",
	                         (where->sector + where->count - remaining),
			         remaining);
			atomic64_inc(&dmc->eio_stats.unaligned_ios);
			r = do_unaligned_io(un_bio, (where->sector +
				            where->count - remaining),
				            remaining, where->bdev, &vecs,
				            &vec_remain);
			if (r < 0) {
				kfree(un_bio);
				return r;
			}
			remaining -= (sector_t)r;
			remaining_bvecs -= vecs;
			bvec = bvec + vecs;
		}
		if (!remaining) {
			break;
		}
		/* Verify that num_vecs should not cross the threshhold */
		/* Check how many max bvecs bdev supports */

		if (remaining_bvecs > EIO_BIO_GET_NR_VECS(where->bdev)) {
			num_bvecs = EIO_BIO_GET_NR_VECS(where->bdev);
			/*
			 * TODO:
			 * we assume the worst case, when 1 bvec contains only
			 * 1 sector - can be less effective sometimes. Also,
			 * in both cases there is overprovisioning of
			 * bvecs in bio
			 */
			remaining_aligned =
				EIO_ALIGN_SECTOR(where->bdev,
				                 to_bytes(num_bvecs));
		} else {
			num_bvecs = remaining_bvecs;
			remaining_aligned =
				EIO_ALIGN_SECTOR(where->bdev, remaining);
		}
		bio = bio_alloc(GFP_NOIO, num_bvecs);
		bio->bi_bdev = where->bdev;
		EIO_BIO_BI_SECTOR(bio) = where->sector +
			                 (where->count - remaining);

		/* Remap the start sector of partition */
		if (hddio)
			EIO_BIO_BI_SECTOR(bio) += dmc->dev_start_sect;

		bio_set_op_attrs(bio, op, op_flags);
		bio->bi_end_io = eio_endio;
		bio->bi_private = io;

		while (remaining_aligned > 0) {
			page = bvec->bv_page;
			offset = bvec->bv_offset;
			if (vec_remain > 0) {
				len = min_t(unsigned long, vec_remain,
				            to_bytes(remaining_aligned));
				offset += bvec->bv_len - vec_remain;
			} else {
				len = min_t(unsigned long, bvec->bv_len,
				            to_bytes(remaining_aligned));
			}

			if (!bio_add_page(bio, page, len, offset))
				break;

			remaining_aligned -= eio_to_sector(len);
			remaining -= eio_to_sector(len);
			if (remaining && (len < bvec->bv_len - vec_remain)) {
				/*
				 * vector is not finished and there are more io,
				 * do not forward bvec, set vec_remain to not
				 * yet processed remain of this vector
				 */
				vec_remain = bvec->bv_len - len;
			} else {
				bvec = bvec + 1;
				vec_remain = 0;
				remaining_bvecs--;
			}
		}

		atomic_inc(&io->count);
		if (hddio)
			hdd_make_request(dmc->origmfn, bio);
		else
			submit_bio(bio);

	} while (remaining);

	EIO_ASSERT(remaining_bvecs == 0);
	return ret;
}

static int eio_async_io(struct cache_c *dmc, struct eio_io_region *where,
                        unsigned op, unsigned op_flags,
                        struct eio_io_request *req)
{
	struct eio_context *io;
	int err = 0;

	io = mempool_alloc(_io_pool, GFP_NOIO);
	if (unlikely(io == NULL)) {
		pr_err("eio_async_io: failed to allocate eio_context.\n");
		return -ENOMEM;
	}
	memset((char *)io, 0, sizeof(struct eio_context));

	atomic_set(&io->count, 1);
	io->callback = req->notify;
	io->context = req->context;
	io->event = NULL;

	switch (req->mtype) {
	case EIO_BVECS:
		err =
			eio_dispatch_io(dmc, where, op, op_flags,
			                req->dptr.pages, io,
			                req->hddio, req->num_bvecs);
		break;

	case EIO_PAGES:
		err =
			eio_dispatch_io_pages(dmc, where, op, op_flags,
			                      req->dptr.plist, io,
			                      req->hddio, req->num_bvecs);
		break;
	}

	/* Check if i/o submission has returned any error */
	if (unlikely(err)) {
		/* Wait for any i/os which are submitted, to end. */
retry:
		if (atomic_read(&io->count) != 1) {
			schedule_timeout(msecs_to_jiffies(1));
			goto retry;
		}

		EIO_ASSERT(io != NULL);
		mempool_free(io, _io_pool);
		io = NULL;
		return err;
	}

	/* Drop the extra reference count here */
	eio_dec_count(io, err);
	return err;
}

static int eio_sync_io(struct cache_c *dmc, struct eio_io_region *where,
		       unsigned op, unsigned op_flags,
	               struct eio_io_request *req)
{
	int ret = 0;
	struct eio_context io;

	DECLARE_COMPLETION_ONSTACK(wait);

	memset((char *)&io, 0, sizeof(io));

	atomic_set(&io.count, 1);
	io.event = &wait;
	io.callback = NULL;
	io.context = NULL;

	/* For synchronous I/Os pass SYNC */
	op_flags |= EIO_REQ_SYNC;

	switch (req->mtype) {
	case EIO_BVECS:
		ret = eio_dispatch_io(dmc, where, op, op_flags, req->dptr.pages,
				      &io, req->hddio, req->num_bvecs);
		break;
	case EIO_PAGES:
		ret = eio_dispatch_io_pages(dmc, where, op, op_flags,
			                    req->dptr.plist, &io, req->hddio,
			                    req->num_bvecs);
		break;
	}

	/* Check if i/o submission has returned any error */
	if (unlikely(ret)) {
		/* Wait for any i/os which are submitted, to end. */
retry:
		if (atomic_read(&(io.count)) != 1) {
			schedule_timeout(msecs_to_jiffies(1));
			goto retry;
		}

		return ret;
	}

	/* Drop extra reference count here */
	eio_dec_count(&io, ret);
	wait_for_completion(&wait);

	if (io.error)
		ret = io.error;

	return ret;
}

int eio_do_io(struct cache_c *dmc, struct eio_io_region *where, unsigned op,
	      unsigned op_flags, struct eio_io_request *io_req)
{
	if (!io_req->notify)
		return eio_sync_io(dmc, where, op, op_flags, io_req);

	return eio_async_io(dmc, where, op, op_flags, io_req);
}

void eio_process_zero_size_bio(struct cache_c *dmc, struct bio *origbio)
{
	unsigned op_flags = 0;
	unsigned op = 0;

	/* Extract bio flags from original bio */
	op_flags = bio_flags(origbio);
	op = bio_op(origbio);

	EIO_ASSERT(EIO_BIO_BI_SIZE(origbio) == 0);
	EIO_ASSERT(op != REQ_OP_READ);

	eio_issue_empty_barrier_flush(dmc->cache_dev->bdev, NULL,
				      EIO_SSD_DEVICE, NULL, op, op_flags);
	eio_issue_empty_barrier_flush(dmc->disk_dev->bdev, origbio,
				      EIO_HDD_DEVICE, dmc->origmfn, op,
				      op_flags);
}

static void eio_bio_end_empty_barrier(struct bio *bio, int error)
{
	EIO_ENDIO_FN_START;

	if (bio->bi_private)
		/*bio->bi_private is expected to be orig_bio here*/
		EIO_BIO_ENDIO((struct bio*)bio->bi_private, error);
	bio_put(bio);
	return;
}

static void eio_issue_empty_barrier_flush(struct block_device *bdev,
					  struct bio *orig_bio, int device,
					  make_request_fn *origmfn,
					  unsigned op, unsigned op_flags)
{
	struct bio *bio;

	bio = bio_alloc(GFP_KERNEL, 0);
	if (!bio)
		if (orig_bio)
			EIO_BIO_ENDIO(orig_bio, -ENOMEM);
	bio->bi_end_io = eio_bio_end_empty_barrier;
	bio->bi_private = orig_bio;
	bio->bi_bdev = bdev;
	bio_set_op_attrs(bio, op, op_flags);

	bio_get(bio);
	if (device == EIO_HDD_DEVICE)
		hdd_make_request(origmfn, bio);

	else
		submit_bio(bio);
	bio_put(bio);
	return;
}

static int eio_finish_nrdirty(struct cache_c *dmc)
{
	int index;
	int ret = 0;
	int retry_count;

	/*
	 * Due to any transient errors, finish_nr_dirty may not drop
	 * to zero. Retry the clean operations for FINISH_NRDIRTY_RETRY_COUNT.
	 */
	retry_count = FINISH_NRDIRTY_RETRY_COUNT;

	index = EIO_HASH_BDEV(dmc->disk_dev->bdev->bd_contains->bd_dev);
	down_write(&eio_ttc_lock[index]);

	/* Wait for the in-flight I/Os to drain out */
	while (atomic64_read(&dmc->nr_ios) != 0) {
		pr_debug("finish_nrdirty: Draining I/O inflight\n");
		schedule_timeout(msecs_to_jiffies(1));
	}
	EIO_ASSERT(!(dmc->sysctl_active.do_clean & EIO_CLEAN_START));

	dmc->sysctl_active.do_clean |= EIO_CLEAN_KEEP | EIO_CLEAN_START;
	up_write(&eio_ttc_lock[index]);

	/*
	 * In the process of cleaning CACHE if CACHE turns to FAILED state,
	 * its a severe error.
	 */
	do {
		if (unlikely(CACHE_FAILED_IS_SET(dmc))) {
			pr_err
				("finish_nrdirty: CACHE \"%s\" is in FAILED state.",
				dmc->cache_name);
			ret = -ENODEV;
			break;
		}

		if (!dmc->sysctl_active.fast_remove)
			eio_clean_all(dmc);
	} while (!dmc->sysctl_active.fast_remove &&
		 (atomic64_read(&dmc->nr_dirty) > 0)
		 && (!(dmc->cache_flags & CACHE_FLAGS_SHUTDOWN_INPROG)));
	dmc->sysctl_active.do_clean &= ~EIO_CLEAN_START;

	/*
	 * If all retry_count exhausted and nr_dirty is still not zero.
	 * Return error.
	 */
	if (((dmc->cache_flags & CACHE_FLAGS_SHUTDOWN_INPROG) ||
	     (retry_count == 0)) && (atomic64_read(&dmc->nr_dirty) > 0))
		ret = -EINVAL;
	if (ret)
		pr_err
			("finish_nrdirty: Failed to finish %llu dirty blocks for cache \"%s\".",
			(unsigned long long)atomic64_read(&dmc->nr_dirty), dmc->cache_name);

	return ret;
}

int eio_cache_edit(char *cache_name, u_int32_t mode, u_int32_t policy)
{
	int error = 0;
	int index;
	struct cache_c *dmc;
	uint32_t old_time_thresh = 0;
	int restart_async_task = 0;
	int ret;

	EIO_ASSERT((mode != 0) || (policy != 0));

	dmc = eio_cache_lookup(cache_name);
	if (NULL == dmc) {
		pr_err("cache_edit: cache %s do not exist", cache_name);
		return -EINVAL;
	}

	if ((dmc->mode == mode) && (dmc->req_policy == policy))
		return 0;

	if (unlikely(CACHE_FAILED_IS_SET(dmc)) ||
	    unlikely(CACHE_DEGRADED_IS_SET(dmc))) {
		pr_err("cache_edit: Cannot proceed with edit on cache \"%s\"" \
		       ". Cache is in failed or degraded state.",
		       dmc->cache_name);
		return -EINVAL;
	}

	spin_lock_irqsave(&dmc->cache_spin_lock, dmc->cache_spin_lock_flags);
	if (dmc->cache_flags & CACHE_FLAGS_SHUTDOWN_INPROG) {
		pr_err("cache_edit: system shutdown in progress, cannot edit" \
		       " cache %s", cache_name);
		spin_unlock_irqrestore(&dmc->cache_spin_lock,
				       dmc->cache_spin_lock_flags);
		return -EINVAL;
	}
	if (dmc->cache_flags & CACHE_FLAGS_MOD_INPROG) {
		pr_err("cache_edit: simultaneous edit/delete operation on " \
		       "cache %s is not permitted", cache_name);
		spin_unlock_irqrestore(&dmc->cache_spin_lock,
				       dmc->cache_spin_lock_flags);
		return -EINVAL;
	}
	dmc->cache_flags |= CACHE_FLAGS_MOD_INPROG;
	spin_unlock_irqrestore(&dmc->cache_spin_lock,
			       dmc->cache_spin_lock_flags);
	old_time_thresh = dmc->sysctl_active.time_based_clean_interval;

	if (dmc->mode == CACHE_MODE_WB) {
		if (CACHE_FAILED_IS_SET(dmc)) {
			pr_err
				("cache_edit:  Can not proceed with edit for Failed cache \"%s\".",
				dmc->cache_name);
			error = -EINVAL;
			goto out;
		}
		eio_stop_async_tasks(dmc);
		restart_async_task = 1;
	}

	/* Wait for nr_dirty to drop to zero */
	if (dmc->mode == CACHE_MODE_WB && mode != CACHE_MODE_WB) {
		if (CACHE_FAILED_IS_SET(dmc)) {
			pr_err
				("cache_edit:  Can not proceed with edit for Failed cache \"%s\".",
				dmc->cache_name);
			error = -EINVAL;
			goto out;
		}

		error = eio_finish_nrdirty(dmc);
		/* This error can mostly occur due to Device removal */
		if (unlikely(error)) {
			pr_err
				("cache_edit: nr_dirty FAILED to finish for cache \"%s\".",
				dmc->cache_name);
			goto out;
		}
		EIO_ASSERT((dmc->sysctl_active.do_clean & EIO_CLEAN_KEEP) &&
			   !(dmc->sysctl_active.do_clean & EIO_CLEAN_START));
		EIO_ASSERT(dmc->sysctl_active.fast_remove ||
			   (atomic64_read(&dmc->nr_dirty) == 0));
	}

	index = EIO_HASH_BDEV(dmc->disk_dev->bdev->bd_contains->bd_dev);
	down_write(&eio_ttc_lock[index]);

	/* Wait for the in-flight I/Os to drain out */
	while (atomic64_read(&dmc->nr_ios) != 0) {
		pr_debug("cache_edit: Draining I/O inflight\n");
		schedule_timeout(msecs_to_jiffies(1));
	}

	pr_debug("cache_edit: Blocking application I/O\n");

	EIO_ASSERT(atomic64_read(&dmc->nr_ios) == 0);

	/* policy change */
	if ((policy != 0) && (policy != dmc->req_policy)) {
		error = eio_policy_switch(dmc, policy);
		if (error) {
			up_write(&eio_ttc_lock[index]);
			goto out;
		}
	}

	/* mode change */
	if ((mode != 0) && (mode != dmc->mode)) {
		error = eio_mode_switch(dmc, mode);
		if (error) {
			up_write(&eio_ttc_lock[index]);
			goto out;
		}
	}

	dmc->sysctl_active.time_based_clean_interval = old_time_thresh;
	/* write updated superblock */
	error = eio_sb_store(dmc);
	if (error) {
		/* XXX: In case of error put the cache in degraded mode. */
		pr_err("eio_cache_edit: superblock update failed(error %d)",
		       error);
		goto out;
	}

	eio_procfs_dtr(dmc);
	eio_procfs_ctr(dmc);

	up_write(&eio_ttc_lock[index]);

out:
	dmc->sysctl_active.time_based_clean_interval = old_time_thresh;

	/*
	 * Resetting EIO_CLEAN_START and EIO_CLEAN_KEEP flags.
	 * EIO_CLEAN_START flag should be restored if eio_stop_async_tasks()
	 * is not called in future.
	 */

	dmc->sysctl_active.do_clean &= ~(EIO_CLEAN_START | EIO_CLEAN_KEEP);

	/* Restart async-task for "WB" cache. */
	if ((dmc->mode == CACHE_MODE_WB) && (restart_async_task == 1)) {
		pr_debug("cache_edit: Restarting the clean_thread.\n");
		EIO_ASSERT(dmc->clean_thread == NULL);
		ret = eio_start_clean_thread(dmc);
		if (ret) {
			error = ret;
			pr_err
				("cache_edit: Failed to restart async tasks. error=%d.\n",
				ret);
		}
		if (dmc->sysctl_active.time_based_clean_interval &&
		    atomic64_read(&dmc->nr_dirty)) {
			schedule_delayed_work(&dmc->clean_aged_sets_work,
					      dmc->
					      sysctl_active.time_based_clean_interval
					      * 60 * HZ);
			dmc->is_clean_aged_sets_sched = 1;
		}
	}
	spin_lock_irqsave(&dmc->cache_spin_lock, dmc->cache_spin_lock_flags);
	dmc->cache_flags &= ~CACHE_FLAGS_MOD_INPROG;
	spin_unlock_irqrestore(&dmc->cache_spin_lock,
			       dmc->cache_spin_lock_flags);
	pr_debug("eio_cache_edit: Allowing application I/O\n");
	return error;
}

static int eio_mode_switch(struct cache_c *dmc, u_int32_t mode)
{
	int error = 0;
	u_int32_t orig_mode;

	EIO_ASSERT(dmc->mode != mode);
	pr_debug("eio_mode_switch: mode switch from %u to %u\n",
		 dmc->mode, mode);

	if (mode == CACHE_MODE_WB) {
		orig_mode = dmc->mode;
		dmc->mode = mode;

		error = eio_allocate_wb_resources(dmc);
		if (error) {
			dmc->mode = orig_mode;
			goto out;
		}
	} else if (dmc->mode == CACHE_MODE_WB) {
		eio_free_wb_resources(dmc);
		dmc->mode = mode;
	} else {                /* (RO -> WT) or (WT -> RO) */
		EIO_ASSERT(((dmc->mode == CACHE_MODE_RO) &&
			    (mode == CACHE_MODE_WT)) ||
			   ((dmc->mode == CACHE_MODE_WT) &&
			    (mode == CACHE_MODE_RO)));
		dmc->mode = mode;
	}

out:
	if (error)
		pr_err("mode_switch: Failed to switch mode, error: %d\n",
		       error);
	return error;
}

/*
 * XXX: Error handling.
 * In case of error put the cache in degraded mode.
 */

static int eio_policy_switch(struct cache_c *dmc, u_int32_t policy)
{
	int error = -EINVAL;
	struct eio_policy *old_policy_ops;

	EIO_ASSERT(dmc->req_policy != policy);
	old_policy_ops = dmc->policy_ops;
	
	error = eio_policy_init(dmc);
	if (error)
		goto out;

	error = eio_repl_blk_init(dmc->policy_ops);
	if (error) {
		error = -ENOMEM;
		pr_err ("eio_policy_swtich: Unable to allocate memory for policy cache block");
		goto out;
	}

	error = eio_repl_sets_init(dmc->policy_ops);
	if (error) {
		error = -ENOMEM;
		pr_err 	("eio_policy_switch: Failed to allocate memory for cache policy");
		goto out;
	}

	eio_policy_lru_pushblks(dmc->policy_ops);
	dmc->req_policy = policy;
	return 0;

out:
	if (dmc->policy_ops != old_policy_ops)
		eio_policy_free(dmc);
	dmc->policy_ops = old_policy_ops;
	return error;
}

void eio_free_wb_pages(struct page **pages, int allocated)
{
	/* Verify that allocated is never 0 or less that zero. */
	if (allocated <= 0)
		return;

	do
		put_page(pages[--allocated]);
	while (allocated);

	*pages = NULL;
}

void eio_free_wb_bvecs(struct bio_vec *bvec, int allocated, int blksize)
{
	int i;

	if (allocated <= 0)
		return;

	for (i = 0; i < allocated; i++) {
		switch (blksize) {
		case BLKSIZE_2K:
			/*
			 * For 2k blocksize, each page is shared between two
			 * bio_vecs. Hence make sure to put_page only for even
			 * indexes.
			 */
			if (((i % 2) == 0) && bvec[i].bv_page) {
				put_page(bvec[i].bv_page);
				bvec[i].bv_page = NULL;
				continue;
			}

			/* For odd index page should already have been freed. */
			if ((i % 2))
				bvec[i].bv_page = NULL;

			continue;

		case BLKSIZE_4K:
		case BLKSIZE_8K:
			if (bvec[i].bv_page) {
				put_page(bvec[i].bv_page);
				bvec[i].bv_page = NULL;
			}

			continue;
		}
	}
}

/*
 * This function allocates pages to array of bvecs allocated by caller.
 * It has special handling of blocksize of 2k where single page is
 * shared between two bio_vecs.
 */

int eio_alloc_wb_bvecs(struct bio_vec *bvec, int max, int blksize)
{
	int i, ret;
	struct bio_vec *iovec;
	struct page *page;

	ret = 0;
	iovec = bvec;
	page = NULL;

	for (i = 0; i < max; i++) {
		switch (blksize) {
		case BLKSIZE_2K:
			/*
			 * In case of 2k blocksize, two biovecs will be sharing
			 * same page address. This is handled below.
			 */

			if ((i % 2) == 0) {
				/* Allocate page only for even bio vector */
				page = alloc_page(GFP_NOIO | __GFP_ZERO);
				if (unlikely(!page)) {
					pr_err
						("eio_alloc_wb_bvecs: System memory too low.\n");
					goto err;
				}
				iovec[i].bv_page = page;
				iovec[i].bv_len = to_bytes(blksize);
				iovec[i].bv_offset = 0;
			} else {
				/* Let the odd biovec share page allocated earlier. */
				EIO_ASSERT(page != NULL);
				iovec[i].bv_page = page;
				iovec[i].bv_len = to_bytes(blksize);
				iovec[i].bv_offset =
					PAGE_SIZE - to_bytes(blksize);

				/* Mark page NULL here as it is not required anymore. */
				page = NULL;
			}

			continue;

		case BLKSIZE_4K:
		case BLKSIZE_8K:
			page = alloc_page(GFP_NOIO | __GFP_ZERO);
			if (unlikely(!page)) {
				pr_err("eio_alloc_wb_bvecs:" \
				       " System memory too low.\n");
				goto err;
			}
			iovec[i].bv_page = page;
			iovec[i].bv_offset = 0;
			iovec[i].bv_len = PAGE_SIZE;

			page = NULL;
			continue;
		}
	}

	goto out;

err:
	if (i != max) {
		if (i > 0)
			eio_free_wb_bvecs(bvec, i, blksize);
		ret = -ENOMEM;
	}

out:
	return ret;
}

int eio_alloc_wb_pages(struct page **pages, int max)
{
	int i, ret = 0;
	struct page *page;

	for (i = 0; i < max; i++) {
		page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (unlikely(!page)) {
			pr_err("alloc_wb_pages: System memory too low.\n");
			break;
		}
		pages[i] = page;
	}

	if (i != max) {
		if (i > 0)
			eio_free_wb_pages(pages, i);
		ret = -ENOMEM;
		goto out;
	}

out:
	return ret;
}

/*
 ****************************************************************************
 * struct bio_vec *eio_alloc_pages(int max_pages, int *page_count)
 * dmc : cache object
 * pages : bio_vec to be allocated for synchronous I/O.
 * page_count : total number of pages allocated.
 ****************************************************************************
 *
 * This function allocates pages capped to minimum of
 * MD_MAX_NR_PAGES OR maximun number of pages supported by
 * block device.
 * This is to ensure that the pages allocated should fit
 * into single bio request.
 */

struct bio_vec *eio_alloc_pages(u_int32_t max_pages, int *page_count)
{
	int pcount, i;
	struct bio_vec *pages;
	int nr_pages;

	/*
	 * Find out no. of pages supported by block device max capped to
	 * MD_MAX_NR_PAGES;
	 */
	nr_pages = min_t(u_int32_t, max_pages, MD_MAX_NR_PAGES);

	pages = kzalloc(nr_pages * sizeof(struct bio_vec), GFP_NOIO);
	if (unlikely(!pages)) {
		pr_err("eio_alloc_pages: System memory too low.\n");
		return NULL;
	}

	pcount = 0;
	for (i = 0; i < nr_pages; i++) {
		pages[i].bv_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (unlikely(!pages[i].bv_page)) {
			pr_err("eio_alloc_pages: System memory too low.\n");
			break;
		} else {
			pages[i].bv_len = PAGE_SIZE;
			pages[i].bv_offset = 0;
			pcount++;
		}
	}

	if (pcount == 0) {
		pr_err("Single page allocation failed. System memory too low.");
		kfree(pages);
		return NULL;
	}

	/* following can be commented out later...
	 * we may have less pages allocated.
	 */
	EIO_ASSERT(pcount == nr_pages);

	/* Set the return values here */
	*page_count = pcount;
	return pages;
}

/*
 * As part of reboot handling, stop all activies and mark the devices as
 * read only.
 */

int eio_reboot_handling(void)
{
	struct cache_c *dmc, *tempdmc = NULL;
	int i, error;
	uint32_t old_time_thresh;

	if (eio_reboot_notified == EIO_REBOOT_HANDLING_DONE)
		return 0;

	(void)wait_on_bit_lock_action((void *)&eio_control->synch_flags,
			       EIO_HANDLE_REBOOT, eio_wait_schedule,
			       TASK_UNINTERRUPTIBLE);
	if (eio_reboot_notified == EIO_REBOOT_HANDLING_DONE) {
		clear_bit(EIO_HANDLE_REBOOT, (void *)&eio_control->synch_flags);
		smp_mb__after_atomic();
		wake_up_bit((void *)&eio_control->synch_flags,
			    EIO_HANDLE_REBOOT);
		return 0;
	}
	EIO_ASSERT(eio_reboot_notified == 0);
	eio_reboot_notified = EIO_REBOOT_HANDLING_INPROG;

	for (i = 0; i < EIO_HASHTBL_SIZE; i++) {
		down_write(&eio_ttc_lock[i]);
		list_for_each_entry(dmc, &eio_ttc_list[i], cachelist) {

			kfree(tempdmc);
			tempdmc = NULL;
			if (unlikely(CACHE_FAILED_IS_SET(dmc)) ||
			    unlikely(CACHE_DEGRADED_IS_SET(dmc))) {
				pr_err
					("Cache \"%s\" is in failed/degraded " \
					"mode. Cannot mark cache read only.\n",
					dmc->cache_name);
				continue;
			}

			while (atomic64_read(&dmc->nr_ios) != 0) {
				pr_debug("rdonly: Draining I/O inflight\n");
				schedule_timeout(msecs_to_jiffies(10));
			}

			EIO_ASSERT(atomic64_read(&dmc->nr_ios) == 0);
			EIO_ASSERT(dmc->cache_rdonly == 0);

			/*
			 * Shutdown processing has the highest priority.
			 * Stop all ongoing activities.
			 */

			spin_lock_irqsave(&dmc->cache_spin_lock,
					  dmc->cache_spin_lock_flags);
			EIO_ASSERT(!
				   (dmc->
				    cache_flags & CACHE_FLAGS_SHUTDOWN_INPROG));
			dmc->cache_flags |= CACHE_FLAGS_SHUTDOWN_INPROG;
			spin_unlock_irqrestore(&dmc->cache_spin_lock,
					       dmc->cache_spin_lock_flags);

			/*
			 * Wait for ongoing edit/delete to complete.
			 */

			while (dmc->cache_flags & CACHE_FLAGS_MOD_INPROG) {
				up_write(&eio_ttc_lock[i]);
				schedule_timeout(msecs_to_jiffies(1));
				down_write(&eio_ttc_lock[i]);
			}
			if (dmc->cache_flags & CACHE_FLAGS_DELETED) {
				/*
				 * Cache got deleted. Free the dmc.
				 */

				tempdmc = dmc;
				continue;
			}
			old_time_thresh =
				dmc->sysctl_active.time_based_clean_interval;
			eio_stop_async_tasks(dmc);
			dmc->sysctl_active.time_based_clean_interval =
				old_time_thresh;

			dmc->cache_rdonly = 1;
			pr_info("Cache \"%s\" marked read only\n",
				dmc->cache_name);
			up_write(&eio_ttc_lock[i]);

			if (dmc->cold_boot && atomic64_read(&dmc->nr_dirty) &&
			    !eio_force_warm_boot) {
				pr_info
					("Cold boot set for cache %s: Draining dirty blocks: %llu",
					dmc->cache_name,
					(unsigned long long)atomic64_read(&dmc->nr_dirty));
				eio_clean_for_reboot(dmc);
			}

			error = eio_md_store(dmc);
			if (error)
				pr_err("Cannot mark cache \"%s\" read only\n",
				       dmc->cache_name);

			spin_lock_irqsave(&dmc->cache_spin_lock,
					  dmc->cache_spin_lock_flags);
			dmc->cache_flags &= ~CACHE_FLAGS_SHUTDOWN_INPROG;
			spin_unlock_irqrestore(&dmc->cache_spin_lock,
					       dmc->cache_spin_lock_flags);

			down_write(&eio_ttc_lock[i]);
		}
		kfree(tempdmc);
		tempdmc = NULL;
		up_write(&eio_ttc_lock[i]);
	}

	eio_reboot_notified = EIO_REBOOT_HANDLING_DONE;
	clear_bit(EIO_HANDLE_REBOOT, (void *)&eio_control->synch_flags);
	smp_mb__after_atomic();
	wake_up_bit((void *)&eio_control->synch_flags, EIO_HANDLE_REBOOT);
	return 0;
}

static int eio_overlap_split_bio(struct request_queue *q, struct bio *bio)
{
	int i, nbios;
	void **bioptr;
	sector_t snum;
	struct bio_container *bc;
	unsigned bvec_idx;
	unsigned bvec_consumed;

	nbios = EIO_BIO_BI_SIZE(bio) >> SECTOR_SHIFT;
	snum = EIO_BIO_BI_SECTOR(bio);
	bioptr = kmalloc(nbios * (sizeof(void *)), GFP_KERNEL);
	if (!bioptr) {
		EIO_BIO_ENDIO(bio, -ENOMEM);
		return 0;
	}
	bc = kmalloc(sizeof(struct bio_container), GFP_NOWAIT);
	if (!bc) {
		EIO_BIO_ENDIO(bio, -ENOMEM);
		kfree(bioptr);
		return 0;
	}

	atomic_set(&bc->bc_holdcount, nbios);
	bc->bc_bio = bio;
	bc->bc_error = 0;
	bvec_idx = EIO_BIO_BI_IDX(bio);
	bvec_consumed = 0;
	for (i = 0; i < nbios; i++) {
		bioptr[i] =
			eio_split_new_bio(bio, bc, &bvec_idx,
					&bvec_consumed, snum);
		if (!bioptr[i])
			break;
		snum++;
	}

	/* Error: cleanup */
	if (i < nbios) {
		for (i--; i >= 0; i--)
			bio_put(bioptr[i]);
		EIO_BIO_ENDIO(bio, -ENOMEM);
		kfree(bc);
		goto out;
	}

	for (i = 0; i < nbios; i++)
		eio_make_request_fn(q, bioptr[i]);

out:
	kfree(bioptr);
	return 0;
}

static struct bio *eio_split_new_bio(struct bio *bio, struct bio_container *bc,
				     unsigned *bvec_idx,
				     unsigned *bvec_consumed, sector_t snum)
{
	struct bio *cbio;
	unsigned iosize = 1 << SECTOR_SHIFT;

	cbio = bio_alloc(GFP_NOIO, 1);
	if (!cbio)
		return NULL;

	EIO_ASSERT(bio->bi_io_vec[*bvec_idx].bv_len >= iosize);

	if (bio->bi_io_vec[*bvec_idx].bv_len <= *bvec_consumed) {
		EIO_ASSERT(bio->bi_io_vec[*bvec_idx].bv_len == *bvec_consumed);
		(*bvec_idx)++;
		EIO_ASSERT(bio->bi_vcnt > *bvec_idx);
		*bvec_consumed = 0;
	}

	cbio->bi_io_vec[0].bv_page = bio->bi_io_vec[*bvec_idx].bv_page;
	cbio->bi_io_vec[0].bv_offset =
		bio->bi_io_vec[*bvec_idx].bv_offset + *bvec_consumed;
	cbio->bi_io_vec[0].bv_len = iosize;
	*bvec_consumed += iosize;

	EIO_BIO_BI_SECTOR(cbio) = snum;
	EIO_BIO_BI_SIZE(cbio) = iosize;
	cbio->bi_bdev = bio->bi_bdev;
	cbio->bi_opf = bio->bi_opf;
	cbio->bi_vcnt = 1;
	EIO_BIO_BI_IDX(cbio) = 0;
	cbio->bi_end_io = eio_split_endio;
	cbio->bi_private = bc;
	return cbio;
}

static void eio_split_endio(struct bio *bio, int error)
{
	struct bio_container *bc = bio->bi_private;

	EIO_ENDIO_FN_START;

	if (error)
		bc->bc_error = error;
	bio_put(bio);
	if (atomic_dec_and_test(&bc->bc_holdcount)) {
		EIO_BIO_ENDIO(bc->bc_bio, bc->bc_error);
		kfree(bc);
	}
	return;
}
