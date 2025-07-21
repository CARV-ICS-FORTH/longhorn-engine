package dataconn

import (
	"fmt"
	"github.com/longhorn/longhorn-engine/pkg/types"
)

type FrMessage struct {
	Complete     chan struct{}
	MagicVersion uint16
	Seq          uint32
	Type         uint32
	Offset       int64
	Size         uint32
	RData        []byte
	WData        []byte
}

var Requests = make(chan *FrMessage, 4096)

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

func (s *FrontendServer) handleRead(msg *FrMessage) {
	msg.RData = msg.RData[:msg.Size]
	//fmt.Println("Data size : ", len(msg.Data))
	//msg.Data = make([]byte, msg.Size)
	_, err := s.data.ReadAt(msg.RData, msg.Offset)
	if err != nil {
		fmt.Println(err)
	}
	msg.Complete <- struct{}{}
}

func (s *FrontendServer) handleWrite(msg *FrMessage) {
	_, err := s.data.WriteAt(msg.WData, msg.Offset)
	if err != nil {
		fmt.Println(err)
	}
	msg.Complete <- struct{}{}
}

func (s *FrontendServer) handleUnmap(msg *FrMessage) {
	_, err := s.data.UnmapAt(msg.Size, msg.Offset)
	if err != nil {
		fmt.Println(err)
	}
	msg.Complete <- struct{}{}
}

func (s *FrontendServer) handlePing(msg *FrMessage) {
	err := s.data.PingResponse()
	if err != nil {
		fmt.Println(err)
	}
	msg.Complete <- struct{}{}
}
