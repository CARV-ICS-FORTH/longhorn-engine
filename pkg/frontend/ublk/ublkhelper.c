// mystruct.c
#define _GNU_SOURCE
#include "ublkhelper.h"
#include <assert.h>
#include <fcntl.h> //for open fd
#include <unistd.h> // for close fd
#include <syslog.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>
#include <syscall.h>
#include <sys/mman.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "ublksrv_tgt_endian.h"

#define	CTRL_DEV	"/dev/ublk-control"
#define CTRL_CMD_HAS_DATA	1
#define CTRL_CMD_HAS_BUF	2
#define CTRL_CMD_NO_TRANS	4

const struct ublksrv_ctrl_dev *ublksrv_get_ctrl_dev(
		const struct ublksrv_dev *dev)
{
	return tdev_to_local(dev)->ctrl_dev;
}

static inline void ublksrv_setup_ring_params(struct io_uring_params *p,
		int cq_depth, unsigned flags) {
	memset(p, 0, sizeof(*p));
	p->flags = flags | IORING_SETUP_CQSIZE;
	p->cq_entries = cq_depth;
}

struct ublksrv_ctrl_dev *ublksrv_ctrl_init(struct ublksrv_dev_data *data) {
	struct io_uring_params p;
	struct ublksrv_ctrl_dev *dev = (struct ublksrv_ctrl_dev *)calloc(1,
			sizeof(*dev));
	struct ublksrv_ctrl_dev_info *info = &dev->dev_info;
	int ret;

	dev->ctrl_fd = open(CTRL_DEV, O_RDWR);
	if (dev->ctrl_fd < 0) {
		fprintf(stderr, "control dev %s can't be opened: %m\n", CTRL_DEV);
		exit(dev->ctrl_fd);
	}

	/* -1 means we ask ublk driver to allocate one free to us */
	info->dev_id = data->dev_id;
	info->nr_hw_queues = data->nr_hw_queues;
	info->queue_depth = data->queue_depth;
	info->max_io_buf_bytes = data->max_io_buf_bytes;
	info->flags = data->flags;
	info->ublksrv_flags = data->ublksrv_flags;

	dev->run_dir = data->run_dir;
	dev->tgt_type = data->tgt_type;
	dev->tgt_ops = data->tgt_ops;
	dev->tgt_argc = data->tgt_argc;
	dev->tgt_argv = data->tgt_argv;

	/* 32 is enough to send ctrl commands */
	ublksrv_setup_ring_params(&p, 32, IORING_SETUP_SQE128);
	ret = io_uring_queue_init_params(32, &dev->ring, &p);
	if (ret < 0) {
		fprintf(stderr, "queue_init: %s\n", strerror(-ret));
		free(dev);
		return NULL;
	}

	return dev;
}

static inline void *ublksrv_get_sqe_cmd(struct io_uring_sqe *sqe)
{
	return (void *)&sqe->addr3;
}
static unsigned int legacy_op_to_ioctl(unsigned int op)
{
	assert(_IOC_TYPE(op) == 0);
	assert(_IOC_DIR(op) == 0);
	assert(_IOC_SIZE(op) == 0);
	assert(op >= UBLK_CMD_GET_QUEUE_AFFINITY &&
			op <= UBLK_CMD_GET_DEV_INFO2);

	return ctrl_cmd_op[op];
}

static inline void ublksrv_set_sqe_cmd_op(struct io_uring_sqe *sqe, __u32 cmd_op)
{
	__u32 *addr = (__u32 *)&sqe->off;

	addr[0] = cmd_op;
	addr[1] = 0;
}


static inline void ublksrv_ctrl_init_cmd(struct ublksrv_ctrl_dev *dev,
		struct io_uring_sqe *sqe,
		struct ublksrv_ctrl_cmd_data *data)
{
	struct ublksrv_ctrl_dev_info *info = &dev->dev_info;
	struct ublksrv_ctrl_cmd *cmd = (struct ublksrv_ctrl_cmd *)ublksrv_get_sqe_cmd(sqe);
	unsigned int cmd_op = data->cmd_op;

	sqe->fd = dev->ctrl_fd;
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->ioprio = 0;

	if (data->flags & CTRL_CMD_HAS_BUF) {
		cmd->addr = data->addr;
		cmd->len = data->len;
	}

	if (data->flags & CTRL_CMD_HAS_DATA) {
		cmd->data[0] = data->data[0];
		cmd->dev_path_len = data->dev_path_len;
	}

	cmd->dev_id = info->dev_id;
	cmd->queue_id = -1;

	if (!(data->flags & CTRL_CMD_NO_TRANS) &&
			(info->flags & UBLK_F_CMD_IOCTL_ENCODE))
		cmd_op = legacy_op_to_ioctl(cmd_op);
	ublksrv_set_sqe_cmd_op(sqe, cmd_op);

	io_uring_sqe_set_data(sqe, cmd);
}

static int __ublksrv_ctrl_cmd(struct ublksrv_ctrl_dev *dev,
		struct ublksrv_ctrl_cmd_data *data)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret = -EINVAL;

	sqe = io_uring_get_sqe(&dev->ring);
	if (!sqe) {
		fprintf(stderr, "can't get sqe ret %d\n", ret);
		fflush(stdout);
		return ret;
	}

	ublksrv_ctrl_init_cmd(dev, sqe, data);

	ret = io_uring_submit(&dev->ring);
	if (ret < 0) {
		fprintf(stderr, "uring submit ret %d\n", ret);
		fflush(stdout);
		return ret;
	}

	do {
		ret = io_uring_wait_cqe(&dev->ring, &cqe);
	} while (ret == -EINTR);
	if (ret < 0) {
		fprintf(stderr, "wait cqe: %s\n", strerror(-ret));
		fflush(stdout);
		return ret;
	}
	io_uring_cqe_seen(&dev->ring, cqe);

	printf("dev %d, ctrl cqe res %d, user_data %llx\n",
			dev->dev_info.dev_id, cqe->res, cqe->user_data);
			fflush(stdout);
	return cqe->res;
}

