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

type IORequest struct {
	Offset int64
	Data   []byte
	Type   int
	Done   chan error
}

var ioChan = make(chan IORequest)

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

func startIOHandler() {
	go func() {
		for req := range ioChan {
			var err error
			if err != nil {
				fmt.Println("Error in io handler")
				continue
			}
			fmt.Println(req)
			req.Done <- err
		}
	}()
}

//export onRequest
func onRequest(msg *C.struct_msghdr, req *C.struct_message, opType C.int) {

	iovecs := (*[2]C.struct_iovec)(unsafe.Pointer(msg.msg_iov))[:msg.msg_iovlen:msg.msg_iovlen]

	// Second buffer
	dataPtr := iovecs[1].iov_base
	dataLen := iovecs[1].iov_len

	// Convert to Go []byte safely
	data := C.GoBytes(dataPtr, C.int(dataLen))
	done := make(chan error)
	switch opType {
	case LONGHORN_CMD_TYPE_READ:

		//_, err := testServer.Data.ReadAt(data, int64(req.offset))
		//if err != nil {
		//	return
		//}
		ioChan <- IORequest{
			Offset: int64(req.offset),
			Data:   data,
			Type:   int(opType),
			Done:   done,
		}

		err := <-done
		if err != nil {
			fmt.Println("Error in io handler")
		}
		fmt.Println("Read at offset : ", int64(req.offset), " with size : ", req.size)
		break
	case LONGHORN_CMD_TYPE_WRITE:
		//at, err := testRwu.WriteAt(data, int64(req.offset))
		//if err != nil {
		//	return
		//}
		ioChan <- IORequest{
			Offset: int64(req.offset),
			Data:   data,
			Type:   int(opType),
			Done:   done,
		}
		err := <-done
		if err != nil {
			fmt.Println("Error in io handler")
		}
		fmt.Println("Write data : ", string(data), " at offset : ", int64(req.offset), " with size : ", req.size)
		break
	default:
		fmt.Println("Unknown command type : ", opType)

	}

}
