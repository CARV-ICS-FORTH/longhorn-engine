package ublk

/*
#cgo CFLAGS: -I.
#cgo LDFLAGS: -luring
#include "ublkhelper.h"
#include "includes.h"
#include <stdlib.h>

*/
import "C"
import (
	"fmt"
	"github.com/longhorn/longhorn-engine/pkg/dataconn"
	"os"
	"unsafe"
	//"runtime"
	//"time"
)

const (
	LONGHORN_CMD_TYPE_READ = iota
	LONGHORN_CMD_TYPE_WRITE
	LONGHORN_CMD_TYPE_RESPONSE
	LONGHORN_CMD_TYPE_ERROR
	LONGHORN_CMD_TYPE_EOF
	LONGHORN_CMD_TYPE_CLOSE
	LONGHORN_CMD_TYPE_PING
	LONGHORN_CMD_TYPE_UNMAP
)

func addDev() {

	err := os.MkdirAll("/tmp/ublksrvd", 0755)
	if err != nil {
		fmt.Println("Error creating directory")
		return
	}
	queueDepth := C.DEF_QD
	nrHwQueues := C.DEF_NR_HW_QUEUES
	devId := -1
	runDir := C.UBLKSRV_PID_DIR
	maxIOBufBytes := C.DEF_BUF_SIZE

	data := C.struct_ublksrv_dev_data{
		queue_depth:      C.ushort(queueDepth),
		nr_hw_queues:     C.ushort(nrHwQueues),
		dev_id:           C.int(devId),
		run_dir:          C.CString(runDir),
		max_io_buf_bytes: C.uint(maxIOBufBytes),
	}

	dev := C.ublksrv_ctrl_init(&data)
	C.ublksrv_ctrl_add_dev(dev)
	C.init_params(dev, &data)

	C.ublksrv_start_daemon(dev)

	C.ublksrv_ctrl_start_dev(dev, C.int(os.Getpid()))

}

//export onRequest
func onRequest(msg *C.struct_msghdr, req *C.struct_message, opType C.int) {

	iovecs := (*[2]C.struct_iovec)(unsafe.Pointer(msg.msg_iov))[:msg.msg_iovlen:msg.msg_iovlen]

	// Second buffer
	dataPtr := iovecs[1].iov_base
	dataLen := iovecs[1].iov_len
	var data []byte
	// Convert to Go []byte safely
	if opType == LONGHORN_CMD_TYPE_WRITE {
		data = unsafe.Slice((*byte)(dataPtr), dataLen)
	} else {
		data = make([]byte, dataLen)
	}
	EngineMsg := dataconn.Message{
		Complete:     make(chan struct{}),
		MagicVersion: dataconn.MagicVersion,
		Seq:          uint32(C.int(req.seq)),
		Type:         uint32(opType),
		Offset:       int64(req.offset),
		Size:         uint32(req.size),
		Data:         data,
	}

	dataconn.Requests <- &EngineMsg
	<-EngineMsg.Complete
	if opType == LONGHORN_CMD_TYPE_READ {
		dst := unsafe.Slice((*byte)(dataPtr), dataLen)
		copy(dst, EngineMsg.Data)
	}

}

//export onRequestAsync
func onRequestAsync(msg *C.struct_msghdr, req *C.struct_message, opType C.int, q *C.struct_ublksrv_queue, data *C.struct_ublk_io_data) {

	//fmt.Println("called at ", time.Now(), "for IO tag ", int(req.seq))
	iovecs := (*[2]C.struct_iovec)(unsafe.Pointer(msg.msg_iov))[:msg.msg_iovlen:msg.msg_iovlen]

	// Second buffer
	dataPtr := iovecs[1].iov_base
	dataLen := iovecs[1].iov_len
	var buf []byte
	// Convert to Go []byte safely
	if opType == LONGHORN_CMD_TYPE_WRITE {
		buf = unsafe.Slice((*byte)(dataPtr), dataLen)
	} else {
		buf = make([]byte, dataLen)
	}
	EngineMsg := dataconn.Message{
		Complete:     make(chan struct{}),
		MagicVersion: dataconn.MagicVersion,
		Seq:          uint32(C.int(req.seq)),
		Type:         uint32(opType),
		Offset:       int64(req.offset),
		Size:         uint32(req.size),
		Data:         buf,
	}

	go func(msgObj *dataconn.Message, opType C.int, dataPtr unsafe.Pointer, dataLen C.size_t, q *C.struct_ublksrv_queue, data *C.struct_ublk_io_data) {
		//	fmt.Println("inside go func at ", time.Now(), "for IO tag ", msgObj.Seq)

		dataconn.Requests <- msgObj
		<-msgObj.Complete
		if opType == LONGHORN_CMD_TYPE_READ {
			dst := unsafe.Slice((*byte)(dataPtr), dataLen)
			copy(dst, msgObj.Data)
		}
		nrSectors := C.get_nr_sectors(data.iod)
		C.ublksrv_complete_io(q, C.uint(data.tag), C.int(nrSectors<<9))
		//	fmt.Println("End of complete_io at ", time.Now(), "for IO tag ", msgObj.Seq)
	}(&EngineMsg, opType, dataPtr, dataLen, q, data)
}

//export handleReplies
func handleReplies(counter C.int) {
	for counter > 0 {
		reply := <-dataconn.Replies
		reply.Complete <- struct{}{}
		counter--
	}
}