int ublksrv_ctrl_set_params(struct ublksrv_ctrl_dev *dev,
		struct ublk_params *params)
{
	struct ublksrv_ctrl_cmd_data data = {
		.cmd_op	= UBLK_CMD_SET_PARAMS,
		.flags	= CTRL_CMD_HAS_BUF,
		.addr = (__u64)params,
		.len = sizeof(*params),
	};
	char buf[UBLKC_PATH_MAX + sizeof(*params)];

	params->len = sizeof(*params);


	return __ublksrv_ctrl_cmd(dev, &data);
}

static int __ublksrv_ctrl_add_dev(struct ublksrv_ctrl_dev *dev, unsigned cmd_op)
{
	struct ublksrv_ctrl_cmd_data data = {
		.cmd_op	= cmd_op,
		.flags	= CTRL_CMD_HAS_BUF | CTRL_CMD_NO_TRANS,
		.addr = (__u64)&dev->dev_info,
		.len = sizeof(struct ublksrv_ctrl_dev_info),
	};

	return __ublksrv_ctrl_cmd(dev, &data);
}


int ublksrv_ctrl_add_dev(struct ublksrv_ctrl_dev *dev)
{

	int ret = __ublksrv_ctrl_add_dev(dev, UBLK_U_CMD_ADD_DEV);

	if (ret < 0)
		return __ublksrv_ctrl_add_dev(dev, UBLK_CMD_ADD_DEV);

	return ret;
}

const struct ublksrv_ctrl_dev_info *ublksrv_ctrl_get_dev_info(
		const struct ublksrv_ctrl_dev *dev)
{
	return &dev->dev_info;
}

static inline bool ublk_is_unprivileged(const struct ublksrv_ctrl_dev *ctrl_dev)
{
	return !!(ctrl_dev->dev_info.flags & UBLK_F_UNPRIVILEGED_DEV);
}

int ublksrv_ctrl_get_affinity(struct ublksrv_ctrl_dev *ctrl_dev)
{
	struct ublksrv_ctrl_cmd_data data = {
		.cmd_op	= UBLK_CMD_GET_QUEUE_AFFINITY,
		.flags	= CTRL_CMD_HAS_DATA | CTRL_CMD_HAS_BUF,
	};
	unsigned char *buf;
	int i, ret;
	int len;
	int path_len;

	if (ublk_is_unprivileged(ctrl_dev))
		path_len = UBLKC_PATH_MAX;
	else
		path_len = 0;

	len = (sizeof(cpu_set_t) + path_len) * ctrl_dev->dev_info.nr_hw_queues;
	buf = malloc(len);

	if (!buf)
		return -ENOMEM;

	for (i = 0; i < ctrl_dev->dev_info.nr_hw_queues; i++) {
		data.data[0] = i;
		data.dev_path_len = path_len;
		data.len = sizeof(cpu_set_t) + path_len;
		data.addr = (__u64)&buf[i * data.len];

		if (path_len)
			snprintf((char *)data.addr, UBLKC_PATH_MAX, "%s%d",
					UBLKC_DEV, ctrl_dev->dev_info.dev_id);

		ret = __ublksrv_ctrl_cmd(ctrl_dev, &data);
		if (ret < 0) {
			free(buf);
			return ret;
		}
	}
	ctrl_dev->queues_cpuset = (cpu_set_t *)buf;

	return 0;
}

int create_pid_file(const char *pid_file, int *pid_fd)
{
#define PID_PATH_LEN  256
	char buf[PID_PATH_LEN];
	int fd, ret;

	fd = open(pid_file, O_RDWR | O_CREAT | O_CLOEXEC,
			S_IRUSR | S_IWUSR);
	if (fd < 0) {
		printf( "Fail to open file %s", pid_file);
		return fd;
	}

	ret = ftruncate(fd, 0);
	if (ret == -1) {
		printf( "Could not truncate pid file %s, err %s",
				pid_file, strerror(errno));
		goto fail;
	}

	snprintf(buf, PID_PATH_LEN, "%ld\n", (long) getpid());
	if (write(fd, buf, strlen(buf)) != strlen(buf)) {
		printf( "Fail to write %s to file %s",
				buf, pid_file);
		ret = -1;
	} else {
		*pid_fd = fd;
	}
 fail:
	if (ret) {
		close(fd);
		unlink(pid_file);
	}
	return ret;
}

static int ublksrv_create_pid_file(struct _ublksrv_dev *dev)
{
	int dev_id = dev->ctrl_dev->dev_info.dev_id;
	char pid_file[64];
	int ret, pid_fd;

	if (!dev->ctrl_dev->run_dir)
		return 0;

	/* create pid file and lock it, so that others can't */
	snprintf(pid_file, 64, "%s/%d.pid", dev->ctrl_dev->run_dir, dev_id);

	ret = create_pid_file(pid_file, &pid_fd);
	if (ret < 0) {
		/* -1 means the file is locked, and we need to remove it */
		if (ret == -1) {
			close(pid_fd);
			unlink(pid_file);
		}
		return ret;
	}
	dev->pid_file_fd = pid_fd;
	return 0;
}

