package dataconn

import (
	"errors"
	"io"
	"net"
	"os"
	"runtime"
	"sync"
	"syscall"
	"unsafe"

	"github.com/Kampadais/giouring"
	"github.com/longhorn/longhorn-engine/pkg/types"
	"github.com/longhorn/longhorn-engine/pkg/util"
	"github.com/sirupsen/logrus"
)

type uringConnection struct {
	conn            net.Conn
	file            *os.File
	fd              int
	readHeader      WireHeader
	readHeaderIovec [1]syscall.Iovec
	currentRead     *Message
	readingState    int // 0: header, 1: data

	readExpected    int
	readAccumulated int

	writeQueue       []*Message
	writeActive      []*Message
	writeMutex       sync.Mutex
	writeIovecs      [1024]syscall.Iovec
	writeExpected    int
	writeAccumulated int

	submitMu sync.Mutex
	ring     *giouring.Ring
}

type UringClient struct {
	closeOnce      sync.Once
	end            chan struct{}
	responses      chan *Message
	messages       [queueLength]*Message
	writeHeaders   [queueLength]WireHeader
	readDataIovecs [queueLength][1]syscall.Iovec
	SeqChan        chan uint32
	uConns         []*uringConnection
	peerAddr       string
	sharedTimeouts types.SharedTimeouts
}

func NewUringClient(conns []net.Conn, sharedTimeouts types.SharedTimeouts) *UringClient {
	if len(conns) == 0 {
		logrus.Fatal("no connections provided to UringClient")
	}

	c := &UringClient{
		peerAddr:       conns[0].RemoteAddr().String(),
		end:            make(chan struct{}, 4096),
		responses:      make(chan *Message, 4096),
		messages:       [queueLength]*Message{},
		SeqChan:        make(chan uint32, queueLength),
		sharedTimeouts: sharedTimeouts,
	}

	for _, conn := range conns {
		tcpConn := conn.(*net.TCPConn)
		file, _ := tcpConn.File()
		uc := &uringConnection{
			conn:         conn,
			file:         file,
			fd:           int(file.Fd()),
			readingState: 0,
			writeQueue:   make([]*Message, 0, queueLength),
		}

		ring, err := giouring.CreateRing(uint32(queueLength * 4))
		if err != nil {
			logrus.WithError(err).Fatal("Failed to create io_uring ring for client connection")
		}
		uc.ring = ring

		uc.readHeaderIovec[0] = syscall.Iovec{Base: (*byte)(unsafe.Pointer(&uc.readHeader)), Len: uint64(unsafe.Sizeof(WireHeader{}))}
		c.uConns = append(c.uConns, uc)
	}

	for i := 0; i < queueLength; i++ {
		// c.writeHeaders[i] = WireHeader{} defaults to zero-value natively
		c.messages[i] = &Message{
			Complete:     make(chan struct{}, 1),
			MagicVersion: MagicVersion,
			Seq:          uint32(i),
			Type:         0,
			Offset:       0,
			Size:         0,
			Data:         nil, // Zero-copy implementation assigns array directly from Longhorn!
			transportErr: nil,
		}
	}
	for i := uint32(0); i < queueLength; i++ {
		c.SeqChan <- i
	}

	for i, uc := range c.uConns {
		go c.readLoop(uc, i)
	}

	return c
}

func (uc *uringConnection) queueSQE(prep func(*giouring.SubmissionQueueEntry)) {
	uc.submitMu.Lock()
	defer uc.submitMu.Unlock()

	for {
		sqe := uc.ring.GetSQE()
		if sqe != nil {
			prep(sqe)
			uc.ring.Submit()
			return
		}
		uc.ring.Submit()
		uc.submitMu.Unlock()
		runtime.Gosched()
		uc.submitMu.Lock()
	}
}

func (c *UringClient) submitNextWrite(uc *uringConnection) {
	uc.writeMutex.Lock()
	defer uc.writeMutex.Unlock()

	if len(uc.writeActive) > 0 {
		return // A batch write is already active
	}

	if len(uc.writeQueue) == 0 {
		return // No pending writes
	}

	// Take up to 512 messages (1024 iovecs)
	batchSize := len(uc.writeQueue)
	if batchSize > 512 {
		batchSize = 512
	}

	uc.writeActive = uc.writeQueue[:batchSize]
	uc.writeQueue = uc.writeQueue[batchSize:]
	uc.writeAccumulated = 0
	uc.writeExpected = 0

	iovecCount := 0

	for _, msg := range uc.writeActive {
		c.writeHeaders[msg.Seq] = WireHeader{
			Offset:       msg.Offset,
			Seq:          msg.Seq,
			Type:         msg.Type,
			Size:         msg.Size,
			DataLength:   0,
			MagicVersion: msg.MagicVersion,
		}

		headerPtr := (*byte)(unsafe.Pointer(&c.writeHeaders[msg.Seq]))
		headerSize := uint64(unsafe.Sizeof(WireHeader{}))

		if msg.Type == TypeWrite {
			c.writeHeaders[msg.Seq].DataLength = uint32(len(msg.Data))
			uc.writeExpected += int(headerSize) + len(msg.Data)
			uc.writeIovecs[iovecCount] = syscall.Iovec{Base: headerPtr, Len: headerSize}
			iovecCount++
			uc.writeIovecs[iovecCount] = syscall.Iovec{Base: &msg.Data[0], Len: uint64(len(msg.Data))}
			iovecCount++
		} else {
			uc.writeExpected += int(headerSize)
			uc.writeIovecs[iovecCount] = syscall.Iovec{Base: headerPtr, Len: headerSize}
			iovecCount++
		}
	}

	uc.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
		sqe.PrepareWritev(uc.fd, uintptr(unsafe.Pointer(&uc.writeIovecs[0])), uint32(iovecCount), 0)
		sqe.UserData = 10000
	})
}

