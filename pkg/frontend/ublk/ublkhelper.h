// ublkhelper.h
#ifndef UBLKHELPER_H
#define UBLKHELPER_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/types.h>
#include <liburing.h>

#define	DEF_QD		128
#define	DEF_NR_HW_QUEUES 1
#define	DEF_BUF_SIZE	(512 << 10)

#define UBLKSRV_PID_DIR  "/tmp/ublksrvd"


//struct __attribute__ ((__packed__)) message {
//    uint16_t    magic;
//    uint32_t    seq;
//    uint32_t    type;
//    int64_t     offset;
//    uint32_t    size;
//    uint32_t    data_length;
//};

struct  message {
    uint16_t    magic;
    uint32_t    seq;
    uint32_t    type;
    int64_t     offset;
    uint32_t    size;
    uint32_t    data_length;
};

//Input to function but may input fields manually to ublksrv_ctrl_init
struct ublksrv_dev_data {
	int		dev_id;
	unsigned	max_io_buf_bytes;
	unsigned short	nr_hw_queues;
	unsigned short	queue_depth;
	const char	*tgt_type;  //unused
	const struct ublksrv_tgt_type *tgt_ops; //unused
	int		tgt_argc;   //unused
	char		**tgt_argv; //unused
	const char	*run_dir;   //defined
	unsigned long	flags;
	unsigned long	ublksrv_flags;
	unsigned long   reserved[7]; //unused ?
};

struct ublksrv_ctrl_dev_info {
	__u16	nr_hw_queues;
	__u16	queue_depth;
	__u16	state;
	__u16	pad0;

	__u32	max_io_buf_bytes;
	__u32	dev_id;

	__s32	ublksrv_pid;
	__u32	pad1;

	__u64	flags;

	/* For ublksrv internal use, invisible to ublk driver */
	__u64	ublksrv_flags;

	__u32	owner_uid;	/* store by kernel */
	__u32	owner_gid;	/* store by kernel */
	__u64	reserved1;
	__u64   reserved2;
};



struct ublksrv_ctrl_dev {
	struct io_uring ring;

	int ctrl_fd;
	unsigned bs_shift;
	struct ublksrv_ctrl_dev_info  dev_info;

	const char *tgt_type;
	const struct ublksrv_tgt_type *tgt_ops;

	/*
	 * default is UBLKSRV_RUN_DIR but can be specified via command line,
	 * pid file will be saved there
	 */
	const char *run_dir;

	union {
		/* used by ->init_tgt() */
		struct {
			int tgt_argc;
			char **tgt_argv;
		};
		/* used by ->recovery_tgt(), tgt_argc == -1 */
		struct {
			int padding;
			const char *recovery_jbuf;
		};
	};

	cpu_set_t *queues_cpuset;

	unsigned long reserved[4];
};

struct ublksrv_ctrl_cmd_data {
	unsigned int cmd_op;
	unsigned short flags;
	unsigned short _pad;

	__u64 data[1];
	__u16 dev_path_len;
	__u16 pad;
	__u32 reserved;

	__u64 addr;
	__u32 len;
};

struct ublksrv_ctrl_cmd {
	/* sent to which device, must be valid */
	__u32	dev_id;

	/* sent to which queue, must be -1 if the cmd isn't for queue */
	__u16	queue_id;
	/*
	 * cmd specific buffer, can be IN or OUT.
	 */
	__u16	len;
	__u64	addr;

	/* inline data */
	__u64	data[1];

	/*
	 * Used for UBLK_F_UNPRIVILEGED_DEV and UBLK_CMD_GET_DEV_INFO2
	 * only, include null char
	 */
	__u16	dev_path_len;
	__u16	pad;
	__u32	reserved;
};



struct ublk_param_basic {
#define UBLK_ATTR_READ_ONLY            (1 << 0)
#define UBLK_ATTR_ROTATIONAL           (1 << 1)
#define UBLK_ATTR_VOLATILE_CACHE       (1 << 2)
#define UBLK_ATTR_FUA                  (1 << 3)
	__u32	attrs;
	__u8	logical_bs_shift;
	__u8	physical_bs_shift;
	__u8	io_opt_shift;
	__u8	io_min_shift;