const struct ublksrv_dev *ublksrv_dev_init(const struct ublksrv_ctrl_dev *ctrl_dev)
{
	int dev_id = ctrl_dev->dev_info.dev_id;
	char buf[64];
	int ret = -1;
	struct _ublksrv_dev *dev = (struct _ublksrv_dev *)calloc(1, sizeof(*dev));
	struct ublksrv_tgt_info *tgt;

	if (!dev)
		return local_to_tdev(dev);

	tgt = &dev->tgt;
	dev->ctrl_dev = ctrl_dev;
	dev->cdev_fd = -1;

	snprintf(buf, 64, "%s%d", UBLKC_DEV, dev_id);
	dev->cdev_fd = open(buf, O_RDWR | O_NONBLOCK);
	if (dev->cdev_fd < 0) {
		printf("can't open %s, ret %d\n", buf, dev->cdev_fd);
		goto fail;
	}

	tgt->fds[0] = dev->cdev_fd;
    ret =0;

	if (ret) {
		printf( "can't init tgt %d/%s/%d, ret %d\n",
				dev_id, ctrl_dev->tgt_type, ctrl_dev->tgt_argc,
				ret);
		goto fail;
	}

	ret = ublksrv_create_pid_file(dev);
	if (ret) {
		printf( "can't create pid file for dev %d, ret %d\n",
				dev_id, ret);
		goto fail;
	}

	return local_to_tdev(dev);
fail:
	//ublksrv_dev_deinit(local_to_tdev(dev)); //TODO delete dev
	return NULL;
}


int init_params(struct ublksrv_ctrl_dev *dev,struct ublksrv_dev_data *data){

struct ublk_params params;
    const struct ublksrv_ctrl_dev_info *info = ublksrv_ctrl_get_dev_info(dev);

    // Clear the entire struct to zero first (optional but safe)
    memset(&params, 0, sizeof(params));

    // Set the type flags
    params.types = UBLK_PARAM_TYPE_BASIC | UBLK_PARAM_TYPE_DISCARD;

    // Fill in the basic params
    params.basic.attrs = 0U;
    params.basic.logical_bs_shift = 9;
    params.basic.physical_bs_shift = 12;
    params.basic.io_opt_shift = 12;
    params.basic.io_min_shift = 9;
    params.basic.max_sectors = info->max_io_buf_bytes >> 9;

    // Hard-code 1GB in sectors (512 bytes per sector, so 1GB / 512)
    params.basic.dev_sectors = (1ULL << 30) >> 9;

    // Fill in discard params
    params.discard.discard_granularity = 1U << 9;
    params.discard.max_discard_sectors = UINT_MAX >> 9;
    params.discard.max_discard_segments = 1;

    // Set length of the params struct
    params.len = sizeof(params);

    // Pass to ublksrv
    ublksrv_ctrl_set_params(dev, &params);
}

#define ublk_un_privileged_prep_data(dev, data)	 \
	char buf[UBLKC_PATH_MAX];			\
	if (ublk_is_unprivileged(dev)) {			\
		snprintf(buf, UBLKC_PATH_MAX, "%s%d", UBLKC_DEV, \
			dev->dev_info.dev_id);			\
		data.flags |= CTRL_CMD_HAS_BUF | CTRL_CMD_HAS_DATA;	\
		data.len = sizeof(buf);	\
		data.dev_path_len = UBLKC_PATH_MAX;	\
		data.addr = (__u64)buf;	\
	}

int ublksrv_ctrl_start_dev(struct ublksrv_ctrl_dev *ctrl_dev,
		int daemon_pid)
{
	struct ublksrv_ctrl_cmd_data data = {
		.cmd_op	= UBLK_CMD_START_DEV,
		.flags	= CTRL_CMD_HAS_DATA,
	};
	int ret;

	ublk_un_privileged_prep_data(ctrl_dev, data);

	ctrl_dev->dev_info.ublksrv_pid = data.data[0] = daemon_pid;

	ret = __ublksrv_ctrl_cmd(ctrl_dev, &data);

	return ret;
}
static void ublksrv_calculate_depths(const struct _ublksrv_dev *dev, int
		*ring_depth, int *cq_depth, int *nr_ios)
{
	const struct ublksrv_ctrl_dev *cdev = dev->ctrl_dev;

	/*
	 * eventfd consumes one extra sqe, and it can be thought as one target
	 * depth
	 */
	int aio_depth = (cdev->dev_info.ublksrv_flags & UBLKSRV_F_NEED_EVENTFD)
		? 1 : 0;
	int depth = cdev->dev_info.queue_depth;
	int tgt_depth = dev->tgt.tgt_ring_depth + aio_depth;

	*nr_ios = depth + dev->tgt.extra_ios;

	/*
	 * queue_depth represents the max count of io commands issued from ublk driver.
	 *
	 * After io command is fetched from ublk driver, the consumed sqe for
	 * fetching io command has been available for target usage, so the uring
	 * depth can be set as the max(queue_depth, tgt_depth).
	 */
	depth = depth > tgt_depth ? depth : tgt_depth;
	*ring_depth = depth;
	*cq_depth = dev->cq_depth ? dev->cq_depth : depth;
}

static inline int ublksrv_gettid(void)
{
	return syscall(SYS_gettid);
}

static int ublksrv_queue_cmd_buf_sz(struct _ublksrv_queue *q)
{
	int size =  q->q_depth * sizeof(struct ublksrv_io_desc);
	unsigned int page_sz = getpagesize();

	return round_up(size, page_sz);
}

static int queue_max_cmd_buf_sz(void)
{
	unsigned int page_sz = getpagesize();

	return round_up(UBLK_MAX_QUEUE_DEPTH * sizeof(struct ublksrv_io_desc),
			page_sz);
}

static inline struct ublksrv_io_desc *ublksrv_get_iod(
		const struct _ublksrv_queue *q, int tag)
{
        return (struct ublksrv_io_desc *)
                &(q->io_cmd_buf[tag * sizeof(struct ublksrv_io_desc)]);
}

