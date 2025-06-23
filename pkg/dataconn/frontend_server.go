package dataconn

import (
	"fmt"
	"github.com/longhorn/longhorn-engine/pkg/types"
)

var Requests = make(chan *Message, 4096)

type FrontendServer struct {
	data types.DataProcessor
}

func NewFrontendServer(data types.DataProcessor) *FrontendServer {
	return &FrontendServer{
		data: data,
	}
}

func (s *FrontendServer) Handle() {
	for {
		msg := <-Requests
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
			fmt.Printf("Unknown message type: %d\n", msg.Type)
			msg.Complete <- struct{}{}
		}
	}
}

func (s *FrontendServer) handleRead(msg *Message) {
	//msg.Data = make([]byte, msg.Size)
	_, err := s.data.ReadAt(msg.Data, msg.Offset)
	if err != nil {
		fmt.Println(err)
	}
	msg.Complete <- struct{}{}
}

func (s *FrontendServer) handleWrite(msg *Message) {
	_, err := s.data.WriteAt(msg.Data, msg.Offset)
	if err != nil {
		fmt.Println(err)
	}
	msg.Complete <- struct{}{}
}

func (s *FrontendServer) handleUnmap(msg *Message) {
	_, err := s.data.UnmapAt(msg.Size, msg.Offset)
	if err != nil {
		fmt.Println(err)
	}
	msg.Complete <- struct{}{}
}

func (s *FrontendServer) handlePing(msg *Message) {
	err := s.data.PingResponse()
	if err != nil {
		fmt.Println(err)
	}
	msg.Complete <- struct{}{}
}
