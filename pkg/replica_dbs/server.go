package replica_dbs

import (
	"fmt"
	"github.com/Kampadais/dbs"
	"strconv"
	"sync"

	"github.com/sirupsen/logrus"

	"github.com/longhorn/longhorn-engine/pkg/replica"
	"github.com/longhorn/longhorn-engine/pkg/types"
)

type Server struct {
	sync.RWMutex
	device     string
	volumeName string
	ctx        *dbs.VolumeContext
	dirty      bool
	rebuilding bool
}

type Info struct {
	Size       int64
	Head       string
	Dirty      bool
	Rebuilding bool
	Parent     string
}

func NewServer(device string, volumeName string) *Server {
	return &Server{
		device:     device,
		volumeName: volumeName,
	}
}

func (s *Server) Create(size int64) error {
	s.Lock()
	defer s.Unlock()

	state, _ := s.Status()
	if state != types.ReplicaStateInitial {
		return nil
	}

	logrus.Infof("Creating replica %s in %s, size %d", s.volumeName, s.device, size)
	if size%dbs.EXTENT_SIZE != 0 {
		return fmt.Errorf("size %d not a multiple of extent size %d", size, dbs.EXTENT_SIZE)
	}
	return dbs.CreateVolume(s.device, s.volumeName, uint64(size))
}

func (s *Server) Open() error {
	s.Lock()
	defer s.Unlock()

	if s.ctx != nil {
		return fmt.Errorf("replica is already open")
	}

	logrus.Infof("Opening replica %s in %s", s.volumeName, s.device)
	ctx, err := dbs.OpenVolume(s.device, s.volumeName)
	if err != nil {
		return err
	}
	s.ctx = ctx
	s.dirty = true
	// XXX Load rebuilding state
	s.rebuilding = false
	return nil
}

func (s *Server) Reload() error {
	s.Lock()
	defer s.Unlock()

	if s.ctx == nil {
		return nil
	}

	logrus.Info("Reloading replica")
	newCtx, err := dbs.OpenVolume(s.device, s.volumeName)
	if err != nil {
		return err
	}

	oldCtx := s.ctx
	s.ctx = newCtx
	// XXX Load rebuilding state
	oldCtx.CloseVolume()
	return nil
}

func (s *Server) Status() (types.ReplicaState, Info) {
	vi, err := dbs.GetVolumeInfo(s.device)
	if err != nil {
		logrus.WithError(err).Errorf("Failed to get volume metadata from device %s", s.device)
		return types.ReplicaStateError, Info{}
	}
	var volume *dbs.VolumeInfo = nil
	for i := range vi {
		if vi[i].VolumeName == s.volumeName {
			volume = &vi[i]
			break
		}
	}
	if volume == nil {
		return types.ReplicaStateInitial, Info{}
	}

	si, err := dbs.GetSnapshotInfo(s.device, s.volumeName)
	if err != nil {
		logrus.WithError(err).Errorf("Failed to get snapshot metadata from device %s", s.device)
		return types.ReplicaStateError, Info{}
	}
	var snapshot *dbs.SnapshotInfo = nil
	for i := range si {
		if si[i].SnapshotId == volume.SnapshotId {
			snapshot = &si[i]
			break
		}
	}
	if snapshot == nil {
		logrus.Errorf("Failed to find snapshot metadata for volume %s", s.volumeName)
		return types.ReplicaStateError, Info{}
	}

	parentSnapshot := ""
	if snapshot.ParentSnapshotId > 0 {
		parentSnapshot = strconv.Itoa(int(snapshot.ParentSnapshotId))
	}
	info := Info{
		Size:       int64(volume.VolumeSize),
		Head:       strconv.Itoa(int(volume.SnapshotId)),
		Dirty:      s.dirty,
		Rebuilding: s.rebuilding,
		Parent:     parentSnapshot,
	}
	if s.ctx == nil {
		return types.ReplicaStateClosed, info
	}
	switch {
	case info.Rebuilding:
		return types.ReplicaStateRebuilding, info
	case info.Dirty:
		return types.ReplicaStateDirty, info
	default:
		return types.ReplicaStateOpen, info
	}
}

