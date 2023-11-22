// Code generated by "stringer -type status"; DO NOT EDIT.

package kmod

import "strconv"

func _() {
	// An "invalid array index" compiler error signifies that the constant values have changed.
	// Re-run the stringer command to generate them again.
	var x [1]struct{}
	_ = x[unknown-0]
	_ = x[unloaded-1]
	_ = x[unloading-2]
	_ = x[loading-3]
	_ = x[live-4]
	_ = x[inuse-5]
}

const _status_name = "unknownunloadedunloadingloadingliveinuse"

var _status_index = [...]uint8{0, 7, 15, 24, 31, 35, 40}

func (i status) String() string {
	if i < 0 || i >= status(len(_status_index)-1) {
		return "status(" + strconv.FormatInt(int64(i), 10) + ")"
	}
	return _status_name[_status_index[i]:_status_index[i+1]]
}
