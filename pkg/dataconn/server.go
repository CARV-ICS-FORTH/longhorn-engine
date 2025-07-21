package dataconn

import (
	"io"
	"net"

	"github.com/sirupsen/logrus"

	"github.com/longhorn/longhorn-engine/pkg/types"
)

type Server struct {
	wire      *SWire
	responses chan *Message
	done      chan struct{}
	data      types.DataProcessor
	messages  chan *Message
}

func NewServer(conn net.Conn, data types.DataProcessor) *Server {
	server := &Server{
		wire:      NewSWire(conn),
		responses: make(chan *Message, 4096),
		done:      make(chan struct{}, 5),
		data:      data,
	}
	server.messages = make(chan *Message, 4096)
	for i := 0; i < 4096; i++ {
		server.messages <- &Message{
			Complete:     make(chan struct{}),
			MagicVersion: MagicVersion,
			Seq:          0,
			Type:         0,
			Offset:       0,
			Size:         0,
			Data:         make([]byte, Blocks*1024),
			transportErr: nil,
		}
	}

	return server
}

func (s *Server) Handle() error {
	go s.write()
	defer func() {
		s.done <- struct{}{}
	}()
	return s.read()
}

func (s *Server) readFromWire(ret chan<- error) {
	msg, err := s.wire.SRead(s)
	if err == io.EOF {
		ret <- err
		return
	} else if err != nil {
		logrus.WithError(err).Error("Failed to read")
		ret <- err
		return
	}
	switch msg.Type {
	case TypeRead:
		go s.handleRead(msg)
	case TypeWrite:
		go s.handleWrite(msg)
	case TypeUnmap:
		go s.handleUnmap(msg)
	case TypePing:
		go s.handlePing(msg)
	default:
		panic("unhandled default case")
	}
	ret <- nil
}

func (s *Server) read() error {
	ret := make(chan error)
	for {
		go s.readFromWire(ret)

		select {
		case err := <-ret:
			if err != nil {
				return err
			}
			continue
		case <-s.done:
			logrus.Info("RPC server stopped")
			return nil
		}
	}
}

func (s *Server) Stop() {
	s.done <- struct{}{}
}

func (s *Server) handleRead(msg *Message) {
	msg.Data = msg.Data[:msg.Size]
	c, err := s.data.ReadAt(msg.Data, msg.Offset)
	s.pushResponse(c, msg, err)
}

func (s *Server) handleWrite(msg *Message) {
	c, err := s.data.WriteAt(msg.Data, msg.Offset)
	s.pushResponse(c, msg, err)
}

func (s *Server) handleUnmap(msg *Message) {
	c, err := s.data.UnmapAt(msg.Size, msg.Offset)
	s.pushResponse(c, msg, err)
}

func (s *Server) handlePing(msg *Message) {
	err := s.data.PingResponse()
	s.pushResponse(0, msg, err)
}

func (s *Server) pushResponse(count int, msg *Message, err error) {

	if msg.Type == TypeWrite || msg.Type == TypeUnmap {
		msg.Size = uint32(count)
	} else {
		msg.Size = uint32(len(msg.Data))
	}

	if err == io.EOF {
		msg.Type = TypeEOF
		msg.Data = msg.Data[:count]
		msg.Size = uint32(len(msg.Data))
	} else if err != nil {
		msg.Type = TypeError
		//msg.Data = []byte(err.Error())
		msg.Size = uint32(len(msg.Data))
	}
	s.responses <- msg
}

func (s *Server) write() {
	for {
		select {
		case msg := <-s.responses:
			if err := s.wire.SWrite(msg); err != nil {
				logrus.WithError(err).Error("Failed to write")
			}
			s.messages <- msg
		case <-s.done:
			msg := &Message{
				Type: TypeClose,
			}
			//Best effort to notify client to close connection
			if err := s.wire.SWrite(msg); err != nil {
				logrus.WithError(err).Warn("Failed to write")
			}
		}
	}
}