static inline __u64 build_user_data(unsigned tag, unsigned op,
		unsigned tgt_data, unsigned is_target_io)
{
	assert(!(tag >> 16) && !(op >> 8) && !(tgt_data >> 16));

	return tag | (op << 16) | (tgt_data << 24) | (__u64)is_target_io << 63;
}

static void ublksrv_queue_adjust_uring_io_wq_workers(struct _ublksrv_queue *q)
{
	struct _ublksrv_dev *dev = q->dev;
	unsigned int val[2] = {0, 0};
	int ret;

	if (!dev->tgt.iowq_max_workers[0] && !dev->tgt.iowq_max_workers[1])
		return;

	ret = io_uring_register_iowq_max_workers(&q->ring, val);
	if (ret)
		printf("%s: register iowq max workers failed %d\n",
				__func__, ret);

	if (!dev->tgt.iowq_max_workers[0])
		dev->tgt.iowq_max_workers[0] = val[0];
	if (!dev->tgt.iowq_max_workers[1])
		dev->tgt.iowq_max_workers[1] = val[1];

	ret = io_uring_register_iowq_max_workers(&q->ring,
			dev->tgt.iowq_max_workers);
	if (ret)
		printf("%s: register iowq max workers failed %d\n",
				__func__, ret);
}

static inline cpu_set_t *ublksrv_get_queue_affinity(
		const struct ublksrv_ctrl_dev *dev, int qid)
{
	unsigned char *buf = (unsigned char *)&dev->queues_cpuset[qid];

	if (ublk_is_unprivileged(dev))
		return (cpu_set_t *)&buf[UBLKC_PATH_MAX];

	return &dev->queues_cpuset[qid];
}

static void ublksrv_set_sched_affinity(struct _ublksrv_dev *dev,
		unsigned short q_id)
{
	const struct ublksrv_ctrl_dev *cdev = dev->ctrl_dev;
	unsigned dev_id = cdev->dev_info.dev_id;
	cpu_set_t *cpuset = ublksrv_get_queue_affinity(cdev, q_id);

	if (sched_setaffinity(0, sizeof(cpu_set_t), cpuset) < 0)
		printf("ublk dev %u queue %u set affinity failed",
				dev_id, q_id);
}

static inline int ublksrv_queue_io_cmd(struct _ublksrv_queue *q,
		struct ublk_io *io, unsigned tag)
{
    pthread_mutex_lock(&q->lock);
	struct ublksrv_io_cmd *cmd;
	struct io_uring_sqe *sqe;
	unsigned int cmd_op = 0;
	__u64 user_data;

	/* only freed io can be issued */
	if (!(io->flags & UBLKSRV_IO_FREE))
		return 0;

	/* we issue because we need either fetching or committing */
	if (!(io->flags &
		(UBLKSRV_NEED_FETCH_RQ | UBLKSRV_NEED_GET_DATA |
		 UBLKSRV_NEED_COMMIT_RQ_COMP)))
		return 0;

	if (io->flags & UBLKSRV_NEED_GET_DATA)
		cmd_op = UBLK_IO_NEED_GET_DATA;
	else if (io->flags & UBLKSRV_NEED_COMMIT_RQ_COMP)
		cmd_op = UBLK_IO_COMMIT_AND_FETCH_REQ;
	else if (io->flags & UBLKSRV_NEED_FETCH_RQ)
		cmd_op = UBLK_IO_FETCH_REQ;

	sqe = io_uring_get_sqe(&q->ring);
	if (!sqe) {
		printf("%s: run out of sqe %d, tag %d\n",
				__func__, q->q_id, tag);
		return -1;
	}

	cmd = (struct ublksrv_io_cmd *)ublksrv_get_sqe_cmd(sqe);

	if (cmd_op == UBLK_IO_COMMIT_AND_FETCH_REQ)
		cmd->result = io->result;

	if (q->state & UBLKSRV_QUEUE_IOCTL_OP)
		cmd_op = _IOWR('u', _IOC_NR(cmd_op), struct ublksrv_io_cmd);

	/* These fields should be written once, never change */
	ublksrv_set_sqe_cmd_op(sqe, cmd_op);
	sqe->fd		= 0;	/*dev->cdev_fd*/
	sqe->opcode	=  IORING_OP_URING_CMD;
	sqe->flags	= IOSQE_FIXED_FILE;
	sqe->rw_flags	= 0;
	cmd->tag	= tag;
	if (!(q->state & UBLKSRV_USER_COPY))
		cmd->addr	= (__u64)io->buf_addr;
	else
		cmd->addr	= 0;
	cmd->q_id	= q->q_id;

	user_data = build_user_data(tag, _IOC_NR(cmd_op), 0, 0);
	io_uring_sqe_set_data64(sqe, user_data);

	io->flags = 0;

    if(atomic_fetch_sub(&q->tgt_io_inflight,1) == 1) {
        pthread_mutex_lock(&q->cond_mutex);
        pthread_cond_signal(&q->cond);
        pthread_mutex_unlock(&q->cond_mutex);
    }
        pthread_mutex_unlock(&q->lock);

	return 1;
}

static void ublksrv_submit_fetch_commands(struct _ublksrv_queue *q)
{
	int i = 0;

	for (i = 0; i < q->q_depth; i++)
		ublksrv_queue_io_cmd(q, &q->ios[i], i);

}

