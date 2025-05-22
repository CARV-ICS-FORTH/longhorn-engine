// mystruct.c
#define _GNU_SOURCE
#include "ublkhelper.h"
#include "includes.h"
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

pthread_mutex_t cond_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;


#define	CTRL_DEV	"/dev/ublk-control"
#define CTRL_CMD_HAS_DATA	1
#define CTRL_CMD_HAS_BUF	2
#define CTRL_CMD_NO_TRANS	4

static void sig_handler(int sig)
{
if (sig == SIGTERM)
		printf("got TERM signal");
}


static void setup_pthread_sigmask()
{
	sigset_t   signal_mask;

	if (signal(SIGTERM, sig_handler) == SIG_ERR)
		return;

	/* make sure SIGTERM won't be blocked */
	sigemptyset(&signal_mask);
	sigaddset(&signal_mask, SIGINT);
	sigaddset(&signal_mask, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &signal_mask, NULL);
}

const struct ublksrv_ctrl_dev *ublksrv_get_ctrl_dev(
		const struct ublksrv_dev *dev)
{
	return tdev_to_local(dev)->ctrl_dev;
}


void ublksrv_apply_oom_protection()
{
	char oom_score_adj_path[64];
	pid_t pid = getpid();
	int fd;

	snprintf(oom_score_adj_path, 64, "/proc/%d/oom_score_adj", pid);

	fd = open(oom_score_adj_path, O_RDWR);
	if (fd > 0) {
		char val[32];
		int len, ret;

		len = snprintf(val, 32, "%d", -1000);
		ret = write(fd, val, len);
		if (ret != len)
			printf("%s:%d write fail %d/%d\n",
					__func__, __LINE__, ret, len);
		close(fd);
	}
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

//	ublk_ctrl_dbg(UBLK_DBG_CTRL_CMD, "dev %d cmd_op %x/%x, user_data %p\n",
//			dev->dev_info.dev_id, data->cmd_op, cmd_op, cmd);
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
		return ret;
	}

	ublksrv_ctrl_init_cmd(dev, sqe, data);

	ret = io_uring_submit(&dev->ring);
	if (ret < 0) {
		fprintf(stderr, "uring submit ret %d\n", ret);
		return ret;
	}

	do {
		ret = io_uring_wait_cqe(&dev->ring, &cqe);
	} while (ret == -EINTR);
	if (ret < 0) {
		fprintf(stderr, "wait cqe: %s\n", strerror(-ret));
		return ret;
	}
	io_uring_cqe_seen(&dev->ring, cqe);

//	printf("dev %d, ctrl cqe res %d, user_data %llx\n",
//			dev->dev_info.dev_id, cqe->res, cqe->user_data);
//			fflush(stdout);
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

/* Wait until ublk device is setup by udev */
static void ublksrv_check_dev(const struct ublksrv_ctrl_dev_info *info)
{
	unsigned int max_time = 1000000, wait = 0;
	char buf[64];

	snprintf(buf, 64, "%s%d", "/dev/ublkc", info->dev_id);

	while (wait < max_time) {
		int fd = open(buf, O_RDWR);

		if (fd > 0) {
			close(fd);
			break;
		}

		usleep(100000);
		wait += 100000;
	}
}
//
//static int start_daemon(void (*child_fn)(void *), void *data)
//{
//	char path[PATH_MAX];
//	int fd;
//	char *res;
//
//	if (setsid() == -1)
//		return -1;
//
//	res = getcwd(path, PATH_MAX);
//	if (!res)
//		printf("%s: %d getcwd failed %m\n", __func__, __LINE__);
//
//	switch (fork()) {
//	case -1: return -1;
//	case 0:  break;
//	default: _exit(EXIT_SUCCESS);
//	}
//
//	if (chdir(path) != 0)
//		printf("%s: %d chdir failed %m\n", __func__, __LINE__);
//
//	close(STDIN_FILENO);
//	fd = open("/dev/null", O_RDWR);
//	if (fd != STDIN_FILENO)
//		return -1;
//	if (dup2(fd, STDOUT_FILENO) != STDOUT_FILENO)
//		return -1;
//	if (dup2(fd, STDERR_FILENO) != STDERR_FILENO)
//		return -1;
//
//	child_fn(data);
//	return 0;
//}

