package cmd

import (
	"fmt"
	"os"
	"text/tabwriter"

	"github.com/longhorn/longhorn-engine/pkg/controller/client"
	"github.com/sirupsen/logrus"
	"github.com/urfave/cli"

	replicaClient "github.com/longhorn/longhorn-engine/pkg/replica/client"
	"github.com/longhorn/longhorn-engine/pkg/types"
)

func LsReplicaCmd() cli.Command {
	return cli.Command{
		Name:      "ls-replica",
		ShortName: "ls",
		Action: func(c *cli.Context) {
			if err := lsReplica(c); err != nil {
				logrus.WithError(err).Fatalf("Error running ls command")
			}
		},
	}
}

func lsReplica(c *cli.Context) error {
	controllerClient, err := getControllerClient(c)
	if err != nil {
		return err
	}
	defer func(controllerClient *client.ControllerClient) {
		err := controllerClient.Close()
		if err != nil {
			logrus.Errorf("Error closing controller client: %v", err)
		}
	}(controllerClient)
	volumeName := c.GlobalString("volume-name")

	reps, err := controllerClient.ReplicaList()
	if err != nil {
		return err
	}

	format := "%s\t%s\t%v\n"
	tw := tabwriter.NewWriter(os.Stdout, 0, 20, 1, ' ', 0)
	_, err = fmt.Fprintf(tw, format, "ADDRESS", "MODE", "CHAIN")
	if err != nil {
		return err
	}
	for _, r := range reps {
		if r.Mode == types.ERR {
			_, err := fmt.Fprintf(tw, format, r.Address, r.Mode, "")
			if err != nil {
				return err
			}
			continue
		}
		chain := interface{}("")
		chainList, err := getChain(r.Address, volumeName)
		if err == nil {
			chain = chainList
		}
		_, err = fmt.Fprintf(tw, format, r.Address, r.Mode, chain)
		if err != nil {
			return err
		}
	}
	err = tw.Flush()
	if err != nil {
		return err
	}

	return nil
}

func getChain(address, volumeName string) ([]string, error) {
	// We don't know the replica's instanceName, so create a client without it.
	repClient, err := replicaClient.NewReplicaClient(address, volumeName, "")
	if err != nil {
		return nil, err
	}
	defer func(repClient *replicaClient.ReplicaClient) {
		err := repClient.Close()
		if err != nil {
			logrus.Errorf("Error closing replica client: %v", err)
		}
	}(repClient)

	r, err := repClient.GetReplica()
	if err != nil {
		return nil, err
	}

	return r.Chain, err
}