const struct ublksrv_queue *ublksrv_queue_init(const struct ublksrv_dev *tdev,
		unsigned short q_id, void *queue_data)
{
	struct io_uring_params p;
	struct _ublksrv_dev *dev = tdev_to_local(tdev);
	struct _ublksrv_queue *q;
	const struct ublksrv_ctrl_dev *ctrl_dev = dev->ctrl_dev;
	int depth = ctrl_dev->dev_info.queue_depth;
	int i, ret = -1;
	int cmd_buf_size, io_buf_size;
	unsigned long off;
	int io_data_size = round_up(dev->tgt.io_data_size,
			sizeof(unsigned long));
	int ring_depth, cq_depth, nr_ios;

	ublksrv_calculate_depths(dev, &ring_depth, &cq_depth, &nr_ios);

	/*
	 * Too many extra ios
	 */
	if (nr_ios > depth * 3)
		return NULL;

	q = (struct _ublksrv_queue *)malloc(sizeof(struct _ublksrv_queue) +
			sizeof(struct ublk_io) * nr_ios);
	dev->__queues[q_id] = q;

	q->tgt_ops = dev->tgt.ops;	//cache ops for fast path
	q->dev = dev;
	if (ctrl_dev->dev_info.flags & UBLK_F_CMD_IOCTL_ENCODE)
		q->state = UBLKSRV_QUEUE_IOCTL_OP;
	else
		q->state = 0;
	if (ctrl_dev->dev_info.flags & UBLK_F_USER_COPY)
		q->state |= UBLKSRV_USER_COPY;
	q->q_id = q_id;
	/* FIXME: depth has to be PO 2 */
	q->q_depth = depth;
	q->io_cmd_buf = NULL;
	q->tid = ublksrv_gettid();

	cmd_buf_size = ublksrv_queue_cmd_buf_sz(q);
	off = UBLKSRV_CMD_BUF_OFFSET + q_id * queue_max_cmd_buf_sz();
	q->io_cmd_buf = (char *)mmap(0, cmd_buf_size, PROT_READ,
			MAP_SHARED | MAP_POPULATE, dev->cdev_fd, off);
	if (q->io_cmd_buf == MAP_FAILED) {
		printf("ublk dev %d queue %d map io_cmd_buf failed",
				q->dev->ctrl_dev->dev_info.dev_id, q->q_id);
		goto fail;
	}

	io_buf_size = ctrl_dev->dev_info.max_io_buf_bytes;
	for (i = 0; i < nr_ios; i++) {
		q->ios[i].buf_addr = NULL;

		/* extra ios needn't to allocate io buffer */
		if (i >= q->q_depth)
			goto skip_alloc_buf;

        if (posix_memalign((void **)&q->ios[i].buf_addr,
                    getpagesize(), io_buf_size)) {
            printf("ublk dev %d queue %d io %d posix_memalign failed",
                    q->dev->ctrl_dev->dev_info.dev_id, q->q_id, i);
            goto fail;
        }

		if (!q->ios[i].buf_addr) {
			printf("ublk dev %d queue %d io %d alloc io_buf failed",
					q->dev->ctrl_dev->dev_info.dev_id, q->q_id, i);
			goto fail;
		}
skip_alloc_buf:
		q->ios[i].flags = UBLKSRV_NEED_FETCH_RQ | UBLKSRV_IO_FREE;
		q->ios[i].data.private_data = malloc(io_data_size);
		q->ios[i].data.tag = i;
		if (i < q->q_depth)
			q->ios[i].data.iod = ublksrv_get_iod(q, i);
		else
			q->ios[i].data.iod = NULL;

	}

	ublksrv_setup_ring_params(&p, cq_depth,
			IORING_SETUP_SQE128 | IORING_SETUP_COOP_TASKRUN);
	ret = io_uring_queue_init_params(ring_depth, &q->ring, &p);
	if (ret < 0) {
		printf("ublk dev %d queue %d setup io_uring failed %d",
				q->dev->ctrl_dev->dev_info.dev_id, q->q_id, ret);
		goto fail;
	}

	q->ring_ptr = &q->ring;

	ret = io_uring_register_files(&q->ring, dev->tgt.fds,
			dev->tgt.nr_fds + 1);
	if (ret) {
		printf("ublk dev %d queue %d register files failed %d",
				q->dev->ctrl_dev->dev_info.dev_id, q->q_id, ret);
		goto fail;
	}

	io_uring_register_ring_fd(&q->ring);

#if defined(PR_SET_IO_FLUSHER)
	if (prctl(PR_SET_IO_FLUSHER, 0, 0, 0, 0) != 0)
		ublk_err("ublk dev %d queue %d set_io_flusher failed",
			q->dev->ctrl_dev->dev_info.dev_id, q->q_id);
#endif

	ublksrv_queue_adjust_uring_io_wq_workers(q);

	q->private_data = queue_data;


	if (ctrl_dev->queues_cpuset)
		ublksrv_set_sched_affinity(dev, q_id);

	setpriority(PRIO_PROCESS, getpid(), -20);

	/* submit all io commands to ublk driver */
	ublksrv_submit_fetch_commands(q);

atomic_init(&q->tgt_io_inflight,0);
pthread_mutex_init(&q->cond_mutex, NULL);
pthread_cond_init(&q->cond, NULL);
pthread_mutex_init(&q->lock,NULL);

	return (struct ublksrv_queue *)q;
 fail:
	printf("ublk dev %d queue %d failed",
			ctrl_dev->dev_info.dev_id, q_id);
	return NULL;
}

static int ublksrv_queue_is_done(struct _ublksrv_queue *q)
{
	return (q->state & UBLKSRV_QUEUE_STOPPING) &&
		!io_uring_sq_ready(&q->ring);
}

static void ublksrv_kill_eventfd(struct _ublksrv_queue *q)
{
	if ((q->state & UBLKSRV_QUEUE_STOPPING) && q->efd >= 0) {
		uint64_t data = 1;
		int ret;

		ret = write(q->efd, &data, sizeof(uint64_t));
		if (ret != sizeof(uint64_t))
			printf("%s:%d write fail %d/%zu\n",
					__func__, __LINE__, ret, sizeof(uint64_t));
	}
}

