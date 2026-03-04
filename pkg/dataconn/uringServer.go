package dataconn

import (
	"io"
	"net"
	"os"
	"runtime"
	"sync"
	"syscall"
	"unsafe"

	"github.com/Kampadais/giouring"
	"github.com/longhorn/longhorn-engine/pkg/types"
	"github.com/sirupsen/logrus"
)

type UringServer struct {
	done chan struct{}
	data types.DataProcessor

	conn           net.Conn
	file           *os.File
	fd             int
	writeHeaders   [queueLength]WireHeader
	readHeader     WireHeader
	serverMessages [queueLength]*Message

	writeIovecs      [1024]syscall.Iovec
	writeQueue       []*Message
	writeActive      []*Message
	writeMutex       sync.Mutex
	writeExpected    int
	writeAccumulated int

	readHeaderIov  [1]syscall.Iovec
	readDataIovecs [queueLength][1]syscall.Iovec

	readExpected    int
	readAccumulated int
	readingState    int // 0: header, 1: data
	currentRead     *Message

	submitMu sync.Mutex
	ring     *giouring.Ring
}

func NewUringServer(conn net.Conn, data types.DataProcessor) *UringServer {
	tcpConn := conn.(*net.TCPConn)
	file, _ := tcpConn.File()

	ring, err := giouring.CreateRing(uint32(queueLength * 4))
	if err != nil {
		logrus.WithError(err).Fatal("Failed to create io_uring ring for server")
	}

	server := &UringServer{
		done:       make(chan struct{}, 5),
		data:       data,
		conn:       conn,
		file:       file,
		fd:         int(file.Fd()),
		ring:       ring,
		writeQueue: make([]*Message, 0, queueLength),
	}
	server.readHeaderIov[0] = syscall.Iovec{Base: (*byte)(unsafe.Pointer(&server.readHeader)), Len: uint64(unsafe.Sizeof(WireHeader{}))}

	for i := 0; i < queueLength; i++ {
		// server.writeHeaders[i] = WireHeader{} defaults to zero-value natively
		server.serverMessages[i] = &Message{
			Complete:     make(chan struct{}, 1),
			MagicVersion: MagicVersion,
			Seq:          uint32(i),
			Type:         0,
			Offset:       0,
			Size:         0,
			Data:         nil, // Zero copy dynamically assigned per-cycle
			transportErr: nil,
		}
	}

	return server
}

func (s *UringServer) Handle() error {
	defer func() {
		s.done <- struct{}{}
		if s.file != nil {
			s.file.Close()
		}
		if s.conn != nil {
			s.conn.Close()
		}
		s.ring.QueueExit()
	}()
	return s.read()
}

func (s *UringServer) queueSQE(prep func(*giouring.SubmissionQueueEntry)) {
	s.submitMu.Lock()
	defer s.submitMu.Unlock()

	for {
		sqe := s.ring.GetSQE()
		if sqe != nil {
			prep(sqe)
			s.ring.Submit()
			return
		}
		s.ring.Submit()
		s.submitMu.Unlock()
		runtime.Gosched()
		s.submitMu.Lock()
	}
}

func (s *UringServer) submitNextWrite() {
	s.writeMutex.Lock()
	defer s.writeMutex.Unlock()

	if len(s.writeActive) > 0 || len(s.writeQueue) == 0 {
		return
	}

	batchSize := len(s.writeQueue)
	if batchSize > 512 {
		batchSize = 512
	}

	s.writeActive = s.writeQueue[:batchSize]
	s.writeQueue = s.writeQueue[batchSize:]
	s.writeAccumulated = 0
	s.writeExpected = 0

	iovecCount := 0

	for _, msg := range s.writeActive {
		s.writeHeaders[msg.Seq] = WireHeader{
			Offset:       msg.Offset,
			Seq:          msg.Seq,
			Type:         msg.Type,
			Size:         msg.Size,
			DataLength:   0,
			MagicVersion: msg.MagicVersion,
		}

		headerPtr := (*byte)(unsafe.Pointer(&s.writeHeaders[msg.Seq]))
		headerSize := uint64(unsafe.Sizeof(WireHeader{}))

		if msg.Type == TypeRead {
			s.writeHeaders[msg.Seq].DataLength = uint32(len(msg.Data))
			s.writeExpected += int(headerSize) + len(msg.Data)
			s.writeIovecs[iovecCount] = syscall.Iovec{Base: headerPtr, Len: headerSize}
			iovecCount++
			s.writeIovecs[iovecCount] = syscall.Iovec{Base: &msg.Data[0], Len: uint64(len(msg.Data))}
			iovecCount++
		} else {
			s.writeExpected += int(headerSize)
			s.writeIovecs[iovecCount] = syscall.Iovec{Base: headerPtr, Len: headerSize}
			iovecCount++
		}
	}

	s.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
		sqe.PrepareWritev(s.fd, uintptr(unsafe.Pointer(&s.writeIovecs[0])), uint32(iovecCount), 0)
		sqe.UserData = 555
	})
}