	__u32	max_sectors;
	__u32	chunk_sectors;

	__u64   dev_sectors;
	__u64   virt_boundary_mask;
};

struct ublk_param_discard {
	__u32	discard_alignment;

	__u32	discard_granularity;
	__u32	max_discard_sectors;

	__u32	max_write_zeroes_sectors;
	__u16	max_discard_segments;
	__u16	reserved0;
};

/*
 * read-only, can't set via UBLK_CMD_SET_PARAMS, disk_devt is available
 * after device is started
 */
struct ublk_param_devt {
	__u32   char_major;
	__u32   char_minor;
	__u32   disk_major;
	__u32   disk_minor;
};

struct ublk_param_zoned {
	__u32	max_open_zones;
	__u32	max_active_zones;
	__u32	max_zone_append_sectors;
	__u8	reserved[20];
};

struct ublk_params {
	/*
	 * Total length of parameters, userspace has to set 'len' for both
	 * SET_PARAMS and GET_PARAMS command, and driver may update len
	 * if two sides use different version of 'ublk_params', same with
	 * 'types' fields.
	 */
	__u32	len;
#define UBLK_PARAM_TYPE_BASIC           (1 << 0)
#define UBLK_PARAM_TYPE_DISCARD         (1 << 1)
#define UBLK_PARAM_TYPE_DEVT            (1 << 2)
#define UBLK_PARAM_TYPE_ZONED           (1 << 3)
	__u32	types;			/* types of parameter included */

	struct ublk_param_basic		basic;
	struct ublk_param_discard	discard;
	struct ublk_param_devt		devt;
	struct ublk_param_zoned	zoned;
};


struct ublksrv_ctrl_dev *ublksrv_ctrl_init(struct ublksrv_dev_data *data);

int ublksrv_ctrl_add_dev(struct ublksrv_ctrl_dev *dev);


int helperFunc(struct ublksrv_ctrl_dev *dev,struct ublksrv_dev_data *data);

#define  UBLKSRV_TGT_MAX_FDS	32
struct ublksrv_tgt_info {
	/** device size */
	unsigned long long dev_size;

	/**
	 * target ring depth, for handling target IOs
	 */
	unsigned int tgt_ring_depth;

	/** how many FDs regisgered */
	unsigned int nr_fds;

	/** file descriptor table */
	int fds[UBLKSRV_TGT_MAX_FDS];

	/** target private data */
	void *tgt_data;

	/**
	 * Extra IO slots for each queue, target code can reserve some
	 * slots for handling internal IO, such as meta data IO, then
	 * ublk_io instances can be assigned for these extra IOs.
	 *
	 * IO slot is useful for storing coroutine data which is for
	 * handling this (meta) IO.
	 */
	unsigned int extra_ios;

	/** size of io private data */
	unsigned int io_data_size;

	/**
	 * target io handling type, target main job is to implement
	 * callbacks defined in this type
	 */
	const struct ublksrv_tgt_type *ops;

	/**
	 * If target needs to override default max workers for io_uring,
	 * initialize io_wq_max_workers with proper value, otherwise
	 * keep them as zero
	 */
	unsigned int iowq_max_workers[2];

	unsigned long reserved[4];
};

struct ublksrv_dev {
	/** device data */
	struct ublksrv_tgt_info tgt;
};
int ublksrv_start_daemon(struct ublksrv_ctrl_dev *ctrl_dev);

int init_params(struct ublksrv_ctrl_dev *dev,struct ublksrv_dev_data *data);
int ublksrv_ctrl_start_dev(struct ublksrv_ctrl_dev *ctrl_dev,int daemon_pid);


#define round_up(val, rnd) \
	(((val) + ((rnd) - 1)) & ~((rnd) - 1))

#define UBLKSRV_F_NEED_EVENTFD		(1UL << 1)

