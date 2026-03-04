package dataconn

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"unsafe"
)

type Wire struct {
	conn        net.Conn
	writer      *bufio.Writer
	reader      io.Reader
	writeHeader WireHeader
	readHeader  WireHeader
}

func NewWire(conn net.Conn) *Wire {
	return &Wire{
		conn:   conn,
		writer: bufio.NewWriterSize(conn, writeBufferSize),
		reader: bufio.NewReaderSize(conn, readBufferSize),
		// writeHeader: WireHeader{}, // Native zero-values
		// readHeader:  WireHeader{}, // Native zero-values
	}
}

func (w *Wire) Write(msg *Message) error {
	w.writeHeader = WireHeader{
		Offset:       msg.Offset,
		Seq:          msg.Seq,
		Type:         msg.Type,
		Size:         msg.Size,
		DataLength:   0,
		MagicVersion: msg.MagicVersion,
	}

	if msg.Type == TypeWrite || (msg.Type == TypeResponse && msg.Data != nil) {
		w.writeHeader.DataLength = uint32(len(msg.Data))
	}

	headerSlice := unsafe.Slice((*byte)(unsafe.Pointer(&w.writeHeader)), unsafe.Sizeof(WireHeader{}))

	if msg.Type == TypeWrite || (msg.Type == TypeResponse && msg.Data != nil) {
		if _, err := w.writer.Write(headerSlice); err != nil {
			return err
		}

		if len(msg.Data) > 0 {
			if _, err := w.writer.Write(msg.Data); err != nil {
				return err
			}
		}
	} else {
		if _, err := w.writer.Write(headerSlice); err != nil {
			return err
		}

	}

	return w.writer.Flush()
}

func (w *Wire) Read() (*Message, error) {
	var msg Message

	headerSlice := unsafe.Slice((*byte)(unsafe.Pointer(&w.readHeader)), unsafe.Sizeof(WireHeader{}))

	if _, err := io.ReadFull(w.reader, headerSlice); err != nil {
		return nil, err
	}

	if w.readHeader.MagicVersion != MagicVersion {
		return nil, fmt.Errorf("wrong API version received: 0x%x", w.readHeader.MagicVersion)
	}

	msg.MagicVersion = w.readHeader.MagicVersion
	msg.Seq = w.readHeader.Seq
	msg.Type = w.readHeader.Type
	msg.Offset = w.readHeader.Offset
	msg.Size = w.readHeader.Size
	length := w.readHeader.DataLength

	if length > 0 {
		msg.Data = make([]byte, length)
		if _, err := io.ReadFull(w.reader, msg.Data); err != nil {
			return nil, err
		}
	}

	return &msg, nil
}

func (w *Wire) Close() error {
	return w.conn.Close()
}

// Legacy sizing mechanism replaced!
// func getRequestHeaderSize() int {
// 	return int(unsafe.Sizeof(WireHeader{}))
// }
