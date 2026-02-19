package dataconn

import (
	"unsafe"

	journal "github.com/longhorn/sparse-tools/stats"
)

const (
	TypeRead = iota
	TypeWrite
	TypeResponse
	TypeError
	TypeEOF
	TypeClose
	TypePing
	TypeUnmap

	messageSize     = (32 + 32 + 32 + 64) / 8 //TODO: unused?
	readBufferSize  = 8096
	writeBufferSize = 8096
)

var HeaderSize = int(unsafe.Sizeof(WireHeader{}))

const (
	MagicVersion = uint16(0x1b01) // LongHorn01
)

type WireHeader struct {
	Offset       int64  // 8 bytes
	Seq          uint32 // 4 bytes
	Type         uint32 // 4 bytes
	Size         uint32 // 4 bytes
	DataLen      uint32 // 4 bytes
	MagicVersion uint16 // 2 bytes
	_            uint16 // Padding
}

type Message struct {
	Complete chan struct{}

	WireHeader
	Data         []byte
	transportErr error

	ID journal.OpID
}
