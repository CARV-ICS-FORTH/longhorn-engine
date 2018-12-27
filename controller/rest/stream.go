package rest

import (
	"fmt"
	"math/rand"
	"net/http"
	"strconv"
	"time"

	"github.com/Sirupsen/logrus"
	"github.com/gorilla/websocket"
	"github.com/rancher/go-rancher/api"

	"github.com/rancher/longhorn-engine/broadcaster"
)

const (
	keepAlivePeriod = 5 * time.Second
	timeoutPeriod   = 3 * keepAlivePeriod

	writeWait = 10 * time.Second
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

func init() {
	rand.Seed(time.Now().UnixNano())
}

func NewStreamHandlerFunc(streamType string,
	eventProcessor func(event *broadcaster.Event, ctx *api.ApiContext) (interface{}, error),
	b *broadcaster.Broadcaster, events ...string) func(w http.ResponseWriter, r *http.Request) error {

	return func(w http.ResponseWriter, r *http.Request) error {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return err
		}
		fields := logrus.Fields{
			"id":   strconv.Itoa(rand.Int()),
			"type": streamType,
		}
		logrus.WithFields(fields).Info("websocket: open")

		watcher := b.NewWatcher(events...)
		defer watcher.Close()

		done := make(chan struct{})

		timeoutTimer := time.NewTimer(timeoutPeriod)

		conn.SetPongHandler(func(data string) error {
			timeoutTimer.Reset(timeoutPeriod)
			return nil
		})

		go func() {
			defer close(done)
			for {
				_, _, err := conn.ReadMessage()
				if err != nil {
					logrus.WithFields(fields).Info(err.Error())
					return
				}
			}
		}()

		apiContext := api.GetApiContext(r)
		keepAliveTicker := time.NewTicker(keepAlivePeriod)
		for {
			select {
			case <-done:
				return nil
			case event := <-watcher.Events():
				data, err := eventProcessor(event, apiContext)
				if err != nil {
					return err
				}
				conn.SetWriteDeadline(time.Now().Add(writeWait))
				if err = conn.WriteJSON(data); err != nil {
					return err
				}
			case <-keepAliveTicker.C:
				err = conn.WriteControl(websocket.PingMessage, []byte{}, time.Now().Add(writeWait))
				if err != nil {
					return err
				}
			case <-timeoutTimer.C:
				logrus.WithFields(fields).Info("websocket: no response for ping, close websocket due to timeout")
				return fmt.Errorf("websocket ping timeout")
			}
		}
	}
}