/*
 * Admin commands, issued by ublk server, and handled by ublk driver.
 *
 * Legacy command definition, don't use in new application, and don't
 * add new such definition any more
 */
 #include <stdatomic.h>

#define	UBLK_CMD_GET_QUEUE_AFFINITY	0x01
#define	UBLK_CMD_GET_DEV_INFO	0x02
#define	UBLK_CMD_ADD_DEV		0x04
#define	UBLK_CMD_DEL_DEV		0x05
#define	UBLK_CMD_START_DEV	0x06
#define	UBLK_CMD_STOP_DEV	0x07
#define	UBLK_CMD_SET_PARAMS	0x08
#define	UBLK_CMD_GET_PARAMS	0x09
#define	UBLK_CMD_START_USER_RECOVERY	0x10
#define	UBLK_CMD_END_USER_RECOVERY	0x11
#define	UBLK_CMD_GET_DEV_INFO2		0x12

/* Any new ctrl command should encode by __IO*() */
#define UBLK_U_CMD_GET_QUEUE_AFFINITY	\
	_IOR('u', UBLK_CMD_GET_QUEUE_AFFINITY, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_GET_DEV_INFO		\
	_IOR('u', UBLK_CMD_GET_DEV_INFO, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_ADD_DEV		\
	_IOWR('u', UBLK_CMD_ADD_DEV, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_DEL_DEV		\
	_IOWR('u', UBLK_CMD_DEL_DEV, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_START_DEV		\
	_IOWR('u', UBLK_CMD_START_DEV, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_STOP_DEV		\
	_IOWR('u', UBLK_CMD_STOP_DEV, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_SET_PARAMS		\
	_IOWR('u', UBLK_CMD_SET_PARAMS, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_GET_PARAMS		\
	_IOR('u', UBLK_CMD_GET_PARAMS, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_START_USER_RECOVERY	\
	_IOWR('u', UBLK_CMD_START_USER_RECOVERY, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_END_USER_RECOVERY	\
	_IOWR('u', UBLK_CMD_END_USER_RECOVERY, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_GET_DEV_INFO2	\
	_IOR('u', UBLK_CMD_GET_DEV_INFO2, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_GET_FEATURES	\
	_IOR('u', 0x13, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_DEL_DEV_ASYNC	\
	_IOR('u', 0x14, struct ublksrv_ctrl_cmd)

/*
 * 64bits are enough now, and it should be easy to extend in case of
 * running out of feature flags
 */
#define UBLK_FEATURES_LEN  8



#define UBLK_F_UNPRIVILEGED_DEV	(1UL << 5)

/* use ioctl encoding for uring command */
#define UBLK_F_CMD_IOCTL_ENCODE	(1UL << 6)

/* Copy between request and user buffer by pread()/pwrite() */
#define UBLK_F_USER_COPY	(1UL << 7)

static const unsigned int ctrl_cmd_op[] = {
	[UBLK_CMD_GET_QUEUE_AFFINITY]	= UBLK_U_CMD_GET_QUEUE_AFFINITY,
	[UBLK_CMD_GET_DEV_INFO]		= UBLK_U_CMD_GET_DEV_INFO,
	[UBLK_CMD_ADD_DEV]		= UBLK_U_CMD_ADD_DEV,
	[UBLK_CMD_DEL_DEV]		= UBLK_U_CMD_DEL_DEV,
	[UBLK_CMD_START_DEV]		= UBLK_U_CMD_START_DEV,
	[UBLK_CMD_STOP_DEV]		= UBLK_U_CMD_STOP_DEV,
	[UBLK_CMD_SET_PARAMS]		= UBLK_U_CMD_SET_PARAMS,
	[UBLK_CMD_GET_PARAMS]		= UBLK_U_CMD_GET_PARAMS,
	[UBLK_CMD_START_USER_RECOVERY]	= UBLK_U_CMD_START_USER_RECOVERY,
	[UBLK_CMD_END_USER_RECOVERY]	= UBLK_U_CMD_END_USER_RECOVERY,
	[UBLK_CMD_GET_DEV_INFO2]	= UBLK_U_CMD_GET_DEV_INFO2,
};


#define UBLKC_PATH_MAX	32
#define	UBLKC_DEV	"/dev/ublkc"

#define JSON_OFFSET   32

struct ublksrv_queue_info {
	const struct ublksrv_dev *dev;
	int qid;
	pthread_t thread;
};



#define local_to_tq(q)	((struct ublksrv_queue *)(q))
#define tq_to_local(q)	((struct _ublksrv_queue *)(q))

#define local_to_tdev(d)	((struct ublksrv_dev *)(d))
#define tdev_to_local(d)	((struct _ublksrv_dev *)(d))

#define	MAX_NR_HW_QUEUES 32
struct _ublksrv_dev {
	//keep same with ublksrv_dev
	struct ublksrv_tgt_info tgt;
	/********** part of API, can't change ************/
	//struct ublksrv_tgt_info tgt;
	/************************************************/

	struct _ublksrv_queue *__queues[MAX_NR_HW_QUEUES];
	char	*io_buf_start;
	pthread_t *thread;
	int cdev_fd;
	int pid_file_fd;

	const struct ublksrv_ctrl_dev *ctrl_dev;
	void	*target_data;
	int	cq_depth;
	int	pad;

	/* reserved isn't necessary any more */
	unsigned long reserved[3];
};

#define		UBLK_IO_OP_READ		0
#define		UBLK_IO_OP_WRITE		1
#define		UBLK_IO_OP_FLUSH		2
#define		UBLK_IO_OP_DISCARD		3
#define		UBLK_IO_OP_WRITE_SAME		4
#define		UBLK_IO_OP_WRITE_ZEROES		5
#define		UBLK_IO_OP_ZONE_OPEN		10
#define		UBLK_IO_OP_ZONE_CLOSE		11
#define		UBLK_IO_OP_ZONE_FINISH		12
#define		UBLK_IO_OP_ZONE_APPEND		13
#define		UBLK_IO_OP_ZONE_RESET_ALL	14
#define		UBLK_IO_OP_ZONE_RESET		15

uint32_t get_nr_sectors(const struct ublksrv_io_desc *iod);
int ublksrv_complete_io(const struct ublksrv_queue *tq, unsigned tag, int res);
void onRequest(struct msghdr *msg ,struct message *req,int opType );
void onRequestAsync(struct msghdr *msg ,struct message *req,int opType,
                    struct ublksrv_queue *q, struct ublk_io_data *data);
void handleReplies(int counter);

int cmd_dev_del(int number);