static void ublksrv_queue_discard_io_pages(struct _ublksrv_queue *q)
{
	const struct ublksrv_ctrl_dev *cdev = q->dev->ctrl_dev;
	unsigned int io_buf_size = cdev->dev_info.max_io_buf_bytes;
	int i = 0;

	for (i = 0; i < q->q_depth; i++)
		madvise(q->ios[i].buf_addr, io_buf_size, MADV_DONTNEED);
}


static void ublksrv_queue_idle_enter(struct _ublksrv_queue *q)
{
	if (q->state & UBLKSRV_QUEUE_IDLE)
		return;

	ublksrv_queue_discard_io_pages(q);
	q->state |= UBLKSRV_QUEUE_IDLE;
}

static inline void ublksrv_queue_idle_exit(struct _ublksrv_queue *q)
{
	if (q->state & UBLKSRV_QUEUE_IDLE) {
		q->state &= ~UBLKSRV_QUEUE_IDLE;
	}
}

static inline unsigned int user_data_to_tag(__u64 user_data)
{
	return user_data & 0xffff;
}

static inline unsigned int user_data_to_op(__u64 user_data)
{
	return (user_data >> 16) & 0xff;
}

static inline int is_target_io(__u64 user_data)
{
	return (user_data & (1ULL << 63)) != 0;
}

static inline void ublksrv_handle_tgt_cqe(struct _ublksrv_queue *q,
		struct io_uring_cqe *cqe)
{
	unsigned tag = user_data_to_tag(cqe->user_data);

	if (cqe->res < 0 && cqe->res != -EAGAIN) {
		printf("%s: failed tgt io: res %d qid %u tag %u, cmd_op %u\n",
			__func__, cqe->res, q->q_id,
			user_data_to_tag(cqe->user_data),
			user_data_to_op(cqe->user_data));
	}

}

static inline void ublksrv_mark_io_done(struct ublk_io *io, int res)
{
	/*
	 * mark io done by target, so that ->ubq_daemon can commit its
	 * result and fetch new request via io_uring command.
	 */
	io->flags |= (UBLKSRV_NEED_COMMIT_RQ_COMP | UBLKSRV_IO_FREE);

	io->result = res;
}


int ublksrv_complete_io(const struct ublksrv_queue *tq, unsigned tag, int res)
{
	struct _ublksrv_queue *q = tq_to_local(tq);


	struct ublk_io *io = &q->ios[tag];

	ublksrv_mark_io_done(io, res);
	return ublksrv_queue_io_cmd(q, io, tag);
}

static inline struct longhorn_io_data *io_tgt_to_longhorn_data(const struct ublk_io_tgt *io)
{
    return (struct longhorn_io_data *)(io + 1);
}

static inline __u8 ublksrv_get_op(const struct ublksrv_io_desc *iod)
{
	return iod->op_flags & 0xff;
}

static int req_to_longhorn_cmd_type(const struct ublksrv_io_desc *iod)
{
    switch (ublksrv_get_op(iod)) {
    case UBLK_IO_OP_READ:
        return LONGHORN_CMD_TYPE_READ;
    case UBLK_IO_OP_WRITE:
        return LONGHORN_CMD_TYPE_WRITE;
    case UBLK_IO_OP_DISCARD:
        return LONGHORN_CMD_TYPE_UNMAP;
    //case UBLK_IO_OP_FLUSH:
    //    return LONGHORN_CMD_TYPE_FLUSH;
    //case UBLK_IO_OP_WRITE_SAME:
    //    return LONGHORN_CMD_TYPE_WRITE_SAME;
    //case UBLK_IO_OP_WRITE_ZEROES:
    //    return LONGHORN_CMD_TYPE_WRITE_ZEROS;
    default:
        return -1;
    }
}

uint32_t get_nr_sectors(const struct ublksrv_io_desc *iod){
    return iod->nr_sectors;
}

static inline struct ublk_io_tgt *__ublk_get_io_tgt_data(const struct ublk_io_data *io)
{
	return (struct ublk_io_tgt *)io->private_data;
}

static inline void __longhorn_build_req(const struct ublksrv_queue *q,
                                        const struct ublk_io_data *data,
                                        const struct longhorn_io_data *longhorn_data,
                                        uint32_t type,
                                        struct message *req)
{
    req->magic = htole16(LONGHORN_MESSAGE_MAGIC);
    req->seq = htole32(longhorn_data->seq);
    req->type = htole32(type);
    req->offset = cpu_to_le64((uint64_t)data->iod->start_sector << 9);
    req->size = htole32(data->iod->nr_sectors << 9);

    if (type == LONGHORN_CMD_TYPE_WRITE) {
        req->data_length = htole32(data->iod->nr_sectors << 9);
    } else {
        req->data_length = htole32(0);
    }
}


void onRequestAsyncWrapper(struct msghdr *msg ,struct message *req,int opType,const struct ublksrv_queue *q, const struct ublk_io_data *data) {
    onRequestAsync(msg ,req,opType,(struct ublksrv_queue *)q, (struct ublk_io_data *)data);
}

static int demo_handle_io_async(const struct ublksrv_queue *q,
		const struct ublk_io_data *data)
{
	const struct ublksrv_io_desc *iod = data->iod;
    struct ublk_io_tgt *io = __ublk_get_io_tgt_data(data);

    int ret = -EIO;
    struct message req;
    struct longhorn_io_data *longhorn_data = io_tgt_to_longhorn_data(io);
    int type = req_to_longhorn_cmd_type(data->iod);
    struct iovec iov[2] = {
        [0] = {
            .iov_base = (void *)&req,
            .iov_len = sizeof(req),
        },
        [1] = {
            .iov_base = (void *)data->iod->addr,
            .iov_len = data->iod->nr_sectors << 9,
        },
    };
    struct msghdr msg = {
        .msg_iov = iov,
        .msg_iovlen = 2,
    };

    if (type == -1) {
        printf("Unsupported longhorn command type %d\n", type);
    }


    longhorn_data->seq = data->tag;
    __longhorn_build_req(q, data, longhorn_data, type, &req);


   onRequestAsyncWrapper(&msg,&req,type,q,data);
   struct _ublksrv_queue *q_t = tq_to_local(q);

   atomic_fetch_add(&q_t->tgt_io_inflight,1);

	return 0;
}