func (s *UringServer) resumeWrite(res int) {
	s.writeMutex.Lock()
	defer s.writeMutex.Unlock()

	s.writeAccumulated += res
	if s.writeAccumulated >= s.writeExpected {
		s.writeActive = nil
		s.writeMutex.Unlock()
		s.submitNextWrite()
		s.writeMutex.Lock()
		return
	}

	remaining := res
	iovecCount := 0
	var startIndex int

	// Determine max possible iovecs
	var totalIovecs int
	for _, msg := range s.writeActive {
		if msg.Type == TypeRead {
			totalIovecs += 2
		} else {
			totalIovecs += 1
		}
	}

	for i := 0; i < totalIovecs; i++ {
		iovLen := int(s.writeIovecs[i].Len)
		if iovLen == 0 {
			continue // Already fully consumed in a prior partial write
		}

		if remaining >= iovLen {
			remaining -= iovLen
			s.writeIovecs[i].Len = 0
		} else {
			s.writeIovecs[i].Base = (*byte)(unsafe.Pointer(uintptr(unsafe.Pointer(s.writeIovecs[i].Base)) + uintptr(remaining)))
			s.writeIovecs[i].Len -= uint64(remaining)
			startIndex = i
			iovecCount = totalIovecs - i
			break
		}
	}

	s.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
		sqe.PrepareWritev(s.fd, uintptr(unsafe.Pointer(&s.writeIovecs[startIndex])), uint32(iovecCount), 0)
		sqe.UserData = 555
	})
}

// Write loop globally bypassed by direct multiplexing on pushResponse

func (s *UringServer) read() error {
	s.submitReadHeader()

	for {
		select {
		case <-s.done:
			logrus.Info("Uring RPC server stopped")
			return nil
		default:
			cqe, err := s.ring.WaitCQE()
			if err != nil {
				if err.Error() == "interrupted system call" || err.Error() == "EINTR" {
					continue
				}
				continue
			}

			if cqe.Res < 0 {
				logrus.Errorf("CQE error result: %v", cqe.Res)
				s.ring.CQESeen(cqe)
				return io.EOF
			}
			if cqe.Res == 0 {
				s.ring.CQESeen(cqe)
				return io.EOF
			}

			state := cqe.UserData
			if state == 555 {
				s.resumeWrite(int(cqe.Res))
				s.ring.CQESeen(cqe)
				continue
			}

			res := int(cqe.Res)
			s.readAccumulated += res

			if s.readAccumulated < s.readExpected {
				// Short read handled inline
				if s.readingState == 0 {
					s.readHeaderIov[0].Base = (*byte)(unsafe.Pointer(uintptr(unsafe.Pointer(s.readHeaderIov[0].Base)) + uintptr(res)))
					s.readHeaderIov[0].Len -= uint64(res)

					s.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
						sqe.PrepareReadv(s.fd, uintptr(unsafe.Pointer(&s.readHeaderIov[0])), 1, 0)
						sqe.UserData = 0
					})
				} else {
					seq := s.currentRead.Seq
					s.readDataIovecs[seq][0].Base = (*byte)(unsafe.Pointer(uintptr(unsafe.Pointer(s.readDataIovecs[seq][0].Base)) + uintptr(res)))
					s.readDataIovecs[seq][0].Len -= uint64(res)

					s.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
						sqe.PrepareReadv(s.fd, uintptr(unsafe.Pointer(&s.readDataIovecs[seq][0])), 1, 0)
						sqe.UserData = uint64(seq)
					})
				}
				s.ring.CQESeen(cqe)
				continue
			}

			if s.readingState == 0 {
				if s.readHeader.MagicVersion != MagicVersion {
					logrus.Errorf("wrong API version received: 0x%x", s.readHeader.MagicVersion)
				}
				seq := s.readHeader.Seq

				if seq >= uint32(len(s.serverMessages)) {
					logrus.Errorf("invalid sequence number on server read: %v", seq)
					s.submitReadHeader()
					s.ring.CQESeen(cqe)
					continue
				}

				msg := s.serverMessages[seq]
				msg.Type = s.readHeader.Type
				msg.Offset = s.readHeader.Offset
				msg.Size = s.readHeader.Size
				length := s.readHeader.DataLength
				if length > 0 {
					// Allocate specifically only when needed (if this isn't handled by backend)
					// In an ideal zero-copy scenario from the engine, Longhorn will provide a buffer pool here.
					// For now, if length > 0, we quickly provision an exact sized slice
					// if backend allows, we can further optimize this.
					if cap(msg.Data) < int(length) {
						msg.Data = make([]byte, length)
					}
					msg.Data = msg.Data[:length]
					s.currentRead = msg
					s.readingState = 1
					s.submitReadData(msg)
				} else {
					s.routeMessage(msg)
					s.submitReadHeader()
				}
			} else {
				s.routeMessage(s.currentRead)
				s.readingState = 0
				s.submitReadHeader()
			}

			s.ring.CQESeen(cqe)
		}
	}
}

