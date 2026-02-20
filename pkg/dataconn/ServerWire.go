package dataconn

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"unsafe"
)

type SWire struct {
	conn   net.Conn
	reader io.Reader
}

func NewSWire(conn net.Conn) *SWire {
	return &SWire{
		conn:   conn,
		reader: bufio.NewReaderSize(conn, readBufferSize),
	}
}

func (w *SWire) WriteBatch(messages []*Message) error {
	buffers := make(net.Buffers, 0, len(messages)*2)
	for _, msg := range messages {
		if msg.Type == TypeRead {
			msg.DataLen = uint32(len(msg.Data))

			msg.Type = TypeResponse
			headerBytes := unsafe.Slice((*byte)(unsafe.Pointer(&msg.WireHeader)), HeaderSize)
			buffers = append(buffers, headerBytes)
			buffers = append(buffers, msg.Data)
		} else {
			msg.Type = TypeResponse
			msg.DataLen = 0
			headerBytes := unsafe.Slice((*byte)(unsafe.Pointer(&msg.WireHeader)), HeaderSize)
			buffers = append(buffers, headerBytes)
		}
	}

	_, err := buffers.WriteTo(w.conn)
	return err
}

func (w *SWire) SWrite(msg *Message) error {
	return w.WriteBatch([]*Message{msg})
}

func (w *SWire) Flush() error {
	return nil
}

func (w *SWire) SRead(s *Server) (*Message, error) {

	var header WireHeader
	headerBytes := unsafe.Slice((*byte)(unsafe.Pointer(&header)), HeaderSize)

	if _, err := io.ReadFull(w.reader, headerBytes); err != nil {
		return nil, err
	}

	if header.MagicVersion != MagicVersion {
		return nil, fmt.Errorf("wrong API version received: 0x%x", header.MagicVersion)
	}
	msg := ServerMessages[header.Seq]
	msg.WireHeader = header

	if header.DataLen > 0 {
		if int(header.DataLen) > cap(msg.Data) {
			fmt.Println("Allocating new buffer for data of length", header.DataLen)
			msg.Data = make([]byte, header.DataLen)
		}
		msg.Data = msg.Data[:header.DataLen]
		if _, err := io.ReadFull(w.reader, msg.Data); err != nil {
			return nil, err
		}
	} else {
		msg.Data = msg.Data[:0]
	}
	return msg, nil
}

func (w *SWire) SClose() error {
	return w.conn.Close()
}
