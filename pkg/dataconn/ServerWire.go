package dataconn

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"unsafe"
)

type SWire struct {
	conn        net.Conn
	writer      *bufio.Writer
	reader      io.Reader
	writeHeader []byte
	readHeader  []byte
}

func NewSWire(conn net.Conn) *SWire {
	return &SWire{
		conn:        conn,
		writer:      bufio.NewWriterSize(conn, writeBufferSize),
		reader:      bufio.NewReaderSize(conn, readBufferSize),
		writeHeader: make([]byte, getRequestHeaderSize()),
		readHeader:  make([]byte, getRequestHeaderSize()),
	}
}

func (w *SWire) SWrite(msg *Message) error {
	offset := 0

	binary.LittleEndian.PutUint16(w.writeHeader[offset:], msg.MagicVersion)
	offset += int(unsafe.Sizeof(msg.MagicVersion))

	binary.LittleEndian.PutUint32(w.writeHeader[offset:], msg.Seq)
	offset += int(unsafe.Sizeof(msg.Seq))

	binary.LittleEndian.PutUint32(w.writeHeader[offset:], msg.Type)
	offset += int(unsafe.Sizeof(msg.Type))

	binary.LittleEndian.PutUint64(w.writeHeader[offset:], uint64(msg.Offset))
	offset += int(unsafe.Sizeof(msg.Offset))

	binary.LittleEndian.PutUint32(w.writeHeader[offset:], msg.Size)
	offset += int(unsafe.Sizeof(msg.Size))

	binary.LittleEndian.PutUint32(w.writeHeader[offset:], uint32(len(msg.Data)))

	if msg.Type == TypeWrite || (msg.Type == TypeResponse && msg.Data != nil) {
		binary.LittleEndian.PutUint32(w.writeHeader[offset:], uint32(len(msg.Data)))
		if _, err := w.writer.Write(w.writeHeader); err != nil {
			return err
		}

		if _, err := w.writer.Write(msg.Data); err != nil {
			return err
		}
	} else {
		binary.LittleEndian.PutUint32(w.writeHeader[offset:], uint32(0))
		if _, err := w.writer.Write(w.writeHeader); err != nil {
			return err
		}

	}

	return w.writer.Flush()
}

func (w *SWire) SRead(s *Server) (*Message, error) {
	msg := <-s.messages

	offset := 0

	if _, err := io.ReadFull(w.reader, w.readHeader); err != nil {
		return nil, err
	}

	msg.MagicVersion = binary.LittleEndian.Uint16(w.readHeader[offset:])
	if msg.MagicVersion != MagicVersion {
		return nil, fmt.Errorf("wrong API version received: 0x%x", msg.MagicVersion)
	}
	offset += int(unsafe.Sizeof(msg.MagicVersion))

	msg.Seq = binary.LittleEndian.Uint32(w.readHeader[offset:])
	offset += int(unsafe.Sizeof(msg.Seq))

	msg.Type = binary.LittleEndian.Uint32(w.readHeader[offset:])
	offset += int(unsafe.Sizeof(msg.Type))

	msg.Offset = int64(binary.LittleEndian.Uint64(w.readHeader[offset:]))
	offset += int(unsafe.Sizeof(msg.Offset))

	msg.Size = binary.LittleEndian.Uint32(w.readHeader[offset:])
	offset += int(unsafe.Sizeof(msg.Size))

	length := binary.LittleEndian.Uint32(w.readHeader[offset:])
	if length > 0 {
		if msg.Data == nil || len(msg.Data) < int(length) {
			msg.Data = make([]byte, length)
		} else {
			msg.Data = msg.Data[:length]
		}
		if _, err := io.ReadFull(w.reader, msg.Data); err != nil {
			return nil, err
		}
	}

	return msg, nil
}

func (w *SWire) SClose() error {
	return w.conn.Close()
}

func SgetRequestHeaderSize() int {
	var msg Message

	return int(unsafe.Sizeof(msg.MagicVersion)) +
		int(unsafe.Sizeof(msg.Seq)) +
		int(unsafe.Sizeof(msg.Type)) +
		int(unsafe.Sizeof(msg.Offset)) +
		int(unsafe.Sizeof(msg.Size)) +
		4 // length of uint32 (data type of the msg.data length)
}
