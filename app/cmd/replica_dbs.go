package cmd

import (
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"syscall"

	"github.com/docker/go-units"
	"github.com/sirupsen/logrus"
	"github.com/urfave/cli"
	"golang.org/x/net/context"

	"github.com/Kampadais/dbs"
	replica "github.com/longhorn/longhorn-engine/pkg/replica_dbs"
	replicarpc "github.com/longhorn/longhorn-engine/pkg/replica_dbs/rpc"
	"github.com/longhorn/longhorn-engine/pkg/types"
	"github.com/longhorn/longhorn-engine/pkg/util"
)

func ReplicaDBSCmd() cli.Command {
	return cli.Command{
		Name:      "replica-dbs",
		UsageText: "longhorn replica-dbs DEVICE",
		Flags: []cli.Flag{
			cli.StringFlag{
				Name:  "listen",
				Value: "localhost:9502",
			},
			cli.StringFlag{
				Name:  "size",
				Usage: "Volume size in bytes or human readable 42kb, 42mb, 42gb",
			},
			cli.StringFlag{
				Name:  "data-server-protocol",
				Value: "tcp",
				Usage: "Specify the data-server protocol. Available options are \"tcp\" and \"unix\"",
			},
			cli.StringFlag{
				Name:  "replica-instance-name",
				Value: "",
				Usage: "Name of the replica instance (for validation purposes)",
			},
			cli.BoolFlag{
				Name:   "initDevice",
				Hidden: false,
				Usage:  "To initialize the device (all data will be lost)",
			},
		},
		Action: func(c *cli.Context) {
			if err := startReplicaDBS(c); err != nil {
				logrus.WithError(err).Fatalf("Error running start replica command")
			}
		},
	}
}

func startReplicaDBS(c *cli.Context) error {
	if c.NArg() != 1 {
		return errors.New("device name is required")
	}

	dir := c.Args()[0]

	//Create folder
	if err := os.MkdirAll(dir, 0755); err != nil {
		fmt.Println("Error creating replica dbs directory:", err)
		return err
	}
	//Create replica file
	path := filepath.Join(dir, "replica_dbs.img")

	size := c.String("size")
	if size == "" {
		return errors.New("size is required")
	}
	volumeSize, err := units.RAMInBytes(size)
	cmd := exec.Command("truncate", "-s "+strconv.FormatInt(volumeSize, 10), path)
	cmd.Stdout = nil
	cmd.Stderr = nil

	fmt.Println("Creating file... , running command:", cmd)
	if err := cmd.Run(); err != nil {
		panic(err)
	}

	fmt.Println("File created successfully at", path)
	if err := dbs.InitDevice(path); err != nil {
		return fmt.Errorf("failed to init device: %w", err)
	}

	volumeName := c.GlobalString("volume-name")
	if volumeName == "" {

		//return errors.New("volume name is required")
		volumeName = "test"
	}
	s := replica.NewServer(path, volumeName)

	if err := s.Create(volumeSize); err != nil {
		return err
	}

	address := c.String("listen")
	replicaInstanceName := c.String("replica-instance-name")
	dataServerProtocol := c.String("data-server-protocol")

	controlAddress, dataAddress, syncAddress, syncPort, err :=
		util.GetAddresses(volumeName, address, types.DataServerProtocol(dataServerProtocol))
	if err != nil {
		return err
	}

	resp := make(chan error)

	go func() {
		listen, err := net.Listen("tcp", controlAddress)
		if err != nil {
			logrus.WithError(err).Warnf("Failed to listen %v", controlAddress)
			resp <- err
			return
		}

		server := replicarpc.NewReplicaServer(volumeName, replicaInstanceName, s)

		logrus.Infof("Listening on gRPC Replica server %s", controlAddress)
		err = server.Serve(listen)
		logrus.WithError(err).Warnf("gRPC Replica server at %v is down", controlAddress)
		resp <- err
	}()

	go func() {
		rpcServer := replicarpc.NewDataServer(types.DataServerProtocol(dataServerProtocol), dataAddress, s)
		logrus.Infof("Listening on data server %s", dataAddress)
		err := rpcServer.ListenAndServe()
		logrus.WithError(err).Warnf("Replica rest server at %v is down", dataAddress)
		resp <- err
	}()
	_, cancel := context.WithCancel(context.Background())
	defer func() {
		if err != nil {
			cancel()
		}
	}()
	if c.Bool("sync-agent") {
		exe, err := exec.LookPath(os.Args[0])
		if err != nil {
			return err
		}

		exe, err = filepath.Abs(exe)
		if err != nil {
			return err
		}

		go func() {
			defer cancel()

			cmd := exec.Command(exe, "--volume-name", volumeName, "sync-agent", "--listen", syncAddress,
				"--replica", controlAddress,
				"--listen-port-range",
				fmt.Sprintf("%v-%v", syncPort+1, syncPort+c.Int("sync-agent-port-count")),
				"--replica-instance-name", replicaInstanceName)
			cmd.SysProcAttr = &syscall.SysProcAttr{
				Pdeathsig: syscall.SIGKILL,
			}
			cmd.Dir = dir
			cmd.Stdout = os.Stdout
			cmd.Stderr = os.Stderr
			logrus.Infof("Listening on sync agent server %s", syncAddress)
			err := cmd.Run()
			logrus.WithError(err).Warnf("Replica sync agent at %v is down", syncAddress)
			resp <- err
		}()
	}

	// empty shutdown hook for signal message
	addShutdown(func() (err error) { return nil })

	return <-resp
}