static void ublksrv_drain_fetch_commands(const struct ublksrv_dev *dev,
		struct ublksrv_queue_info *info)
{
	const struct ublksrv_ctrl_dev_info *dinfo =
		ublksrv_ctrl_get_dev_info(ublksrv_get_ctrl_dev(dev));
	unsigned nr_queues = dinfo->nr_hw_queues;
	int i;
	void *ret;

	for (i = 0; i < nr_queues; i++)
		pthread_join(info[i].thread, &ret);
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
//	ret = ublksrv_tgt_init(dev, ctrl_dev->tgt_type, ctrl_dev->tgt_ops,
//			ctrl_dev->tgt_argc, ctrl_dev->tgt_argv);
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


static pthread_mutex_t jbuf_lock;
static char jbuf[4096];

static void ublksrv_io_handler(void *data)
{
	const struct ublksrv_ctrl_dev *ctrl_dev = (struct ublksrv_ctrl_dev *)data;
	const struct ublksrv_ctrl_dev_info *dinfo =
		ublksrv_ctrl_get_dev_info(ctrl_dev);
	int dev_id = dinfo->dev_id;
	int i;
	char buf[32];
	const struct ublksrv_dev *dev;
	struct ublksrv_queue_info *info_array;

	snprintf(buf, 32, "%s-%d", "ublksrvd", dev_id);
	openlog(buf, LOG_PID, LOG_USER);

	printf("start ublksrv io daemon %s\n", buf);

	pthread_mutex_init(&jbuf_lock, NULL);

	dev = ublksrv_dev_init(ctrl_dev);
	if (!dev) {
		printf( "dev-%d start ubsrv failed", dev_id);
		goto out;
	}

	setup_pthread_sigmask();

	if (!(dinfo->flags & UBLK_F_UNPRIVILEGED_DEV))
		ublksrv_apply_oom_protection();

	info_array = (struct ublksrv_queue_info *)calloc(sizeof(
				struct ublksrv_queue_info),
			dinfo->nr_hw_queues);

//	for (i = 0; i < dinfo->nr_hw_queues; i++) {
//		info_array[i].dev = dev;
//		info_array[i].qid = i;
//		pthread_create(&info_array[i].thread, NULL,
//				ublksrv_io_handler_fn,
//				&info_array[i]);
//	}

	/* wait until we are terminated */
	ublksrv_drain_fetch_commands(dev, info_array);
//	free(info_array);
//	free(jbuf);

	//ublksrv_dev_deinit(dev); //TODO delete dev
out:
	printf("end ublksrv io daemon");
	closelog();
}

//static int ublksrv_start_io_daemon(const struct ublksrv_ctrl_dev *dev)
//{
//	start_daemon(ublksrv_io_handler, (void *)dev);
//	return 0;
//}

const char *ublksrv_ctrl_get_run_dir(const struct ublksrv_ctrl_dev *dev)
{
	return dev->run_dir;
}



static int ublksrv_check_dev_data(const char *buf, int size)
{
	struct ublk_params p;

	if (size < JSON_OFFSET)
		return -EINVAL;

	return 0;// ublksrv_json_read_params(&p, &buf[JSON_OFFSET]);
}


static int ublksrv_get_io_daemon_pid(const struct ublksrv_ctrl_dev *ctrl_dev,
		bool check_data)
{
	const char *run_dir = ublksrv_ctrl_get_run_dir(ctrl_dev);
	const struct ublksrv_ctrl_dev_info *info =
		ublksrv_ctrl_get_dev_info(ctrl_dev);
	int ret = -1, pid_fd;
	char path[256];
	char *buf = NULL;
	int size = JSON_OFFSET;
	int daemon_pid;
	struct stat st;

	if (!run_dir)
		return -EINVAL;

	snprintf(path, 256, "%s/%d.pid", run_dir, info->dev_id);

	pid_fd = open(path, O_RDONLY);
	if (pid_fd < 0)
		goto out;

	if (fstat(pid_fd, &st) < 0)
		goto out;

	if (check_data)
		size = st.st_size;
	else
		size = JSON_OFFSET;

	buf = (char *)malloc(size);
	if (read(pid_fd, buf, size) <= 0)
		goto out;

	daemon_pid = strtol(buf, NULL, 10);
	if (daemon_pid < 0)
		goto out;

	ret = kill(daemon_pid, 0);
	if (ret)
		goto out;

	if (check_data) {
		ret = ublksrv_check_dev_data(buf, size);
		if (ret)
			goto out;
	}
	ret = daemon_pid;
out:
	if (pid_fd > 0)
		close(pid_fd);
	free(buf);
	return ret;
}

//static int ublksrv_start_daemon(struct ublksrv_ctrl_dev *ctrl_dev)
//{
//	const struct ublksrv_ctrl_dev_info *dinfo =
//		ublksrv_ctrl_get_dev_info(ctrl_dev);
//	int cnt = 0, daemon_pid, ret;
//
//	ublksrv_check_dev(dinfo);
//
//	ret = ublksrv_ctrl_get_affinity(ctrl_dev);
//	if (ret < 0) {
//		fprintf(stderr, "dev %d get affinity failed %d\n",
//				dinfo->dev_id, ret);
//		return -1;
//	}
//
//	switch (fork()) {
//	case -1:
//		return -1;
//	case 0:
//		ublksrv_start_io_daemon(ctrl_dev);
//		break;
//	}
//
//	/* wait until daemon is started, or timeout after 3 seconds */
//	do {
//		daemon_pid = ublksrv_get_io_daemon_pid(ctrl_dev, true);
//		if (daemon_pid < 0) {
//			usleep(100000);
//			cnt++;
//		}
//	} while (daemon_pid < 0 && cnt < 30);
//
//	return daemon_pid;
//
//}



int helperFunc(struct ublksrv_ctrl_dev *dev,struct ublksrv_dev_data *data){
    const char *dump_buf;
    int ret;
    const struct ublksrv_ctrl_dev_info *info = ublksrv_ctrl_get_dev_info(dev);
    data->dev_id = info->dev_id;


    ret = ublksrv_start_daemon(dev);
    	if (ret <= 0) {
    		fprintf(stderr, "start dev %d daemon failed, ret %d\n",
    				data->dev_id, ret);
    				//TODO Here del dev ublksrv_ctrl_del_dev
    	}

//    dump_buf = ublksrv_tgt_get_dev_data(dev);
//    ublksrv_tgt_set_params(dev, dump_buf);
return data->dev_id;

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

    //fflush(stdout);
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

	q->cmd_inflight += 1;

//	printf("%s: (qid %d tag %u cmd_op %u) iof %x stopping %d\n",
//			__func__, q->q_id, tag, cmd_op,
//			io->flags, !!(q->state & UBLKSRV_QUEUE_STOPPING));

    atomic_fetch_sub(&q->completed,1);
    if(atomic_fetch_sub(&q->tgt_io_inflight,1) == 1) {
        pthread_mutex_lock(&cond_mutex);
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&cond_mutex);
    }
        pthread_mutex_unlock(&q->lock);

