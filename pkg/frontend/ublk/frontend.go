package ublk

/*
#cgo CFLAGS: -I.
#cgo LDFLAGS: -luring
#include "ublkhelper.h"
#include <stdlib.h>

*/
import "C"
import (
	"fmt"
	"github.com/longhorn/longhorn-engine/pkg/dataconn"
	"github.com/longhorn/longhorn-engine/pkg/types"
	"github.com/pkg/errors"
	"github.com/sirupsen/logrus"
	"io"
	"net"
	"os"
	"path/filepath"
	"unsafe"
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
	frontendName = "ublk"

	SocketDirectory = "/var/run"
	DevPath         = "/dev/longhorn/"
	qdepth          = 32
	chanSize        = 4096
)

type newServer struct {
	Data types.DataProcessor
}

var Done = make(chan struct{})
var msgChan = make(chan *dataconn.FrMessage, 4096)

type Ublk struct {
	Volume     string
	Size       int64
	UblkID     int
	Queues     int
	QueueDepth int
	BlockSize  int
	DaemonPId  int

	isUp         bool
	socketPath   string
	socketServer *dataconn.Server
}

func New(frontendQueues int) *Ublk {
	return &Ublk{Queues: frontendQueues}
}

func (u *Ublk) FrontendName() string {
	return frontendName
}

func (u *Ublk) Init(name string, size, sectorSize int64) error {
	u.Volume = name
	u.Size = size

	return nil
}

