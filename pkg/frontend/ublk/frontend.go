package ublk

import (
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"

	"github.com/longhorn/longhorn-engine/pkg/dataconn"
	"github.com/longhorn/longhorn-engine/pkg/types"
	"github.com/pkg/errors"
	"github.com/sirupsen/logrus"

	ublk "github.com/Kampadais/GoUblksrv"
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
	chanSize        = 512
)

type newServer struct {
	Data types.DataProcessor
}

var Done = make(chan struct{})
var msgChan = make(chan *dataconn.FrMessage, 4096)

type Ublk struct {
	dev          *ublk.UblkDevice
	socketPath   string
	socketServer *dataconn.Server
}

func New(options types.FrontendOptions) *Ublk {
	params := ublk.UblkParams{
		Queues:     options.UblkSrvOptions.Queues,
		QueueDepth: options.UblkSrvOptions.QueueDepth,
	}
	ublkDev, err := ublk.NewUblkDevice("", params)
	if err != nil {
		logrus.Errorf("Failed to create ublk device: %v", err)
		return nil
	}
	return &Ublk{
		dev: ublkDev,
	}
}

func (u *Ublk) FrontendName() string {
	return frontendName
}

func (u *Ublk) Init(name string, size, sectorSize int64) error {
	u.dev.Volume = name
	u.dev.Size = size

	return nil
}

func (u *Ublk) Startup(rwu types.ReaderWriterUnmapperAt) error {

	u.dev.Start(rwu)

	return nil
}

func (u *Ublk) Shutdown() error {
	u.dev.Delete()
	return nil
}

func (u *Ublk) State() types.State {
	info, err := u.dev.GetInfo()
	if err != nil {
		logrus.Errorf("Failed to get info from device: %v", err)
	}

	if info.State == ublk.StateLive {
		return types.StateUp
	}
	return types.StateDown

}

func (u *Ublk) Endpoint() string {
	if u.State() == types.StateUp {
		return "/dev/ublkb" + strconv.Itoa(u.dev.ID)
	}
	return ""
}

func (u *Ublk) GetSocketPath() string {
	if u.dev.Volume == "" {
		panic("Invalid volume name")
	}
	return filepath.Join(SocketDirectory, "longhorn-"+u.dev.Volume+".sock")
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