static void ublksrv_handle_cqe(struct io_uring *r,
		struct io_uring_cqe *cqe, void *data)
{
	struct _ublksrv_queue *q = container_of(r, struct _ublksrv_queue, ring);
	unsigned tag = user_data_to_tag(cqe->user_data);
	unsigned cmd_op = user_data_to_op(cqe->user_data);
	int fetch = (cqe->res != UBLK_IO_RES_ABORT) &&
		!(q->state & UBLKSRV_QUEUE_STOPPING);
	struct ublk_io *io;


	/* Don't retrieve io in case of target io */
	if (is_target_io(cqe->user_data)) {
		ublksrv_handle_tgt_cqe(q, cqe);
		return;
	}

	io = &q->ios[tag];

	if (!fetch) {
		q->state |= UBLKSRV_QUEUE_STOPPING;
		io->flags &= ~UBLKSRV_NEED_FETCH_RQ;
	}

	if (cqe->res == UBLK_IO_RES_OK) {

        demo_handle_io_async(local_to_tq(q), &io->data);

	} else if (cqe->res == UBLK_IO_RES_NEED_GET_DATA) {
		io->flags |= UBLKSRV_NEED_GET_DATA | UBLKSRV_IO_FREE;
		ublksrv_queue_io_cmd(q, io, tag);
	} else {
		/*
		 * COMMIT_REQ will be completed immediately since no fetching
		 * piggyback is required.
		 *
		 * Marking IO_FREE only, then this io won't be issued since
		 * we only issue io with (UBLKSRV_IO_FREE | UBLKSRV_NEED_*)
		 *
		 * */
		io->flags = UBLKSRV_IO_FREE;
	}
}

static int ublksrv_reap_events_uring(struct io_uring *r)
{
	struct io_uring_cqe *cqe;
	unsigned head;
	int count = 0;

	io_uring_for_each_cqe(r, head, cqe) {
		ublksrv_handle_cqe(r, cqe, NULL);
		count += 1;
	}
	io_uring_cq_advance(r, count);

	return count;
}


int ublksrv_process_io(const struct ublksrv_queue *tq)
{
	struct _ublksrv_queue *q = tq_to_local(tq);
	int ret, reapped;
	struct __kernel_timespec ts = {
		.tv_sec = UBLKSRV_IO_IDLE_SECS,
		.tv_nsec = 0
        };
	struct __kernel_timespec *tsp = (q->state & UBLKSRV_QUEUE_IDLE) ?
		NULL : &ts;
	struct io_uring_cqe *cqe;

	if (ublksrv_queue_is_done(q))
		return -ENODEV;



    pthread_mutex_lock(&q->cond_mutex);
    while(atomic_load(&q->tgt_io_inflight)>0) {
        pthread_cond_wait(&q->cond,&q->cond_mutex);
    }
    pthread_mutex_unlock(&q->cond_mutex);

   ret = io_uring_submit_and_wait_timeout(&q->ring, &cqe, 1, tsp, NULL);

	reapped = ublksrv_reap_events_uring(&q->ring);

	if ((q->state & UBLKSRV_QUEUE_STOPPING))
		ublksrv_kill_eventfd(q);
	else {
		if (ret == -ETIME && reapped == 0 &&
				!io_uring_sq_ready(&q->ring))
			ublksrv_queue_idle_enter(q);
		else
			ublksrv_queue_idle_exit(q);
	}
	return reapped;
}

static void *demo_null_io_handler_fn(void *data)
{
	struct ublksrv_queue_info *info = (struct ublksrv_queue_info *)data;
	const struct ublksrv_dev *dev = info->dev;
	const struct ublksrv_ctrl_dev_info *dinfo =
		ublksrv_ctrl_get_dev_info(ublksrv_get_ctrl_dev(dev));
	unsigned dev_id = dinfo->dev_id;
	unsigned short q_id = info->qid;
	const struct ublksrv_queue *q;

	sched_setscheduler(getpid(), SCHED_RR, NULL);

	q = ublksrv_queue_init(dev, q_id, NULL);

	if (!q) {
		fprintf(stderr, "ublk dev %d queue %d init queue failed\n",
				dinfo->dev_id, q_id);
		return NULL;
	}

	do {
		if (ublksrv_process_io(q) < 0){
			break;
			}
	} while (1);

	notifyShutdown();

	return NULL;
}

static int demo_null_io_handler(struct ublksrv_ctrl_dev *ctrl_dev)
{
	int ret, i;
	const struct ublksrv_dev *dev;
	struct ublksrv_queue_info *info_array;
	void *thread_ret;
	const struct ublksrv_ctrl_dev_info *dinfo =
		ublksrv_ctrl_get_dev_info(ctrl_dev);

	info_array = (struct ublksrv_queue_info *)
		calloc(sizeof(struct ublksrv_queue_info), dinfo->nr_hw_queues);
	if (!info_array)
		return -ENOMEM;

	dev = ublksrv_dev_init(ctrl_dev);
	if (!dev) {
		free(info_array);
		return -ENOMEM;
	}

	for (i = 0; i < dinfo->nr_hw_queues; i++) {
		info_array[i].dev = dev;
		info_array[i].qid = i;
		pthread_create(&info_array[i].thread, NULL,
				demo_null_io_handler_fn,
				&info_array[i]);
	}

	/* everything is fine now, start us */
	ret = ublksrv_ctrl_start_dev(ctrl_dev, getpid());
	if (ret < 0)
		goto fail;

	for (i = 0; i < dinfo->nr_hw_queues; i++)
		pthread_join(info_array[i].thread, &thread_ret);
 fail:

	free(info_array);

	return ret;
}