	return 1;
}

static void ublksrv_submit_fetch_commands(struct _ublksrv_queue *q)
{
	int i = 0;

	for (i = 0; i < q->q_depth; i++)
		ublksrv_queue_io_cmd(q, &q->ios[i], i);


	//__ublksrv_queue_event(q); //TODO events disabled
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
	q->cmd_inflight = 0;
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

//		if (dev->tgt.ops->alloc_io_buf)
//			q->ios[i].buf_addr =
//				dev->tgt.ops->alloc_io_buf(local_to_tq(q),
//					i, io_buf_size);
//		else
			if (posix_memalign((void **)&q->ios[i].buf_addr,
						getpagesize(), io_buf_size)) {
				printf("ublk dev %d queue %d io %d posix_memalign failed",
						q->dev->ctrl_dev->dev_info.dev_id, q->q_id, i);
				goto fail;
			}
		//q->ios[i].buf_addr = malloc(io_buf_size);
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

		//ublk_assert(io_data_size ^ (unsigned long)q->ios[i].data.private_data);
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

	/*
	* N.B. PR_SET_IO_FLUSHER was added with Linux 5.6+.
	*/
#if defined(PR_SET_IO_FLUSHER)
	if (prctl(PR_SET_IO_FLUSHER, 0, 0, 0, 0) != 0)
		ublk_err("ublk dev %d queue %d set_io_flusher failed",
			q->dev->ctrl_dev->dev_info.dev_id, q->q_id);
#endif

	ublksrv_queue_adjust_uring_io_wq_workers(q);

	q->private_data = queue_data;

//	if (ctrl_dev->tgt_ops->init_queue) {
//		if (ctrl_dev->tgt_ops->init_queue(local_to_tq(q),
//					&q->private_data))
//			goto fail;
//	}
//TODO maybe need queue

	if (ctrl_dev->queues_cpuset)
		ublksrv_set_sched_affinity(dev, q_id);

	setpriority(PRIO_PROCESS, getpid(), -20);

//	ret = ublksrv_setup_eventfd(q);
//	if (ret < 0) {
//		ublk_err("ublk dev %d queue %d setup eventfd failed: %s",
//			q->dev->ctrl_dev->dev_info.dev_id, q->q_id,
//			strerror(-ret));
//		goto fail;
//	}
//TODO disabled eventfd
	/* submit all io commands to ublk driver */
	ublksrv_submit_fetch_commands(q);
	q->cmd_inflight = 0;
atomic_init(&q->tgt_io_inflight,0);
atomic_init(&q->completed,0);
atomic_init(&q->requested,0);
pthread_mutex_init(&q->lock,NULL);
	return (struct ublksrv_queue *)q;
 fail:
	//ublksrv_queue_deinit(local_to_tq(q)); //TODO deinit diabled
	printf("ublk dev %d queue %d failed",
			ctrl_dev->dev_info.dev_id, q_id);
	return NULL;
}

