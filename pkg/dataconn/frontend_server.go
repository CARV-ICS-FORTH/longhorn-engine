package dataconn

import (
	"fmt"
	"github.com/longhorn/longhorn-engine/pkg/types"
)

const (
	FrontendthreadCount = 32
)

var Requests = make(chan *Message, 1024)

type FrontendServer struct {
	responses chan *Message
	done      chan struct{}
	data      types.DataProcessor
}

func NewFrontendServer(data types.DataProcessor) *FrontendServer {
	//init theads
	server := &FrontendServer{
		responses: make(chan *Message, 1024),
		done:      make(chan struct{}, 5),
		data:      data,
	}
	for i := 0; i < FrontendthreadCount; i++ {
		go func(s *FrontendServer) {
			for {
				msg := <-Requests
				switch msg.Type {
				case TypeRead:
					s.handleRead(msg)
				case TypeWrite:
					s.handleWrite(msg)
				case TypeUnmap:
					s.handleUnmap(msg)
				case TypePing:
					s.handlePing(msg)
				}
			}
		}(server)
	}
	return server
}

func (s *FrontendServer) Stop() {
	s.done <- struct{}{}
}

func (s *FrontendServer) handleRead(msg *Message) {
	msg.Data = make([]byte, msg.Size)
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

func (s *FrontendServer) write() {

}

func (s *FrontendServer) handleRequests() {

}