struct ublk_io_data {
	/** tag of this io data, unique in queue wide */
	int tag;
	unsigned int pad;

	/** io description from ublk driver */
	const struct ublksrv_io_desc *iod;

	/**
	 * IO private data, created in ublksrv_queue_init(),
	 * data size is specified in ublksrv_tgt_info.io_data_size
	 */
	void *private_data;
};


struct ublk_io {
	char *buf_addr;

#define UBLKSRV_NEED_FETCH_RQ		(1UL << 0)
#define UBLKSRV_NEED_COMMIT_RQ_COMP	(1UL << 1)
#define UBLKSRV_IO_FREE			(1UL << 2)
#define UBLKSRV_NEED_GET_DATA		(1UL << 3)
	unsigned int flags;

	/* result is updated after all target ios are done */
	unsigned int result;

	struct ublk_io_data  data;
};
struct ublksrv_queue {
	/** queue id */
	int q_id;

	/** So far, all queues in same device has same depth */
	int q_depth;

	/** io uring for handling io commands() from ublk driver */
	struct io_uring *ring_ptr;

	/** which device this queue belongs to */
	const struct ublksrv_dev *dev;

	/** queue's private data, passed from ublksrv_queue_init() */
	void *private_data;
};

struct _ublksrv_queue {
	/********** part of API, can't change ************/
	int q_id;
	int q_depth;

	struct io_uring *ring_ptr;
	struct _ublksrv_dev *dev;
	void *private_data;
	/*************************************************/

	/*
	 * Read only by ublksrv daemon, setup via mmap on /dev/ublkcN.
	 *
	 * ublksrv_io_desc(iod) is stored in this buffer, so iod
	 * can be retrieved by request's tag directly.
	 *
	 * ublksrv writes the iod into this array, and notify ublksrv daemon
	 * by issued io_uring command beforehand.
	 * */
	char *io_cmd_buf;
	char *io_buf;

	unsigned cmd_inflight, tgt_io_inflight;	//obsolete
	unsigned state;

	/* eventfd */
	int efd;

	/* cache tgt ops */
	const struct ublksrv_tgt_type *tgt_ops;

	/*
	 * ring for submit io command to ublk driver, can only be issued
	 * from ublksrv daemon.
	 *
	 * ring depth == dev_info->queue_depth.
	 */
	struct io_uring ring;

	unsigned  tid;

#define UBLKSRV_NR_CTX_BATCH 4
	int nr_ctxs;
	struct ublksrv_aio_ctx *ctxs[UBLKSRV_NR_CTX_BATCH];

	unsigned long reserved[8];

	struct ublk_io ios[0];
};

#define UBLKSRV_QUEUE_STOPPING	(1U << 0)
#define UBLKSRV_QUEUE_IDLE	(1U << 1)
#define UBLKSRV_QUEUE_IOCTL_OP	(1U << 2)
#define UBLKSRV_USER_COPY	(1U << 3)

struct ublksrv_io_desc {
	/* op: bit 0-7, flags: bit 8-31 */
	__u32		op_flags;

	union {
		__u32		nr_sectors;
		__u32		nr_zones; /* for UBLK_IO_OP_REPORT_ZONES */
	};

	/* start sector for this io */
	__u64		start_sector;

	/* buffer address in ublksrv daemon vm space, from ublk driver */
	__u64		addr;
};


#define UBLKSRV_CMD_BUF_OFFSET	0
#define UBLKSRV_IO_BUF_OFFSET	0x80000000
#define UBLK_MAX_QUEUE_DEPTH	4096
#define	UBLK_IO_FETCH_REQ		0x20
#define	UBLK_IO_COMMIT_AND_FETCH_REQ	0x21
#define	UBLK_IO_NEED_GET_DATA	0x22

struct ublksrv_io_cmd {
	__u16	q_id;

	/* for fetch/commit which result */
	__u16	tag;

	/* io result, it is valid for COMMIT* command only */
	__s32	result;

