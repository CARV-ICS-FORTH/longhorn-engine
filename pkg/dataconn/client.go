package dataconn

import (
	"errors"
	"io"
	"net"

	"github.com/longhorn/longhorn-engine/pkg/types"
	"github.com/sirupsen/logrus"
)

const (
	queueLength = 512
	Blocks      = 512
)

// Client replica client
type Client struct {
	end        chan struct{}
	requests   chan *Message
	send       chan *Message
	responses  chan *Message
	messages   [queueLength]*Message
	writeBuffs [queueLength][]byte
	SeqChan    chan uint32
	wires      []*CWire
	peerAddr   string
}

// NewClient replica client
func NewClient(conns []net.Conn, sharedTimeouts types.SharedTimeouts) *Client {
	var wires []*CWire
	for _, conn := range conns {
		wires = append(wires, NewCWire(conn))
	}

	c := &Client{
		wires:     wires,
		peerAddr:  conns[0].RemoteAddr().String(),
		end:       make(chan struct{}, 4096),
		requests:  make(chan *Message, 4096),
		send:      make(chan *Message, 4096),
		responses: make(chan *Message, 4096),
		messages:  [queueLength]*Message{},
		SeqChan:   make(chan uint32, queueLength),
	}
	for i := 0; i < queueLength; i++ {
		c.messages[i] = &Message{
			Complete: make(chan struct{}),
			WireHeader: WireHeader{
				MagicVersion: MagicVersion,
				Seq:          uint32(i),
				Type:         0,
				Offset:       0,
				Size:         0,
				DataLen:      0,
			},
			Data:         make([]byte, Blocks*1024),
			transportErr: nil,
		}
	}
	for i := uint32(0); i < queueLength; i++ {
		c.SeqChan <- i
	}
	c.write()
	c.read()
	return c
}

// TargetID operation target ID
func (c *Client) TargetID() string {
	return c.peerAddr
}

// WriteAt replica client
func (c *Client) WriteAt(buf []byte, offset int64) (int, error) {
	return c.operation(TypeWrite, buf, uint32(len(buf)), offset)
}

// UnmapAt replica client
func (c *Client) UnmapAt(length uint32, offset int64) (int, error) {
	return c.operation(TypeUnmap, nil, length, offset)
}

// SetError replica client transport error
func (c *Client) SetError(err error) {
	c.responses <- &Message{
		transportErr: err,
	}
}

// ReadAt replica client
func (c *Client) ReadAt(buf []byte, offset int64) (int, error) {
	return c.operation(TypeRead, buf, uint32(len(buf)), offset)
}

// Ping replica client
func (c *Client) Ping() error {
	_, err := c.operation(TypePing, nil, 0, 0)
	return err
}

func (c *Client) operation(op uint32, buf []byte, length uint32, offset int64) (int, error) {
	seq := <-c.SeqChan
	msg := c.messages[seq]
	if op == TypeWrite {
		c.writeBuffs[seq] = buf
		//msg.Data = buf
	}
	msg.Type = op
	msg.Offset = offset
	msg.Size = length

	c.send <- msg

	<-msg.Complete
	// Only copy the message if a read is requested
	if op == TypeRead && (msg.Type == TypeResponse || msg.Type == TypeEOF) {
		copy(buf, msg.Data)
	}
	if msg.Type == TypeError {
		return 0, errors.New(string(msg.Data))
	}
	if msg.Type == TypeEOF {
		return int(msg.Size), io.EOF
	}

	c.SeqChan <- msg.Seq

	return int(msg.Size), nil
	//return len(buf), nil
}

// Close replica client
func (c *Client) Close() {
	for _, wire := range c.wires {
		err := wire.CClose()
		if err != nil {
			return
		}
	}
	//reply error to all pending requests
	for i := 0; i < queueLength; i++ {
		go func() {
			msg := c.messages[i]
			select {
			case <-msg.Complete:
				// already completed
			default:
				c.replyError(msg, errors.New("connection closed"))
				c.SeqChan <- msg.Seq
			}
		}()

	}

	c.end <- struct{}{}
}

func (c *Client) replyError(req *Message, err error) {
	req.Type = TypeError
	req.Data = []byte(err.Error())
	req.Complete <- struct{}{}
}

//func (c *Client) handleRequest(req *Message) {
//	req.MagicVersion = MagicVersion
//
//	req.Seq = <-c.SeqChan
//
//	c.messages[req.Seq] = req
//	c.send <- req
//}

func (c *Client) handleResponse(resp *Message) {

	resp.Complete <- struct{}{}

}

func (c *Client) write() {
	for _, wire := range c.wires {
		go func(w *CWire) {
			batch := make([]*Message, 0, 64)
			for {
				select {
				case msg := <-c.send:
					batch = append(batch, msg)

					// Smart Batching: try to consume more messages if available
					for i := 0; i < 63; i++ { // max 64 total
						select {
						case nextMsg := <-c.send:
							batch = append(batch, nextMsg)
						default:
							goto SEND
						}
					}
				SEND:
					if err := w.WriteBatch(batch); err != nil {
						c.responses <- &Message{
							transportErr: err,
						}
					}
					// Clear batch for reuse to avoid allocation if we used a pool,
					// but here we just slice it to 0 or reallocate?
					// slice to 0 is better.
					batch = batch[:0]
				}
			}
		}(wire)
	}
}

func (c *Client) read() {
	for _, wire := range c.wires {
		go func(w *CWire) {
			for {
				msg, err := w.CRead(c)
				if err != nil {
					logrus.WithError(err).Errorf("Error reading from wire %v", c.peerAddr)
					c.responses <- &Message{
						transportErr: err,
					}
					break
				}
				c.handleResponse(msg)
			}
		}(wire)
	}
}
