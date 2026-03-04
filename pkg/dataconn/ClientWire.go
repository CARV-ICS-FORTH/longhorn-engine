package dataconn

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"unsafe"
)

type CWire struct {
	conn        net.Conn
	writer      *bufio.Writer
	reader      io.Reader
	writeHeader WireHeader
	readHeader  WireHeader
}

func NewCWire(conn net.Conn) *CWire {
	return &CWire{
		conn:   conn,
		writer: bufio.NewWriterSize(conn, writeBufferSize),
		reader: bufio.NewReaderSize(conn, readBufferSize),
		// writeHeader: WireHeader{}, // Native zero-values
		// readHeader:  WireHeader{}, // Native zero-values
	}
}

func (w *CWire) CWrite(msg *Message, c *Client) error {
	w.writeHeader = WireHeader{
		Offset:       msg.Offset,
		Seq:          msg.Seq,
		Type:         msg.Type,
		Size:         msg.Size,
		DataLength:   0,
		MagicVersion: msg.MagicVersion,
	}
	if msg.Type == TypeWrite {
		w.writeHeader.DataLength = uint32(len(c.writeBuffs[msg.Seq]))
	}

	headerSlice := unsafe.Slice((*byte)(unsafe.Pointer(&w.writeHeader)), unsafe.Sizeof(WireHeader{}))

	if msg.Type == TypeWrite {
		if _, err := w.writer.Write(headerSlice); err != nil {
			return err
		}

		if _, err := w.writer.Write(c.writeBuffs[msg.Seq]); err != nil {
			return err
		}
	} else {
		if _, err := w.writer.Write(headerSlice); err != nil {
			return err
		}

	}

	//fmt.Println("Write Request : Seq : ", msg.Seq, " Type : ", msg.Type, " Offset : ", msg.Offset, " Size : ", msg.Size)
	return w.writer.Flush()
}

func (w *CWire) CRead(c *Client) (*Message, error) {
	headerSlice := unsafe.Slice((*byte)(unsafe.Pointer(&w.readHeader)), unsafe.Sizeof(WireHeader{}))

	if _, err := io.ReadFull(w.reader, headerSlice); err != nil {
		return nil, err
	}

	if w.readHeader.MagicVersion != MagicVersion {
		return nil, fmt.Errorf("wrong API version received: 0x%x", w.readHeader.MagicVersion)
	}

	msg := c.messages[w.readHeader.Seq]

	msg.Type = w.readHeader.Type
	msg.Offset = w.readHeader.Offset
	msg.Size = w.readHeader.Size

	length := w.readHeader.DataLength
	if length > 0 {
		msg.Data = msg.Data[:length]
		if _, err := io.ReadFull(w.reader, msg.Data); err != nil {
			return nil, err
		}
	}

	//fmt.Println("Read Reply : Seq : ", msg.Seq, " Type : ", msg.Type, " Offset : ", msg.Offset, " Size : ", msg.Size)

	return msg, nil
}

func (w *CWire) CClose() error {
	return w.conn.Close()
}

// Legacy sizing mechanism replaced!
// func CgetRequestHeaderSize() int {
// 	return int(unsafe.Sizeof(WireHeader{}))
// }
