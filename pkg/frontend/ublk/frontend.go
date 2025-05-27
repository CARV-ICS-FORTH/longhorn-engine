package ublk

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
)

const (
	frontendName = "ublk"

	SocketDirectory = "/var/run"
	DevPath         = "/dev/longhorn/"
	qdepth          = 32
)

type newServer struct {
	Data types.DataProcessor
}

var testServer *newServer

func New(frontendQueues int) *Ublk {
	return &Ublk{Queues: frontendQueues}
}

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

func (u *Ublk) FrontendName() string {
	return frontendName
}

func (u *Ublk) Init(name string, size, sectorSize int64) error {
	u.Volume = name
	u.Size = size

	return nil
}

func (u *Ublk) Startup(rwu types.ReaderWriterUnmapperAt) error {

	dataconn.NewFrontendServer(NewDataProcessorWrapper(rwu))
	logrus.Info("New frontend server established")

	u.addDev()
	return nil

}

func (u *Ublk) Shutdown() error {
	u.shutDownC()
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