static void ublksrv_reset_aio_batch(struct _ublksrv_queue *q)
{
	q->nr_ctxs = 0;
}


static void ublksrv_submit_aio_batch(struct _ublksrv_queue *q)
{
	int i;

	for (i = 0; i < q->nr_ctxs; i++) {
		struct ublksrv_aio_ctx *ctx = q->ctxs[i];
		uint64_t data = 1;
		int ret;

		ret = write(ctx->efd, &data, sizeof(uint64_t));
		if (ret != sizeof(uint64_t))
			printf("%s:%d write fail ctx[%d]: %d/%zu\n",
					__func__, __LINE__, i, ret, sizeof(uint64_t));
	}
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

	printf("dev%d-q%d: enter idle %x\n",
			q->dev->ctrl_dev->dev_info.dev_id, q->q_id, q->state);
	ublksrv_queue_discard_io_pages(q);
	q->state |= UBLKSRV_QUEUE_IDLE;

//	if (q->tgt_ops->idle_fn)
//		q->tgt_ops->idle_fn(local_to_tq(q), true);
}

static inline void ublksrv_queue_idle_exit(struct _ublksrv_queue *q)
{
	if (q->state & UBLKSRV_QUEUE_IDLE) {
		printf("dev%d-q%d: exit idle %x\n",
			q->dev->ctrl_dev->dev_info.dev_id, q->q_id, q->state);
		q->state &= ~UBLKSRV_QUEUE_IDLE;
//		if (q->tgt_ops->idle_fn)
//			q->tgt_ops->idle_fn(local_to_tq(q), false);
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

static inline int is_eventfd_io(__u64 user_data)
{
	return (user_data & (1ULL << 62)) != 0;
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

	if (is_eventfd_io(cqe->user_data)) {
//		if (q->tgt_ops->handle_event)
//			q->tgt_ops->handle_event(local_to_tq(q));
        //TODO handle event
        printf("eventfd io not implemented\n");
	} else {
//		if (q->tgt_ops->tgt_io_done)
//			q->tgt_ops->tgt_io_done(local_to_tq(q),
//					&q->ios[tag].data, cqe);
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

// In C
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


   // onRequest(&msg,&req,type);
   onRequestAsyncWrapper(&msg,&req,type,q,data);
   struct _ublksrv_queue *q_t = tq_to_local(q);
   atomic_fetch_add(&q_t->tgt_io_inflight,1);
  //  longhorn_data->done = 1;


//ret = longhorn_queue_req(q, data, &req, &msg);
//	ublksrv_complete_io(q, data->tag, iod->nr_sectors << 9);

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

//	printf("%s: res %d (qid %d tag %u cmd_op %u target %d event %d) stopping %d\n",
//			__func__, cqe->res, q->q_id, tag, cmd_op,
//			is_target_io(cqe->user_data),
//			is_eventfd_io(cqe->user_data),
//			(q->state & UBLKSRV_QUEUE_STOPPING));

	/* Don't retrieve io in case of target io */
	if (is_target_io(cqe->user_data)) {
		ublksrv_handle_tgt_cqe(q, cqe);
		return;
	}

	io = &q->ios[tag];
	q->cmd_inflight--;

	if (!fetch) {
		q->state |= UBLKSRV_QUEUE_STOPPING;
		io->flags &= ~UBLKSRV_NEED_FETCH_RQ;
	}

	/*
	 * So far, only sync tgt's io handling is implemented.
	 *
	 * todo: support async tgt io handling via io_uring, and the ublksrv
	 * daemon can poll on both two rings.
	 */

	if (cqe->res == UBLK_IO_RES_OK) {
		//ublk_assert(tag < q->q_depth);

		//q->tgt_ops->handle_io_async(local_to_tq(q), &io->data);
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
//	printf("dev%d-q%d: to_submit %d inflight %u/%u stopping %d\n",
//				q->dev->ctrl_dev->dev_info.dev_id,
//				q->q_id, io_uring_sq_ready(&q->ring),
//				q->cmd_inflight, q->tgt_io_inflight,
//				(q->state & UBLKSRV_QUEUE_STOPPING));

	if (ublksrv_queue_is_done(q))
		return -ENODEV;



    pthread_mutex_lock(&cond_mutex);
    while(atomic_load(&q->tgt_io_inflight)>0) {
        pthread_cond_wait(&cond,&cond_mutex);
    }
    pthread_mutex_unlock(&cond_mutex);

   ret = io_uring_submit_and_wait_timeout(&q->ring, &cqe, 1, tsp, NULL);
//    fflush(stdout);
//    if(atomic_load(&q->tgt_io_inflight)>0){
//       io_uring_submit(&q->ring);
//    }else {
//      ret = io_uring_submit_and_wait_timeout(&q->ring, &cqe, 1, tsp, NULL);
//    }
//	//ublksrv_reset_aio_batch(q);
	reapped = ublksrv_reap_events_uring(&q->ring);
	atomic_fetch_add(&q->requested,reapped);
	//ublksrv_submit_aio_batch(q);


   // handleReplies(reapped);

//	if (q->tgt_ops->handle_io_background)
//		q->tgt_ops->handle_io_background(local_to_tq(q),
//				io_uring_sq_ready(&q->ring));
//TODO handle io background
//	printf( "submit result %d, reapped %d stop %d idle %d",
//			ret, reapped, (q->state & UBLKSRV_QUEUE_STOPPING),
//			(q->state & UBLKSRV_QUEUE_IDLE));

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

	pthread_mutex_lock(&jbuf_lock);
//	ublksrv_json_write_queue_info(ublksrv_get_ctrl_dev(dev), jbuf, sizeof jbuf,
//			q_id, ublksrv_gettid());
	pthread_mutex_unlock(&jbuf_lock);
	q = ublksrv_queue_init(dev, q_id, NULL);
	//fflush(stdout);
	if (!q) {
		fprintf(stderr, "ublk dev %d queue %d init queue failed\n",
				dinfo->dev_id, q_id);
		return NULL;
	}

	fprintf(stdout, "tid %d: ublk dev %d queue %d started\n",
			ublksrv_gettid(),
			dev_id, q->q_id);
	do {
		if (ublksrv_process_io(q) < 0){
			break;
			}
	} while (1);

	fprintf(stdout, "ublk dev %d queue %d exited\n", dev_id, q->q_id);
	//fflush(stdout);
	//ublksrv_queue_deinit(q); //TODO deinit disabled
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

	//demo_null_set_parameters(ctrl_dev, dev);

	/* everything is fine now, start us */
	ret = ublksrv_ctrl_start_dev(ctrl_dev, getpid());
	if (ret < 0)
		goto fail;

//	ublksrv_ctrl_get_info(ctrl_dev);
//	ublksrv_ctrl_dump(ctrl_dev, jbuf); //TODO dump disabled

	/* wait until we are terminated */
	for (i = 0; i < dinfo->nr_hw_queues; i++)
		pthread_join(info_array[i].thread, &thread_ret);
 fail:
//	ublksrv_dev_deinit(dev); //TODO deinit disabled

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