func (u *Ublk) Startup(rwu types.ReaderWriterUnmapperAt) error {

	go func() {
		server := dataconn.NewFrontendServer(NewDataProcessorWrapper(rwu))
		server.Handle()

	}()

	for i := range chanSize {
		msg := dataconn.FrMessage{
			Complete:     make(chan struct{}, 1),
			MagicVersion: dataconn.MagicVersion,
			Seq:          uint32(i),
			Type:         uint32(100),
			Offset:       int64(0),
			Size:         uint32(0),
			RData:        make([]byte, dataconn.Blocks*1024),
			WData:        nil,
		}
		msgChan <- &msg
	}

	err := os.MkdirAll("/tmp/ublksrvd", 0755)
	if err != nil {
		fmt.Println("Error creating directory")
		return err
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

	u.UblkID = int(dev.dev_info.dev_id)

	C.ublksrv_start_daemon(dev)
	return nil

}

func (u *Ublk) Shutdown() error {
	go C.cmd_dev_del(C.int(u.UblkID))
	<-Done
	return nil
}

func (u *Ublk) State() types.State {
	if u.isUp {
		return types.StateUp
	}
	return types.StateDown
}

func (u *Ublk) Endpoint() string {
	if u.isUp {
		return u.GetSocketPath()
	}
	return ""
}

func (u *Ublk) GetSocketPath() string {
	if u.Volume == "" {
		panic("Invalid volume name")
	}
	return filepath.Join(SocketDirectory, "longhorn-"+u.Volume+".sock")
}

func (u *Ublk) startSocketServer(rwu types.ReaderWriterUnmapperAt) error {
	socketPath := u.GetSocketPath()
	if err := os.MkdirAll(filepath.Dir(socketPath), 0700); err != nil {
		return errors.Wrapf(err, "cannot create directory %v", filepath.Dir(socketPath))
	}

	if st, err := os.Stat(socketPath); err == nil && !st.IsDir() {
		if err := os.Remove(socketPath); err != nil {
			return err
		}
	}

	u.socketPath = socketPath
	go func() {
		err := u.startSocketServerListen(rwu)
		if err != nil {
			logrus.Errorf("Failed to start socket server: %v", err)
		}
	}()
	return nil
}

func (u *Ublk) startSocketServerListen(rwu types.ReaderWriterUnmapperAt) error {
	ln, err := net.Listen("unix", u.socketPath)
	if err != nil {
		return err
	}
	defer func(ln net.Listener) {
		err := ln.Close()
		if err != nil {
			logrus.WithError(err).Error("Failed to close socket listener")
		}
	}(ln)

	for {
		conn, err := ln.Accept()
		if err != nil {
			logrus.WithError(err).Error("Failed to accept socket connection")
			continue
		}
		go u.handleServerConnection(conn, rwu)
	}
}

func (u *Ublk) handleServerConnection(c net.Conn, rwu types.ReaderWriterUnmapperAt) {
	defer func(c net.Conn) {
		err := c.Close()
		if err != nil {
			logrus.WithError(err).Error("Failed to close socket server connection")
		}
	}(c)

	server := dataconn.NewServer(c, NewDataProcessorWrapper(rwu))
	logrus.Info("New data socket connection established")
	if err := server.Handle(); err != nil && err != io.EOF {
		logrus.WithError(err).Errorf("Failed to handle socket server connection")
	} else if err == io.EOF {
		logrus.Warn("Socket server connection closed")
	}
}

//export onRequestAsync
func onRequestAsync(msg *C.struct_msghdr, req *C.struct_message, opType C.int, q *C.struct_ublksrv_queue, data *C.struct_ublk_io_data) {

	iovecs := (*[2]C.struct_iovec)(unsafe.Pointer(msg.msg_iov))[:msg.msg_iovlen:msg.msg_iovlen]

	dataPtr := iovecs[1].iov_base
	dataLen := iovecs[1].iov_len

	EngineMsg := <-msgChan

	EngineMsg.Size = uint32(req.size)
	EngineMsg.Seq = uint32(C.int(req.seq))
	EngineMsg.Type = uint32(opType)
	EngineMsg.Offset = int64(req.offset)

	if opType == LONGHORN_CMD_TYPE_WRITE {
		EngineMsg.WData = unsafe.Slice((*byte)(dataPtr), dataLen)
	}

	//fmt.Println("onRequestAsync: opType:", opType, "dataPtr:", dataPtr, "dataLen:", dataLen, "q:", q, "data:", buf)
	go func(msgObj *dataconn.FrMessage, opType C.int, dataPtr unsafe.Pointer, dataLen C.size_t, q *C.struct_ublksrv_queue, data *C.struct_ublk_io_data) {
		//fmt.Println("Request : Seq : ", msgObj.Seq, " Type : ", msgObj.Type, " Offset : ", msgObj.Offset, " Size : ", msgObj.Size)

		dataconn.Requests <- msgObj
		<-msgObj.Complete
		//fmt.Println("Reply at Request : Seq : ", msgObj.Seq, " Type : ", msgObj.Type, " Offset : ", msgObj.Offset, " Size : ", msgObj.Size)

		if opType == LONGHORN_CMD_TYPE_READ {
			//fmt.Println("Read request completed, copying data: ", msgObj.Data)
			dst := unsafe.Slice((*byte)(dataPtr), dataLen)
			copy(dst, msgObj.RData)
		}

		nrSectors := C.get_nr_sectors(data.iod)
		C.ublksrv_complete_io(q, C.uint(data.tag), C.int(nrSectors<<9))
		msgChan <- msgObj
	}(EngineMsg, opType, dataPtr, dataLen, q, data)

}

//export notifyShutdown
func notifyShutdown() {
	fmt.Println("notify shutdown chan")
	Done <- struct{}{}
}

type DataProcessorWrapper struct {
	rwu types.ReaderWriterUnmapperAt
}

func NewDataProcessorWrapper(rwu types.ReaderWriterUnmapperAt) DataProcessorWrapper {
	return DataProcessorWrapper{
		rwu: rwu,
	}
}

func (d DataProcessorWrapper) ReadAt(p []byte, off int64) (n int, err error) {
	return d.rwu.ReadAt(p, off)
	//return len(p), nil
}

func (d DataProcessorWrapper) WriteAt(p []byte, off int64) (n int, err error) {
	return d.rwu.WriteAt(p, off)
	//return len(p), nil
}

func (d DataProcessorWrapper) UnmapAt(length uint32, off int64) (n int, err error) {
	return d.rwu.UnmapAt(length, off)
}

func (d DataProcessorWrapper) PingResponse() error {
	return nil
}

func (u *Ublk) Upgrade(name string, size, sectorSize int64, rwu types.ReaderWriterUnmapperAt) error {
	return fmt.Errorf("upgrade is not supported")
}

func (u *Ublk) Expand(size int64) error {
	return fmt.Errorf("expand is not supported")
}
