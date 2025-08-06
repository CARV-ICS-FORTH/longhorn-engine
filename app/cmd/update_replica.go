package cmd

import (
	"errors"
	"fmt"

	"github.com/longhorn/longhorn-engine/pkg/controller/client"
	"github.com/sirupsen/logrus"
	"github.com/urfave/cli"

	"github.com/longhorn/longhorn-engine/pkg/types"
)

func UpdateReplicaCmd() cli.Command {
	return cli.Command{
		Name:      "update-replica",
		ShortName: "update",
		Flags: []cli.Flag{
			cli.StringFlag{
				Name:  "mode",
				Usage: "Replica mode. The value can be RO, RW or ERR.",
			},
		},
		Action: func(c *cli.Context) {
			_, err := updateReplica(c)
			if err != nil {
				logrus.WithError(err).Fatalf("Error running update replica command")
			}
		},
	}
}

func updateReplica(c *cli.Context) (*types.ControllerReplicaInfo, error) {
	if c.NArg() == 0 {
		return nil, errors.New("replica address is required")
	}
	replica := c.Args()[0]

	mode := types.Mode(c.String("mode"))
	if mode != types.WO && mode != types.RW && mode != types.ERR {
		return nil, fmt.Errorf("unsupported replica mode: %v", mode)
	}

	controllerClient, err := getControllerClient(c)
	if err != nil {
		return nil, err
	}
	defer func(controllerClient *client.ControllerClient) {
		err := controllerClient.Close()
		if err != nil {
			fmt.Printf("Error closing controller client connection: %v", err)
		}
	}(controllerClient)

	return controllerClient.ReplicaUpdate(replica, mode)
}
