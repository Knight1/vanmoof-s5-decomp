// Package job implements the AWS IoT Jobs client used by the gateway to receive
// and execute cloud-pushed commands for the bike.
//
// The cloud → bike command channel rides the reserved AWS IoT Jobs MQTT topics
// under $aws/things/<thing>/jobs/. The gateway:
//
//   - subscribes to the "notify-next" topic, which fires whenever a new job is
//     queued for the thing (handleNotifyNext);
//   - on startup, drains any already-pending executions (getPendingExecutions);
//   - for each next job, marks it IN_PROGRESS, dispatches it to a registered
//     handler keyed by the job document's "type" field, and reports the terminal
//     status (SUCCEEDED / FAILED / REJECTED) back to AWS (execute, updateStatus).
//
// Reconstructed from the stripped go1.19 gateway binary (Ghidra program
// `gateway`, image base 0x10000). The OEM symbol names come from the gopclntab.
package job

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/eclipse/paho.golang/paho"
	"github.com/tidwall/gjson"
	"go.uber.org/zap"
)

// AWS IoT Jobs job execution status values, reported on the
// $aws/things/<thing>/jobs/<jobID>/update topic.
const (
	statusInProgress = "IN_PROGRESS"
	statusSucceeded  = "SUCCEEDED"
	statusFailed     = "FAILED"
	statusRejected   = "REJECTED"
)

// Reserved AWS IoT Jobs topic fragments. The full job topic is built as
//
//	$aws/things/<thingName>/jobs/<suffix>
//
// (see the topic helpers in request.go). "notify-next" is the only subscribed
// topic; the rest are publish/response request topics.
const (
	jobsTopicPrefix  = "$aws/things/" // OEM literal @0x2d28f8 ("$aws/things/")
	jobsTopicInfix   = "/jobs/"       // OEM literal @0x2d2787 ("/jobs/")
	notifyNextTopic  = "notify-next"  // OEM literal @0x2d27cd
	getPendingSuffix = "get"          // OEM literal @0x2d023d ("get")
)

// handler runs a single job operation. It is given the per-job context (carrying
// the timeout derived from the job's timeoutInMinutes — see document.Context)
// and the parsed job document, and returns an error to mark the job FAILED.
type handler func(ctx context.Context, doc document) error

// Client is the AWS IoT Jobs client. It is wired up by the top-level gateway,
// which registers the operation handlers (only "log_upload" in this build).
type Client struct {
	mqtt      mqttClient         // loopback/AWS MQTT v5 client (paho wrapper)
	thingName string             // the AWS IoT thing name (== bike serial)
	handlers  map[string]handler // job "type" -> handler; one key: "log_upload"
	log       *zap.SugaredLogger
}

// mqttClient is the subset of the IoT MQTT wrapper the jobs client uses:
// request/response correlated publishes and subscribe/unsubscribe.
type mqttClient interface {
	// Perform issues a request and waits for the correlated response on the
	// reply topic; used for get-pending / update / describe.
	Perform(ctx context.Context, topic string, payload []byte) (*paho.Publish, error)
	Subscribe(ctx context.Context, topic string, cb func(*paho.Publish)) error
	Unsubscribe(ctx context.Context, topic string) error
}

// Start subscribes to the thing's notify-next topic and then immediately drains
// any executions already queued in the cloud.
//
// OEM 0x2b2640
func (c Client) Start(ctx context.Context) error {
	c.log.Info("Starting job client") // @0x2d587b

	topic := c.topic(notifyNextTopic) // $aws/things/<thing>/jobs/notify-next
	if err := c.mqtt.Subscribe(ctx, topic, c.handleNotifyNext); err != nil {
		return fmt.Errorf("subscribe notify-next: %w", err) // @0x2d8e8d
	}
	return c.getPendingExecutions(ctx)
}

// Stop unsubscribes from the notify-next topic.
//
// OEM 0x2b2d10
func (c *Client) Stop(ctx context.Context) error {
	c.log.Info("Stopping job client") // @0x2d588e

	topic := c.topic(notifyNextTopic) // $aws/things/<thing>/jobs/notify-next
	if err := c.mqtt.Unsubscribe(ctx, topic); err != nil {
		return fmt.Errorf("unsubscribing to notify-next: %w", err) // @0x2d22... ("unsubscribing to notify-next: %w")
	}
	return nil
}

// notifyNextPayload is the body AWS publishes to .../jobs/notify-next when a new
// job becomes the next pending execution. Only the execution summary is used.
type notifyNextPayload struct {
	Execution executionSummary `json:"execution"`
}

