package shadow

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
)

// reportRequest is the shadow update body the gateway publishes to report its
// state. Per the OEM reflection-type metadata (*shadow.reportRequest), the
// "state" object carries only the reported section, plus a clientToken for
// reply correlation:
//
//	{"state": {"reported": <raw>}, "clientToken": "<token>"}
type reportRequest struct {
	State struct {
		Reported json.RawMessage `json:"reported"`
	} `json:"state"`
	ClientToken string `json:"clientToken"`
}

// Report publishes the given reported state to the shadow's ".../update" topic
// and waits for the "/accepted" (or "/rejected") reply.
//
// On a rejected reply the AWS error is decoded (parseShadowErrorResponse); the
// per-request response handling is split into the Report.func1 / Report.func2
// closures in the OEM (the accepted/rejected branches of the correlated reply).
//
// OEM 0x279160
func (c *Client) Report(ctx context.Context, reported json.RawMessage) error {
	topic := c.topic(actionUpdate)

	var req reportRequest
	req.State.Reported = reported
	req.ClientToken = c.clientToken()

	body, err := json.Marshal(&req)
	if err != nil {
		return fmt.Errorf("marshal: %w", err)
	}

	resp, err := c.mqtt.Perform(ctx, topic, body)
	if err != nil {
		return fmt.Errorf("publish update: %w", err) // @0x... ("publish update: %w")
	}

	if strings.HasSuffix(resp.Topic, resultRejected) {
		return parseShadowErrorResponse(resp.Payload)
	}
	return nil
}