func (s *UringServer) submitReadHeader() {
	s.readAccumulated = 0
	s.readExpected = int(unsafe.Sizeof(WireHeader{}))
	s.readHeaderIov[0] = syscall.Iovec{Base: (*byte)(unsafe.Pointer(&s.readHeader)), Len: uint64(unsafe.Sizeof(WireHeader{}))}

	s.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
		sqe.PrepareReadv(s.fd, uintptr(unsafe.Pointer(&s.readHeaderIov[0])), 1, 0)
		sqe.UserData = 0 // 0 implies header
	})
}

func (s *UringServer) submitReadData(msg *Message) {
	s.readAccumulated = 0
	s.readExpected = len(msg.Data)
	s.readDataIovecs[msg.Seq][0] = syscall.Iovec{Base: &msg.Data[0], Len: uint64(len(msg.Data))}

	s.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
		sqe.PrepareReadv(s.fd, uintptr(unsafe.Pointer(&s.readDataIovecs[msg.Seq][0])), 1, 0)
		sqe.UserData = uint64(msg.Seq)
	})
}

func (s *UringServer) routeMessage(msg *Message) {
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
		logrus.Errorf("Unhandled message type: %v", msg.Type)
	}
}

func (s *UringServer) Stop() {
	select {
	case s.done <- struct{}{}:
	default:
	}
}

func (s *UringServer) handleRead(msg *Message) {
	if cap(msg.Data) < int(msg.Size) {
		msg.Data = make([]byte, msg.Size)
	}
	msg.Data = msg.Data[:msg.Size]
	c, err := s.data.ReadAt(msg.Data, msg.Offset)
	s.pushResponse(c, msg, err)
}

func (s *UringServer) handleWrite(msg *Message) {
	c, err := s.data.WriteAt(msg.Data, msg.Offset)
	s.pushResponse(c, msg, err)
}

func (s *UringServer) handleUnmap(msg *Message) {
	c, err := s.data.UnmapAt(msg.Size, msg.Offset)
	s.pushResponse(c, msg, err)
}

func (s *UringServer) handlePing(msg *Message) {
	err := s.data.PingResponse()
	s.pushResponse(0, msg, err)
}

func (s *UringServer) pushResponse(count int, msg *Message, err error) {
	if msg.Type == TypeWrite || msg.Type == TypeUnmap {
		msg.Size = uint32(count)
	} else {
		msg.Size = uint32(len(msg.Data))
	}

	if err == io.EOF {
		msg.Type = TypeEOF
		msg.Size = uint32(len(msg.Data))
	} else if err != nil {
		msg.Type = TypeError
		msg.Size = uint32(len(msg.Data))
	}
	s.writeMutex.Lock()
	s.writeQueue = append(s.writeQueue, msg)
	s.writeMutex.Unlock()
	s.submitNextWrite()
}
