
#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <libaio.h>

#ifdef CONFIG_VALGRIND
#include <valgrind/memcheck.h>
#endif

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/kthread.h>

#include "tools-util.h"

struct fops {
	void (*init)(void);
	void (*cleanup)(void);
	void (*read)(struct bio *bio, struct iovec * iov, unsigned i);
	void (*write)(struct bio *bio, struct iovec * iov, unsigned i);
};

static struct fops *fops;
static io_context_t aio_ctx;
static atomic_t running_requests;

void generic_make_request(struct bio *bio)
{
	struct iovec *iov;
	struct bvec_iter iter;
	struct bio_vec bv;
	ssize_t ret;
	unsigned i;

	if (bio->bi_opf & REQ_PREFLUSH) {
		ret = fdatasync(bio->bi_bdev->bd_fd);
		if (ret) {
			fprintf(stderr, "fsync error: %m\n");
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
			return;
		}
	}

	i = 0;
	bio_for_each_segment(bv, bio, iter)
		i++;

	iov = alloca(sizeof(*iov) * i);

	i = 0;
	bio_for_each_segment(bv, bio, iter) {
		void *start = page_address(bv.bv_page) + bv.bv_offset;
		size_t len = bv.bv_len;

		iov[i++] = (struct iovec) {
			.iov_base = start,
			.iov_len = len,
		};

#ifdef CONFIG_VALGRIND
		/* To be pedantic it should only be on IO completion. */
		if (bio_op(bio) == REQ_OP_READ)
			VALGRIND_MAKE_MEM_DEFINED(start, len);
#endif
	}

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		fops->read(bio, iov, i);
		break;
	case REQ_OP_WRITE:
		fops->write(bio, iov, i);
		break;
	case REQ_OP_FLUSH:
		ret = fsync(bio->bi_bdev->bd_fd);
		if (ret)
			die("fsync error: %m");
		bio_endio(bio);
		break;
	default:
		BUG();
	}
}

static void submit_bio_wait_endio(struct bio *bio)
{
	complete(bio->bi_private);
}

int submit_bio_wait(struct bio *bio)
{
	struct completion done;

	init_completion(&done);
	bio->bi_private = &done;
	bio->bi_end_io = submit_bio_wait_endio;
	bio->bi_opf |= REQ_SYNC;
	submit_bio(bio);
	wait_for_completion(&done);

	return blk_status_to_errno(bio->bi_status);
}

int blkdev_issue_discard(struct block_device *bdev,
			 sector_t sector, sector_t nr_sects,
			 gfp_t gfp_mask)
{
	return 0;
}

int blkdev_issue_zeroout(struct block_device *bdev,
			 sector_t sector, sector_t nr_sects,
			 gfp_t gfp_mask, unsigned flags)
{
	/* Not yet implemented: */
	BUG();
}

unsigned bdev_logical_block_size(struct block_device *bdev)
{
	struct stat statbuf;
	unsigned blksize;
	int ret;

	ret = fstat(bdev->bd_fd, &statbuf);
	BUG_ON(ret);

	if (!S_ISBLK(statbuf.st_mode))
		return statbuf.st_blksize;

	xioctl(bdev->bd_fd, BLKPBSZGET, &blksize);
	return blksize;
}

sector_t get_capacity(struct gendisk *disk)
{
	struct block_device *bdev =
		container_of(disk, struct block_device, __bd_disk);
	struct stat statbuf;
	u64 bytes;
	int ret;

	ret = fstat(bdev->bd_fd, &statbuf);
	BUG_ON(ret);

	if (!S_ISBLK(statbuf.st_mode))
		return statbuf.st_size >> 9;

	ret = ioctl(bdev->bd_fd, BLKGETSIZE64, &bytes);
	BUG_ON(ret);

	return bytes >> 9;
}

void blkdev_put(struct block_device *bdev, fmode_t mode)
{
	fdatasync(bdev->bd_fd);
	close(bdev->bd_sync_fd);
	close(bdev->bd_fd);
	free(bdev);
}

