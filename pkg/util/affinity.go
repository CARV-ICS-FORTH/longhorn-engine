package util

import (
	"runtime"

	"golang.org/x/sys/unix"
)

// PinToCore locks the current goroutine to an OS thread and pins it to a specific CPU.
func PinToCore(cpuID int) error {
	runtime.LockOSThread()
	var cpuSet unix.CPUSet
	cpuSet.Zero()
	cpuSet.Set(cpuID)
	return unix.SchedSetaffinity(0, &cpuSet)
}