func (s *Server) SetRebuilding(rebuilding bool) error {
	s.Lock()
	defer s.Unlock()

	state, _ := s.Status()
	// Must be Open/Dirty to set true or must be Rebuilding to set false
	if (rebuilding && state != types.ReplicaStateOpen && state != types.ReplicaStateDirty) ||
		(!rebuilding && state != types.ReplicaStateRebuilding) {
		return fmt.Errorf("cannot set rebuilding=%v from state %s", rebuilding, state)
	}

	s.rebuilding = rebuilding
	return nil
}

func (s *Server) Revert(name, created string) error {
	logrus.Infof("Replica server does not support Revert")
	return fmt.Errorf("cannot revert to snapshot [%s] on replica at %s", name, created)
}

func (s *Server) Snapshot(name string, userCreated bool, createdTime string, labels map[string]string) error {
	logrus.Infof("Replica server does not support Snapshot")
	return fmt.Errorf("cannot create snapshot [%s] volume, user created %v, created time %v, labels %v",
		name, userCreated, createdTime, labels)
}

func (s *Server) SetUnmapMarkDiskChainRemoved(enabled bool) {
	logrus.Infof("Replica server does not support SetUnmapMarkDiskChainRemoved")
	return
}

func (s *Server) Expand(size int64) error {
	logrus.Infof("Replica server does not support Expand")
	return fmt.Errorf("cannot expand replica to size %v", size)
}

func (s *Server) RemoveDiffDisk(name string, force bool) error {
	logrus.Infof("Replica server does not support RemoveDiffDisk")
	return fmt.Errorf("cannot remove disk %s, force %v", name, force)
}

func (s *Server) ReplaceDisk(target, source string) error {
	logrus.Infof("Replica server does not support ReplaceDisk")
	return fmt.Errorf("cannot replace disk %v with %v", source, target)
}

func (s *Server) MarkDiskAsRemoved(name string) error {
	logrus.Infof("Replica server does not support MarkDiskAsRemoved")
	return fmt.Errorf("cannot mark disk %v as removed", name)
}

func (s *Server) PrepareRemoveDisk(name string) ([]replica.PrepareRemoveAction, error) {
	logrus.Infof("Replica server does not support PrepareRemoveDisk")
	return nil, fmt.Errorf("cannot remove disk %s", name)
}

func (s *Server) Delete() error {
	s.Lock()
	defer s.Unlock()

	logrus.Info("Deleting replica")
	if s.ctx != nil {
		s.ctx.CloseVolume()
		s.ctx = nil
	}
	return dbs.DeleteVolume(s.device, s.volumeName)
}

func (s *Server) Close() error {
	s.Lock()
	defer s.Unlock()

	logrus.Info("Closing replica")
	if s.ctx != nil {
		s.ctx.CloseVolume()
		s.ctx = nil
	}
	return nil
}

func (s *Server) WriteAt(buf []byte, offset int64) (int, error) {

	if s.ctx == nil {
		return 0, fmt.Errorf("replica no longer exists")
	}

	// Try with a read lock and upgrade to a write lock if necessary
	s.RLock()
	err := s.ctx.WriteAt(buf, uint64(offset))
	s.RUnlock()

	return len(buf), err
}

func (s *Server) ReadAt(buf []byte, offset int64) (int, error) {
	if s.ctx == nil {
		return 0, fmt.Errorf("replica no longer exists")
	}

	s.RLock()
	err := s.ctx.ReadAt(buf, uint64(offset))
	s.RUnlock()
	return len(buf), err
}

func (s *Server) UnmapAt(length uint32, off int64) (int, error) {
	s.Lock()
	defer s.Unlock()

	if s.ctx == nil {
		return 0, fmt.Errorf("replica no longer exists")
	}
	err := s.ctx.UnmapAt(uint64(length), uint64(off))
	return int(length), err
}

func (s *Server) SetRevisionCounter(counter int64) error {
	logrus.Infof("Replica server does not support SetRevisionCounter")
	return fmt.Errorf("cannot set revision counter to %v", counter)
}

func (s *Server) PingResponse() error {
	// The Error variable is not used, so we are ok as long as we have a context
	if s.ctx == nil {
		return fmt.Errorf("ping failure: replica state %v", types.ReplicaStateClosed)
	}
	return nil
}