	union {
		/*
		 * userspace buffer address in ublksrv daemon process, valid for
		 * FETCH* command only
		 *
		 * `addr` should not be used when UBLK_F_USER_COPY is enabled,
		 * because userspace handles data copy by pread()/pwrite() over
		 * /dev/ublkcN. But in case of UBLK_F_ZONED, this union is
		 * re-used to pass back the allocated LBA for
		 * UBLK_IO_OP_ZONE_APPEND which actually depends on
		 * UBLK_F_USER_COPY
		 */
		__u64	addr;
		__u64	zone_append_lba;
	};
};



#define UBLKSRV_IO_IDLE_SECS    20
struct ublksrv_aio {
	struct ublksrv_io_desc io;
	union {
		int res;	/* output */
		int fd;		/* input */
	};

	/* reserved 31 ~ 24, bit 23 ~ 13: qid, bit 12 ~ 0: tag */
	unsigned id;
	struct ublksrv_aio *next;
	unsigned long data[0];
};


struct aio_list {
	struct ublksrv_aio *head, *tail;
};

struct ublksrv_aio_list {
	pthread_spinlock_t lock;
	struct aio_list list;
};


struct ublksrv_aio_ctx {
	struct ublksrv_aio_list submit;

	/* per-queue completion list */
	struct ublksrv_aio_list *complete;

	int efd;		//for wakeup us

#define UBLKSRV_AIO_QUEUE_WIDE	(1U << 0)
	unsigned int		flags;
	bool dead;

	const struct ublksrv_dev *dev;

	void *ctx_data;

	unsigned long reserved[8];
};

#ifndef offsetof
#define offsetof(TYPE, MEMBER)  ((size_t)&((TYPE *)0)->MEMBER)
#endif

#ifndef container_of
#define container_of(ptr, type, member) ({                              \
	unsigned long __mptr = (unsigned long)(ptr);                    \
	((type *)(__mptr - offsetof(type, member))); })
#endif

#define UBLK_IO_RES_ABORT		(-ENODEV)
#define UBLK_IO_RES_NEED_GET_DATA	1
#define UBLK_IO_RES_OK			0


#define LONGHORN_MESSAGE_MAGIC 0x1b01 // LongHorn01



enum {
    LONGHORN_CMD_TYPE_READ = 0,
    LONGHORN_CMD_TYPE_WRITE,
    LONGHORN_CMD_TYPE_RESPONSE,
    LONGHORN_CMD_TYPE_ERROR,
    LONGHORN_CMD_TYPE_EOF,
    LONGHORN_CMD_TYPE_CLOSE,
    LONGHORN_CMD_TYPE_PING,
    LONGHORN_CMD_TYPE_UNMAP,
};

#define _LONGHORN_OP_BASE     (1)
#define LONGHORN_OP_READ      (_LONGHORN_OP_BASE + LONGHORN_CMD_TYPE_READ)
#define LONGHORN_OP_WRITE     (_LONGHORN_OP_BASE + LONGHORN_CMD_TYPE_WRITE)
#define LONGHORN_OP_RESPONSE  (_LONGHORN_OP_BASE + LONGHORN_CMD_TYPE_RESPONSE)
#define LONGHORN_OP_ERROR     (_LONGHORN_OP_BASE + LONGHORN_CMD_TYPE_ERROR)
#define LONGHORN_OP_EOF       (_LONGHORN_OP_BASE + LONGHORN_CMD_TYPE_EOF)
#define LONGHORN_OP_CLOSE     (_LONGHORN_OP_BASE + LONGHORN_CMD_TYPE_CLOSE)
#define LONGHORN_OP_PING      (_LONGHORN_OP_BASE + LONGHORN_CMD_TYPE_PING)
#define LONGHORN_OP_UNMAP     (_LONGHORN_OP_BASE + LONGHORN_CMD_TYPE_UNMAP)


struct ublk_io_tgt {
	//co_handle_type co;
	const struct io_uring_cqe *tgt_io_cqe;
	int queued_tgt_io;	/* obsolete */
};

struct longhorn_io_data {
    uint32_t seq;
    uint32_t done;    // for handling partial recv
};
#endif