// handleNotifyNext is the notify-next subscription callback. It unmarshals the
// notifyNextPayload; if an execution is present, it runs it. A new goroutine is
// spawned to do the actual work (handleNotifyNext.func1, OEM 0x2b3210) so the
// MQTT receive loop is not blocked.
//
// OEM 0x2b2e80
func (c Client) handleNotifyNext(p *paho.Publish) {
	var payload notifyNextPayload
	if err := json.Unmarshal(p.Payload, &payload); err != nil {
		c.log.Errorw("unmarshalling notifyNextPayload", "error", err) // @0x2d... ("unmarshalling notifyNextPayload")
		return
	}
	if payload.Execution.JobID == "" {
		// No next execution queued.
		return
	}
	// OEM spawns handleNotifyNext.func1 (0x2b3210): runs execute on its own
	// goroutine with a fresh background context.
	go func(exec executionSummary) {
		c.log.Info("Executing job in handleNotifyNext") // @0x2d... ("Executing job in handleNotifyNext")
		_ = c.execute(context.Background(), exec)
	}(payload.Execution)
}

// execute is the job dispatcher. It parses the job document, looks the document
// "type" up in the handler map, and drives the IN_PROGRESS → terminal status
// reporting around the handler call.
//
//   - document parse failure        → error ("unmarshal job document: %w")
//   - unknown "type"                → log "No matching job handler found." +
//     mark REJECTED
//   - known "type"                  → mark IN_PROGRESS → run handler →
//     mark SUCCEEDED (ok) or FAILED (handler error)
//
// OEM 0x2b34d0
func (c Client) execute(ctx context.Context, exec executionSummary) (err error) {
	var doc document
	if uerr := json.Unmarshal(exec.JobDocument, &doc); uerr != nil {
		return fmt.Errorf("unmarshal job document: %w", uerr) // @0x2d9722
	}

	// Per-job context carrying the job's timeout (timeoutInMinutes, default 1h).
	ctx, cancel := doc.Context(ctx)
	defer cancel()

	// The job "type" selects the handler; "job_id" is used for status reporting.
	jobType := gjson.GetBytes(exec.JobDocument, "type").String() // field "type" @0x2d04fe
	jobID := exec.JobID                                          // field "job_id" @0x2d0afa

	h, ok := c.handlers[jobType]
	if !ok {
		c.log.Errorw("No matching job handler found.", "type", jobType) // @0x2db32c
		if serr := c.updateStatus(ctx, exec, statusRejected, jobID); serr != nil {
			return fmt.Errorf("setting status to REJECTED: %w", serr) // @ "setting status to REJECTED: %w"
		}
		return nil
	}

	c.log.Info("Set job status IN_PROGRESS") // @0x2d90bc
	if serr := c.updateStatus(ctx, exec, statusInProgress, jobID); serr != nil {
		return fmt.Errorf("setting status to IN_PROGRESS: %w", serr) // @ "setting status to IN_PROGRESS: %w"
	}

	c.log.Info("Handle job") // @0x2d1ef2
	if herr := h(ctx, doc); herr != nil {
		c.log.Info("Set job status FAILED") // @0x2d687f
		if serr := c.updateStatus(ctx, exec, statusFailed, jobID); serr != nil {
			return fmt.Errorf("setting status to FAILED: %w", serr) // @ "setting status to FAILED: %w"
		}
		return fmt.Errorf("execute execution: %w", herr) // @0x2d6a23
	}

	c.log.Info("Set job status SUCCEEDED") // @0x2d8246
	if serr := c.updateStatus(ctx, exec, statusSucceeded, jobID); serr != nil {
		return fmt.Errorf("setting status to SUCCEEDED: %w", serr) // @ "setting status to SUCCEEDED: %w"
	}
	return nil
}

// updateStatus reports a new execution status to AWS via updateExecution, then
// re-describes the execution to confirm the transition.
//
// OEM 0x2b40e0
func (c Client) updateStatus(ctx context.Context, exec executionSummary, status, jobID string) error {
	// describeExecution after the update, to read back the new state.
	if _, err := c.describeExecution(ctx, jobID); err != nil {
		return fmt.Errorf("describing execution: %w", err) // @0x2d837e
	}
	if _, err := c.updateExecution(ctx, jobID, status); err != nil {
		return fmt.Errorf("updating execution: %w", err) // @0x2d774e
	}
	return nil
}
