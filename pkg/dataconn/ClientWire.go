package dataconn

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"unsafe"
)

type CWire struct {
	conn   net.Conn
	reader io.Reader
}

func NewCWire(conn net.Conn) *CWire {
	return &CWire{
		conn:   conn,
		reader: bufio.NewReaderSize(conn, readBufferSize),
	}
}

func (w *CWire) WriteBatch(messages []*Message, c *Client) error {
	buffers := make(net.Buffers, 0, len(messages)*2)
	for _, msg := range messages {
		// Update WireHeader fields that depend on Data
		// msg.Size is redundant with DataLen? The original code had msg.Size as "requested size" for Read,
		// and explicit data length write for Write/Response.
		// WireHeader has Size and DataLen.
		// Size in WireHeader corresponds to the 'Size' field in original Message (uint32).
		// DataLen is the length of attached data.

		if msg.Type == TypeWrite {
			msg.DataLen = uint32(len(c.writeBuffs[msg.Seq]))

			// Unsafe cast WireHeader to []byte
			headerBytes := unsafe.Slice((*byte)(unsafe.Pointer(&msg.WireHeader)), HeaderSize)
			buffers = append(buffers, headerBytes)
			buffers = append(buffers, c.writeBuffs[msg.Seq])
		} else {
			msg.DataLen = 0
			headerBytes := unsafe.Slice((*byte)(unsafe.Pointer(&msg.WireHeader)), HeaderSize)
			buffers = append(buffers, headerBytes)
		}
	}

	_, err := buffers.WriteTo(w.conn)
	return err
}

func (w *CWire) CWrite(msg *Message, c *Client) error {
	return w.WriteBatch([]*Message{msg}, c)
}

func (w *CWire) Flush() error {
	return nil
}

func (w *CWire) CRead(c *Client) (*Message, error) {
	// Read directly into a temporary WireHeader or the target message's header if we know the Seq beforehand.
	// But we don't know Seq until we read the header.
	// So we read into w.readHeader (which is []byte) and cast it to WireHeader?
	// Or even better: read directly into a stack-allocated WireHeader struct.

	var header WireHeader
	headerBytes := unsafe.Slice((*byte)(unsafe.Pointer(&header)), HeaderSize)

	if _, err := io.ReadFull(w.reader, headerBytes); err != nil {
		return nil, err
	}

	if header.MagicVersion != MagicVersion {
		return nil, fmt.Errorf("wrong API version received: 0x%x", header.MagicVersion)
	}

	msg := c.messages[header.Seq]
	msg.WireHeader = header // Copy header data to message

	if header.DataLen > 0 {
		if int(header.DataLen) > cap(msg.Data) {
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

func (w *CWire) CClose() error {
	return w.conn.Close()
}
