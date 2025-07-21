package dataconn

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"unsafe"
)

type CWire struct {
	conn        net.Conn
	writer      *bufio.Writer
	reader      io.Reader
	writeHeader []byte
	readHeader  []byte
}

func NewCWire(conn net.Conn) *CWire {
	return &CWire{
		conn:        conn,
		writer:      bufio.NewWriterSize(conn, writeBufferSize),
		reader:      bufio.NewReaderSize(conn, readBufferSize),
		writeHeader: make([]byte, getRequestHeaderSize()),
		readHeader:  make([]byte, getRequestHeaderSize()),
	}
}

func (w *CWire) CWrite(msg *Message, c *Client) error {
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

	if msg.Type == TypeWrite {
		binary.LittleEndian.PutUint32(w.writeHeader[offset:], uint32(len(c.writeBuffs[msg.Seq])))
		if _, err := w.writer.Write(w.writeHeader); err != nil {
			return err
		}

		if _, err := w.writer.Write(c.writeBuffs[msg.Seq]); err != nil {
			return err
		}
	} else {
		binary.LittleEndian.PutUint32(w.writeHeader[offset:], uint32(0))
		if _, err := w.writer.Write(w.writeHeader); err != nil {
			return err
		}

	}

	//fmt.Println("Write Request : Seq : ", msg.Seq, " Type : ", msg.Type, " Offset : ", msg.Offset, " Size : ", msg.Size)
	return w.writer.Flush()
}

func (w *CWire) CRead(c *Client) (*Message, error) {

	offset := 0
	if _, err := io.ReadFull(w.reader, w.readHeader); err != nil {
		return nil, err
	}

	Mg := binary.LittleEndian.Uint16(w.readHeader[offset:])
	if Mg != MagicVersion {
		return nil, fmt.Errorf("wrong API version received: 0x%x", Mg)
	}
	offset += int(unsafe.Sizeof(Mg))

	Seq := binary.LittleEndian.Uint32(w.readHeader[offset:])
	offset += int(unsafe.Sizeof(Seq))

	msg := c.messages[Seq]

	msg.Type = binary.LittleEndian.Uint32(w.readHeader[offset:])
	offset += int(unsafe.Sizeof(msg.Type))

	msg.Offset = int64(binary.LittleEndian.Uint64(w.readHeader[offset:]))
	offset += int(unsafe.Sizeof(msg.Offset))

	msg.Size = binary.LittleEndian.Uint32(w.readHeader[offset:])
	offset += int(unsafe.Sizeof(msg.Size))

	length := binary.LittleEndian.Uint32(w.readHeader[offset:])
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

func CgetRequestHeaderSize() int {
	var msg Message

	return int(unsafe.Sizeof(msg.MagicVersion)) +
		int(unsafe.Sizeof(msg.Seq)) +
		int(unsafe.Sizeof(msg.Type)) +
		int(unsafe.Sizeof(msg.Offset)) +
		int(unsafe.Sizeof(msg.Size)) +
		4 // length of uint32 (data type of the msg.data length)
}