func (c *UringClient) resumeWrite(uc *uringConnection, res int) {
	uc.writeMutex.Lock()
	defer uc.writeMutex.Unlock()

	uc.writeAccumulated += res
	if uc.writeAccumulated >= uc.writeExpected { // Done!
		uc.writeActive = nil
		uc.writeMutex.Unlock()
		c.submitNextWrite(uc)
		uc.writeMutex.Lock()
		return
	}

	// Adjust iovecs for short write across the batch
	remaining := res
	iovecCount := 0
	var startIndex int

	// First, determine the maximum possible iovecs utilized in this batch
	var totalIovecs int
	for _, msg := range uc.writeActive {
		if msg.Type == TypeWrite {
			totalIovecs += 2
		} else {
			totalIovecs += 1
		}
	}

	for i := 0; i < totalIovecs; i++ {
		iovLen := int(uc.writeIovecs[i].Len)
		if iovLen == 0 {
			continue // Already fully consumed in a prior partial write
		}

		if remaining >= iovLen {
			remaining -= iovLen
			uc.writeIovecs[i].Len = 0
		} else {
			uc.writeIovecs[i].Base = (*byte)(unsafe.Pointer(uintptr(unsafe.Pointer(uc.writeIovecs[i].Base)) + uintptr(remaining)))
			uc.writeIovecs[i].Len -= uint64(remaining)
			startIndex = i
			iovecCount = totalIovecs - i
			break
		}
	}

	uc.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
		sqe.PrepareWritev(uc.fd, uintptr(unsafe.Pointer(&uc.writeIovecs[startIndex])), uint32(iovecCount), 0)
		sqe.UserData = 10000
	})
}

// Write queue serialization successfully resolved in operation()

func (c *UringClient) readLoop(uc *uringConnection, cpuID int) {

	if err := util.PinToCore(cpuID); err != nil {
		logrus.WithError(err).Warnf("Failed to pin client readLoop to core %d", cpuID)
	}
	c.submitReadHeader(uc)

	for {
		cqe, err := uc.ring.WaitCQE()
		if err != nil {
			select {
			case <-c.end:
				return
			default:
				if err.Error() == "interrupted system call" || err.Error() == "EINTR" {
					continue
				}
				logrus.WithError(err).Error("WaitCQE error")
				continue
			}
		}

		select {
		case <-c.end:
			uc.ring.CQESeen(cqe)
			return
		default:
		}

		ucIdx := int(cqe.UserData)
		if ucIdx == 10000 {
			if cqe.Res <= 0 {
				logrus.Errorf("Write CQE error result: %v", cqe.Res)
				uc.ring.CQESeen(cqe)
				go c.Close()
				return
			}
			c.resumeWrite(uc, int(cqe.Res))
			uc.ring.CQESeen(cqe)
			continue
		}

		if cqe.Res <= 0 {
			logrus.Errorf("CQE read error or EOF: %v", cqe.Res)
			uc.ring.CQESeen(cqe)
			go c.Close()
			return
		}

		res := int(cqe.Res)
		uc.readAccumulated += res

		if uc.readAccumulated < uc.readExpected {
			// Short read! Adjust pointer and wait for more.
			if uc.readingState == 0 {
				uc.readHeaderIovec[0].Base = (*byte)(unsafe.Pointer(uintptr(unsafe.Pointer(uc.readHeaderIovec[0].Base)) + uintptr(res)))
				uc.readHeaderIovec[0].Len -= uint64(res)

				uc.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
					sqe.PrepareReadv(uc.fd, uintptr(unsafe.Pointer(&uc.readHeaderIovec[0])), 1, 0)
					sqe.UserData = 0
				})
			} else {
				msg := uc.currentRead
				c.readDataIovecs[msg.Seq][0].Base = (*byte)(unsafe.Pointer(uintptr(unsafe.Pointer(c.readDataIovecs[msg.Seq][0].Base)) + uintptr(res)))
				c.readDataIovecs[msg.Seq][0].Len -= uint64(res)

				uc.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
					sqe.PrepareReadv(uc.fd, uintptr(unsafe.Pointer(&c.readDataIovecs[msg.Seq][0])), 1, 0)
					sqe.UserData = 1
				})
			}
			uc.ring.CQESeen(cqe)
			continue
		}

		// Full read completed
		if uc.readingState == 0 {
			if uc.readHeader.MagicVersion != MagicVersion {
				logrus.Errorf("wrong API version received: 0x%x", uc.readHeader.MagicVersion)
			}

			Seq := uc.readHeader.Seq

			if Seq >= uint32(len(c.messages)) {
				logrus.Errorf("invalid sequence number: %v", Seq)
				uc.ring.CQESeen(cqe)
				c.submitReadHeader(uc)
				continue
			}
			msg := c.messages[Seq]
			msg.Type = uc.readHeader.Type
			msg.Offset = uc.readHeader.Offset
			msg.Size = uc.readHeader.Size
			length := uc.readHeader.DataLength

			if length > 0 {
				msg.Data = msg.Data[:length]
				uc.currentRead = msg
				uc.readingState = 1
				c.submitReadData(uc)
			} else {
				c.handleResponse(msg)
				c.submitReadHeader(uc)
			}
		} else {
			c.handleResponse(uc.currentRead)
			uc.readingState = 0
			c.submitReadHeader(uc)
		}

		uc.ring.CQESeen(cqe)
	}
}

