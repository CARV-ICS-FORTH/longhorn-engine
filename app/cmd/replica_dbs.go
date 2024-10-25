package cmd

import (
	"errors"
	"fmt"
	"net"

	"github.com/docker/go-units"
	"github.com/sirupsen/logrus"
	"github.com/urfave/cli"

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

	device := c.Args()[0]

	initDevice := c.Bool("initDevice")
	if initDevice == true {
		if err := dbs.InitDevice(device); err != nil {
			return fmt.Errorf("failed to init device: %w", err)
		}
	}

	volumeName := c.GlobalString("volume-name")
	if volumeName == "" {

		//return errors.New("volume name is required")
		volumeName = "test"
	}
	s := replica.NewServer(device, volumeName)

	size := c.String("size")
	if size != "" {
		size, err := units.RAMInBytes(size)
		if err != nil {
			return err
		}

		if err := s.Create(size); err != nil {
			return err
		}
	}

	address := c.String("listen")
	replicaInstanceName := c.String("replica-instance-name")
	dataServerProtocol := c.String("data-server-protocol")

	controlAddress, dataAddress, _, _, err :=
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

	// empty shutdown hook for signal message
	addShutdown(func() (err error) { return nil })

	return <-resp
}