int ublksrv_start_daemon(struct ublksrv_ctrl_dev *ctrl_dev)
{
	int ret;

	if (ublksrv_ctrl_get_affinity(ctrl_dev) < 0)
		return -1;

	ret = demo_null_io_handler(ctrl_dev);

	return ret;
}

int ublksrv_ctrl_del_dev(struct ublksrv_ctrl_dev *dev)
{
	struct ublksrv_ctrl_cmd_data data = {
		.cmd_op = UBLK_CMD_DEL_DEV,
		.flags = 0,
	};

	ublk_un_privileged_prep_data(dev, data);

	return __ublksrv_ctrl_cmd(dev, &data);
}


int ublksrv_ctrl_stop_dev(struct ublksrv_ctrl_dev *dev)
{
	struct ublksrv_ctrl_cmd_data data = {
		.cmd_op	= UBLK_CMD_STOP_DEV,
	};
	int ret;

	ublk_un_privileged_prep_data(dev, data);

	ret = __ublksrv_ctrl_cmd(dev, &data);

	return ret;
}

void ublksrv_ctrl_deinit(struct ublksrv_ctrl_dev *dev)
{
	close(dev->ring.ring_fd);
	close(dev->ctrl_fd);
	free(dev->queues_cpuset);
	free(dev);
}

static int __ublksrv_ctrl_get_info_no_trans(struct ublksrv_ctrl_dev *dev,
		unsigned cmd_op)
{
	char buf[UBLKC_PATH_MAX + sizeof(dev->dev_info)];
	struct ublksrv_ctrl_cmd_data data = {
		.cmd_op	= cmd_op,
		.flags	= CTRL_CMD_HAS_BUF | CTRL_CMD_NO_TRANS,
		.addr = (__u64)&dev->dev_info,
		.len = sizeof(struct ublksrv_ctrl_dev_info),
	};
	bool has_dev_path = false;
	int ret;

	if (ublk_is_unprivileged(dev) && _IOC_NR(data.cmd_op) == UBLK_CMD_GET_DEV_INFO)
		return -EINVAL;

	if (_IOC_NR(data.cmd_op) == UBLK_CMD_GET_DEV_INFO2) {
		snprintf(buf, UBLKC_PATH_MAX, "%s%d", UBLKC_DEV,
			dev->dev_info.dev_id);
		data.flags |= CTRL_CMD_HAS_BUF | CTRL_CMD_HAS_DATA;
		data.len = sizeof(buf);
		data.dev_path_len = UBLKC_PATH_MAX;
		data.addr = (__u64)buf;
		has_dev_path = true;
	}

	ret = __ublksrv_ctrl_cmd(dev, &data);
	if (ret >= 0 && has_dev_path)
		memcpy(&dev->dev_info, &buf[UBLKC_PATH_MAX],
				sizeof(dev->dev_info));
	return ret;
}

static int __ublksrv_ctrl_get_info(struct ublksrv_ctrl_dev *dev,
		unsigned cmd_op)
{
	unsigned new_code = legacy_op_to_ioctl(cmd_op);
	int ret = __ublksrv_ctrl_get_info_no_trans(dev, new_code);

	/*
	 * Try ioctl cmd encoding first, then fallback to legacy command
	 * opcode if ioctl encoding fails
	 */
	if (ret < 0)
		ret = __ublksrv_ctrl_get_info_no_trans(dev, cmd_op);

	return ret;
}

int ublksrv_ctrl_get_info(struct ublksrv_ctrl_dev *dev)
{
	int ret;

	unsigned cmd_op	=
#ifdef UBLK_CMD_GET_DEV_INFO2
		UBLK_CMD_GET_DEV_INFO2;
#else
		UBLK_CMD_GET_DEV_INFO;
#endif
	ret = __ublksrv_ctrl_get_info(dev, cmd_op);

	if (cmd_op == UBLK_CMD_GET_DEV_INFO)
		return ret;

	if (ret < 0) {
		/* unprivileged does support GET_DEV_INFO2 */
		if (ublk_is_unprivileged(dev))
			return ret;
		/*
		 * fallback to GET_DEV_INFO since driver may not support
		 * GET_DEV_INFO2
		 */
		ret = __ublksrv_ctrl_get_info(dev, UBLK_CMD_GET_DEV_INFO);
	}

	return ret;
}

int cmd_dev_del(int number)
{
	struct ublksrv_ctrl_dev *dev;
	int ret;
	struct ublksrv_dev_data data = {
		.dev_id = number,
		.run_dir = UBLKSRV_PID_DIR,
	};

	dev = ublksrv_ctrl_init(&data);
	if (!dev) {
		fprintf(stderr, "ublksrv_ctrl_init failed id %d\n", number);
		fflush(stderr);
		return -EOPNOTSUPP;
	}

	ret = ublksrv_ctrl_get_info(dev);
    	if (ret < 0) {
    		ret = 0;
    		fprintf(stderr, "can't get dev info from %d: %d\n", number, ret);
    		goto fail;
    	}

	ret = ublksrv_ctrl_stop_dev(dev);
	if (ret < 0) {
		fprintf(stderr, "stop dev %d failed\n", number);
	    goto fail;
	}

	ret = ublksrv_ctrl_del_dev(dev);
    	if (ret < 0) {
    		fprintf(stderr, "delete dev %d failed %d\n", number, ret);
    		goto fail;
    	}

fail :
    fflush(stderr);
	ublksrv_ctrl_deinit(dev);
	return ret;
}
