package rpc

import (
	"fmt"
	"net"

	"github.com/sirupsen/logrus"

	"github.com/longhorn/longhorn-engine/pkg/dataconn"
	replica "github.com/longhorn/longhorn-engine/pkg/replica_dbs"
	"github.com/longhorn/longhorn-engine/pkg/types"
)

type DataServer struct {
	protocol types.DataServerProtocol
	address  string
	s        *replica.Server
}

func NewDataServer(protocol types.DataServerProtocol, address string, s *replica.Server) *DataServer {
	return &DataServer{
		protocol: protocol,
		address:  address,
		s:        s,
	}
}

func (s *DataServer) ListenAndServe() error {
	switch s.protocol {
	case types.DataServerProtocolTCP:
		return s.listenAndServeTCP()
	case types.DataServerProtocolUring:
		return s.listenAndServeUring()
	case types.DataServerProtocolUNIX:
		return s.listenAndServeUNIX()
	default:
		return fmt.Errorf("unsupported protocol: %v", s.protocol)
	}
}

func (s *DataServer) listenAndServeTCP() error {
	addr, err := net.ResolveTCPAddr("tcp", s.address)
	if err != nil {
		return err
	}

	l, err := net.ListenTCP("tcp", addr)
	if err != nil {
		return err
	}

	for {
		conn, err := l.AcceptTCP()
		if err != nil {
			logrus.WithError(err).Error("failed to accept tcp connection")
			continue
		}

		logrus.Infof("New connection from: %v", conn.RemoteAddr())

		go func(conn net.Conn) {
			server := dataconn.NewServer(conn, s.s)
			err := server.Handle()
			if err != nil {
				return
			}
		}(conn)
	}
}

var numaOffset = 8

func (s *DataServer) listenAndServeUring() error {
	cpuID := 0
	addr, err := net.ResolveTCPAddr("tcp", s.address)
	if err != nil {
		return err
	}

	l, err := net.ListenTCP("tcp", addr)
	if err != nil {
		return err
	}

	for {
		conn, err := l.AcceptTCP()
		if err != nil {
			logrus.WithError(err).Error("failed to accept uring tcp connection")
			continue
		}

		logrus.Infof("New uring connection from: %v", conn.RemoteAddr())

		go func(conn net.Conn, id int) {
			server := dataconn.NewUringServer(conn, s.s)
			desiredCore := numaOffset + (id % 8)
			if err = server.Handle(desiredCore); err != nil {
				logrus.WithError(err).Warn("failed to handle uring data server")
			}
		}(conn, cpuID)
	}
}

func (s *DataServer) listenAndServeUNIX() error {
	unixAddr, err := net.ResolveUnixAddr("unix", s.address)
	if err != nil {
		return err
	}

	l, err := net.ListenUnix("unix", unixAddr)
	if err != nil {
		return err
	}

	for {
		conn, err := l.AcceptUnix()
		if err != nil {
			logrus.WithError(err).Error("failed to accept unix-domain-socket connection")
			continue
		}
		logrus.Infof("New connection from: %v", conn.RemoteAddr())
		go func(conn net.Conn) {
			server := dataconn.NewServer(conn, s.s)
			err := server.Handle()
			if err != nil {
				return
			}
		}(conn)
	}
}