struct block_device *blkdev_get_by_path(const char *path, fmode_t mode,
					void *holder)
{
	struct block_device *bdev;
	int fd, sync_fd, buffered_fd, flags = 0;

	if ((mode & (FMODE_READ|FMODE_WRITE)) == (FMODE_READ|FMODE_WRITE))
		flags = O_RDWR;
	else if (mode & FMODE_READ)
		flags = O_RDONLY;
	else if (mode & FMODE_WRITE)
		flags = O_WRONLY;

	if (!(mode & FMODE_BUFFERED))
		flags |= O_DIRECT;

#if 0
	/* using O_EXCL doesn't work with opening twice for an O_SYNC fd: */
	if (mode & FMODE_EXCL)
		flags |= O_EXCL;
#endif
	buffered_fd = open(path, flags & ~O_DIRECT);
	if (buffered_fd < 0)
		return ERR_PTR(-errno);

	fd = open(path, flags);
	if (fd < 0)
		fd = dup(buffered_fd);
	if (fd < 0) {
		close(buffered_fd);
		return ERR_PTR(-errno);
	}

	sync_fd	= open(path, flags|O_SYNC);
	if (sync_fd < 0)
		sync_fd	= open(path, (flags & ~O_DIRECT)|O_SYNC);
	if (sync_fd < 0) {
		close(fd);
		close(buffered_fd);
		return ERR_PTR(-errno);
	}

	bdev = malloc(sizeof(*bdev));
	memset(bdev, 0, sizeof(*bdev));

	strncpy(bdev->name, path, sizeof(bdev->name));
	bdev->name[sizeof(bdev->name) - 1] = '\0';

	bdev->bd_dev		= xfstat(fd).st_rdev;
	bdev->bd_fd		= fd;
	bdev->bd_sync_fd	= sync_fd;
	bdev->bd_buffered_fd	= buffered_fd;
	bdev->bd_holder		= holder;
	bdev->bd_disk		= &bdev->__bd_disk;
	bdev->bd_disk->bdi	= &bdev->bd_disk->__bdi;
	bdev->queue.backing_dev_info = bdev->bd_disk->bdi;

	return bdev;
}

void bdput(struct block_device *bdev)
{
	BUG();
}

int lookup_bdev(const char *path, dev_t *dev)
{
	return -EINVAL;
}

static void io_fallback(void)
{
	fops++;
	if (fops->init == NULL)
		die("no fallback possible, something is very wrong");
	fops->init();
}

static void sync_check(struct bio *bio, int ret)
{
	if (ret != bio->bi_iter.bi_size) {
		die("IO error: %s\n", strerror(-ret));
	}

	if (bio->bi_opf & REQ_FUA) {
		ret = fdatasync(bio->bi_bdev->bd_fd);
		if (ret)
			die("fsync error: %s\n", strerror(-ret));
	}
	bio_endio(bio);
}

static void sync_init(void) {}

static void sync_cleanup(void)
{
	/* not necessary? */
	sync();
}

static void sync_read(struct bio *bio, struct iovec * iov, unsigned i)
{

	int fd = bio->bi_opf & REQ_FUA
			? bio->bi_bdev->bd_sync_fd
			: bio->bi_bdev->bd_fd;
	ssize_t ret = preadv(fd, iov, i, bio->bi_iter.bi_sector << 9);
	sync_check(bio, ret);
}

static void sync_write(struct bio *bio, struct iovec * iov, unsigned i)
{
	int fd = bio->bi_opf & REQ_FUA
			? bio->bi_bdev->bd_sync_fd
			: bio->bi_bdev->bd_fd;
	ssize_t ret = pwritev(fd, iov, i, bio->bi_iter.bi_sector << 9);
	sync_check(bio, ret);
}