func (c *UringClient) submitReadHeader(uc *uringConnection) {
	uc.readAccumulated = 0
	uc.readExpected = int(unsafe.Sizeof(WireHeader{}))
	uc.readHeaderIovec[0] = syscall.Iovec{Base: (*byte)(unsafe.Pointer(&uc.readHeader)), Len: uint64(unsafe.Sizeof(WireHeader{}))}

	uc.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
		sqe.PrepareReadv(uc.fd, uintptr(unsafe.Pointer(&uc.readHeaderIovec[0])), 1, 0)
		sqe.UserData = 0 // Using constant inside uc
	})
}

func (c *UringClient) submitReadData(uc *uringConnection) {
	uc.readAccumulated = 0
	uc.readExpected = len(uc.currentRead.Data)
	c.readDataIovecs[uc.currentRead.Seq][0] = syscall.Iovec{Base: &uc.currentRead.Data[0], Len: uint64(len(uc.currentRead.Data))}

	uc.queueSQE(func(sqe *giouring.SubmissionQueueEntry) {
		sqe.PrepareReadv(uc.fd, uintptr(unsafe.Pointer(&c.readDataIovecs[uc.currentRead.Seq][0])), 1, 0)
		sqe.UserData = 1 // Using constant inside uc
	})
}

func (c *UringClient) handleResponse(resp *Message) {
	select {
	case resp.Complete <- struct{}{}:
	default:
	}
}

func (c *UringClient) TargetID() string {
	return c.peerAddr
}

func (c *UringClient) WriteAt(buf []byte, offset int64) (int, error) {
	return c.operation(TypeWrite, buf, uint32(len(buf)), offset)
}

func (c *UringClient) UnmapAt(length uint32, offset int64) (int, error) {
	return c.operation(TypeUnmap, nil, length, offset)
}

func (c *UringClient) SetError(err error) {
	c.responses <- &Message{transportErr: err}
}

func (c *UringClient) ReadAt(buf []byte, offset int64) (int, error) {
	return c.operation(TypeRead, buf, uint32(len(buf)), offset)
}

func (c *UringClient) Ping() error {
	_, err := c.operation(TypePing, nil, 0, 0)
	return err
}

func (c *UringClient) operation(op uint32, buf []byte, length uint32, offset int64) (int, error) {
	seq := <-c.SeqChan
	msg := c.messages[seq]

	// Zero-copy injection: io_uring uses this pointer directly
	msg.Data = buf
	msg.Type = op
	msg.Offset = offset
	msg.Size = length

	// Eliminate global channel bottleneck: route writes locally and trigger queue
	uc := c.uConns[msg.Seq%uint32(len(c.uConns))]
	uc.writeMutex.Lock()
	uc.writeQueue = append(uc.writeQueue, msg)
	uc.writeMutex.Unlock()
	c.submitNextWrite(uc)

	<-msg.Complete

	c.SeqChan <- msg.Seq

	if msg.Type == TypeError {
		return 0, errors.New("backend error")
	}
	if msg.Type == TypeEOF {
		return int(msg.Size), io.EOF
	}

	return int(msg.Size), nil
}

func (c *UringClient) replyError(req *Message, err error) {
	req.Type = TypeError
	req.Data = []byte(err.Error())
	select {
	case req.Complete <- struct{}{}:
	default:
	}
}

func (c *UringClient) Close() {
	c.closeOnce.Do(func() {
		close(c.end)

		for _, uc := range c.uConns {
			if uc.file != nil {
				uc.file.Close()
			}
			if uc.conn != nil {
				uc.conn.Close()
			}
		}

		for _, uc := range c.uConns {
			uc.ring.QueueExit()
		}

		for i := 0; i < queueLength; i++ {
			go func(idx int) {
				msg := c.messages[idx]
				select {
				case <-msg.Complete:
				default:
					c.replyError(msg, errors.New("connection closed"))
					select {
					case c.SeqChan <- msg.Seq:
					default:
					}
				}
			}(i)
		}
	})
}
