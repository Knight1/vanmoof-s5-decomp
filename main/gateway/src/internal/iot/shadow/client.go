package shadow

import (
	"context"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"sync/atomic"

	"github.com/eclipse/paho.golang/paho"
)

// Reserved AWS IoT named-shadow topic fragments. The full topic is built as
//
//	$aws/things/<thingName>/shadow/name/<shadowName>/<action>[/<result>]
//
// where action ∈ {get, update} and result ∈ {accepted, rejected, delta}.
const (
	shadowTopicPrefix = "$aws/things/"  // OEM literal @0x2d28f8
	shadowTopicInfix  = "/shadow/name/" // OEM literal @0x2d... ("/shadow/name/")
	actionGet         = "get"           // shadow get request
	actionUpdate      = "update"        // shadow update request
	resultAccepted    = "/accepted"     // success reply suffix
	resultRejected    = "/rejected"     // error reply suffix
	resultDelta       = "/delta"        // desired-vs-reported delta notification
)

// Client is the AWS Device Shadow client for one named shadow. It is wired up by
// the top-level gateway (used by the configshadow sync path).
type Client struct {
	mqtt       mqttClient // request/response correlated MQTT v5 client
	thingName  string     // AWS IoT thing name (== bike serial)
	shadowName string     // the named shadow ("configshadow"/"telemetry-config")
	tokenSeq   uint64     // monotonically increasing client-token counter
}

// mqttClient is the subset of the IoT MQTT wrapper the shadow client uses.
type mqttClient interface {
	// Perform publishes a request and waits for the correlated accepted/rejected
	// reply.
	Perform(ctx context.Context, topic string, payload []byte) (*paho.Publish, error)
}

// topic builds a $aws/things/<thing>/shadow/name/<name>/<suffix> topic.
//
// OEM 0x... (shadow.(*Client).topic)
func (c *Client) topic(suffix string) string {
	return shadowTopicPrefix + c.thingName + shadowTopicInfix + c.shadowName + "/" + suffix
}

// clientToken returns a fresh correlation token for the next request. The OEM
// uses a monotonically increasing counter rendered as a decimal string.
//
// OEM 0x... (shadow.(*Client).clientToken)
func (c *Client) clientToken() string {
	n := atomic.AddUint64(&c.tokenSeq, 1)
	return strconv.FormatUint(n, 10)
}

// State fetches the current shadow document by publishing to the
// ".../get" topic and parsing the "/accepted" reply (or decoding the
// "/rejected" reply into an error). It returns the shadow state's
// desired/reported/delta sections.
//
// OEM 0x27a000
func (c *Client) State(ctx context.Context) (*State, error) {
	topic := c.topic(actionGet)

	// The get request body carries only a clientToken for correlation.
	body, _ := json.Marshal(struct {
		ClientToken string `json:"clientToken"`
	}{ClientToken: c.clientToken()})

	resp, err := c.mqtt.Perform(ctx, topic, body)
	if err != nil {
		return nil, fmt.Errorf("do request: %w", err) // @0x... ("do request: %w")
	}

	if strings.HasSuffix(resp.Topic, resultRejected) {
		return nil, parseShadowErrorResponse(resp.Payload)
	}

	// The accepted reply wraps the shadow under "state".
	var doc struct {
		State State `json:"state"`
	}
	if err := json.Unmarshal(resp.Payload, &doc); err != nil {
		return nil, fmt.Errorf("decoding error: %v", err) // @0x... ("decoding error: %v")
	}
	return &doc.State, nil
}