static int aio_completion_thread(void *arg)
{
	struct io_event events[8], *ev;
	int ret;
	bool stop = false;

	while (!stop) {
		ret = io_getevents(aio_ctx, 1, ARRAY_SIZE(events),
				   events, NULL);

		if (ret < 0 && ret == -EINTR)
			continue;
		if (ret < 0)
			die("io_getevents() error: %s", strerror(-ret));

		for (ev = events; ev < events + ret; ev++) {
			struct bio *bio = (struct bio *) ev->data;

			/* This should only happen during blkdev_cleanup() */
			if (!bio) {
				BUG_ON(atomic_read(&running_requests) != 0);
				stop = true;
				continue;
			}

			if (ev->res != bio->bi_iter.bi_size)
				bio->bi_status = BLK_STS_IOERR;

			bio_endio(bio);
			atomic_dec(&running_requests);
		}
	}

	return 0;
}

static struct task_struct *aio_task = NULL;

static void aio_init(void)
{
	struct task_struct *p;
	long err = io_setup(256, &aio_ctx);
	if (!err) {
		p = kthread_run(aio_completion_thread, NULL, "aio_completion");
		BUG_ON(IS_ERR(p));

		aio_task = p;

	} else if (err == -ENOSYS) {
		io_fallback();
	} else {
		die("io_setup() error: %s", strerror(err));
	}
}

static void aio_cleanup(void)
{
	struct task_struct *p = NULL;
	swap(aio_task, p);
	get_task_struct(p);

	/* I mean, really?! IO_CMD_NOOP is even defined, but not implemented. */
	int fds[2];
	int ret = pipe(fds);
	if (ret != 0)
		die("pipe err: %s", strerror(ret));

	/* Wake up the completion thread with spurious work. */
	int junk = 0;
	struct iocb iocb = {
		.aio_lio_opcode = IO_CMD_PWRITE,
		.data = NULL, /* Signal to stop */
		.aio_fildes = fds[1],
		.u.c.buf = &junk,
		.u.c.nbytes = 1,
	}, *iocbp = &iocb;
	ret = io_submit(aio_ctx, 1, &iocbp);
	if (ret != 1)
		die("io_submit cleanup err: %s", strerror(-ret));

	ret = kthread_stop(p);
	BUG_ON(ret);

	put_task_struct(p);

	close(fds[0]);
	close(fds[1]);
}

static void aio_op(struct bio *bio, struct iovec *iov, unsigned i, int opcode)
{
	ssize_t ret;
	struct iocb iocb = {
		.data		= bio,
		.aio_fildes	= bio->bi_opf & REQ_FUA
			? bio->bi_bdev->bd_sync_fd
			: bio->bi_bdev->bd_fd,
		.aio_lio_opcode	= opcode,
		.u.c.buf        = iov,
		.u.c.nbytes     = i,
		.u.c.offset     = bio->bi_iter.bi_sector << 9,

	}, *iocbp = &iocb;

	atomic_inc(&running_requests);
	ret = io_submit(aio_ctx, 1, &iocbp);
	if (ret != 1)
		die("io_submit err: %s", strerror(-ret));
}

static void aio_read(struct bio *bio, struct iovec *iov, unsigned i)
{
	aio_op(bio, iov, i, IO_CMD_PREADV);
}

static void aio_write(struct bio *bio, struct iovec * iov, unsigned i)
{
	aio_op(bio, iov, i, IO_CMD_PWRITEV);
}


/* not implemented */
static void uring_init(void) {
	io_fallback();
}

struct fops fops_list[] = {
	{
		.init		= uring_init,
	}, {
		.init		= aio_init,
		.cleanup	= aio_cleanup,
		.read		= aio_read,
		.write		= aio_write,
	}, {
		.init		= sync_init,
		.cleanup	= sync_cleanup,
		.read		= sync_read,
		.write		= sync_write,
	}, {
		/* NULL */
	}
};

__attribute__((constructor(102)))
static void blkdev_init(void)
{
	fops = fops_list;
	fops->init();
}

__attribute__((destructor(102)))
static void blkdev_cleanup(void)
{
	fops->cleanup();
}
