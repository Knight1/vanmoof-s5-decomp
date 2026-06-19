package job

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
)

// executionSummary mirrors the AWS IoT Jobs JobExecution / execution-summary
// object delivered in the notify-next payload and in describe responses.
type executionSummary struct {
	JobID         string          `json:"jobId"`
	Status        string          `json:"status"`
	QueuedAt      int64           `json:"queuedAt"`
	ExecutionNum  int64           `json:"executionNumber"`
	VersionNumber int64           `json:"versionNumber"`
	JobDocument   json.RawMessage `json:"jobDocument"`
}

// --- request envelopes ----------------------------------------------------
//
// Every Jobs MQTT request carries a clientToken so the matching accepted/
// rejected response can be correlated. The three request shapes each get a
// SetClientToken so the MQTT wrapper can stamp a fresh token before publishing.

// getPendingExecutionsRequest is published to .../jobs/get to list the thing's
// IN_PROGRESS and QUEUED executions.
type getPendingExecutionsRequest struct {
	ClientToken string `json:"clientToken"`
}

// SetClientToken stamps the correlation token onto the request.
//
// OEM 0x2b48d0
func (r *getPendingExecutionsRequest) SetClientToken(token string) { r.ClientToken = token }

// updateExecutionRequest is published to .../jobs/<jobID>/update to transition a
// job execution to a new status.
type updateExecutionRequest struct {
	Status      string `json:"status"`
	ClientToken string `json:"clientToken"`
}

// SetClientToken stamps the correlation token onto the request.
//
// OEM 0x2b4be0
func (r *updateExecutionRequest) SetClientToken(token string) { r.ClientToken = token }

// describeExecutionRequest is published to .../jobs/<jobID>/get to fetch a single
// execution (including the full job document).
type describeExecutionRequest struct {
	IncludeJobDocument bool   `json:"includeJobDocument"`
	ClientToken        string `json:"clientToken"`
}

// SetClientToken stamps the correlation token onto the request.
//
// OEM 0x2b4e50
func (r *describeExecutionRequest) SetClientToken(token string) { r.ClientToken = token }

// topic builds a $aws/things/<thing>/jobs/<suffix> topic.
func (c Client) topic(suffix string) string {
	return jobsTopicPrefix + c.thingName + jobsTopicInfix + suffix
}

// rejectedSuffix marks an error response from AWS IoT Jobs. Both the accepted
// and rejected replies arrive on the same correlated reply; the gateway checks
// whether the reply topic ends in "/rejected".
const rejectedSuffix = "/rejected" // OEM literal @0x2d1745 ("/rejected")

// getPendingExecutions asks AWS for the executions already queued for the thing
// and runs the first pending job (if any). It is called once at Start.
//
// OEM 0x2b4950
func (c Client) getPendingExecutions(ctx context.Context) error {
	c.log.Info("Getting pending executions") // @0x2d906e

	topic := c.topic(getPendingSuffix) // $aws/things/<thing>/jobs/get
	body, _ := json.Marshal(&getPendingExecutionsRequest{})

	resp, err := c.mqtt.Perform(ctx, topic, body)
	if err != nil {
		return err
	}
	if strings.HasSuffix(resp.Topic, rejectedSuffix) {
		return fmt.Errorf("parsing job error: %w", parseJobErrorResponse(resp.Payload)) // @0x2d6c6f
	}

	var result struct {
		InProgressJobs []executionSummary `json:"inProgressJobs"`
		QueuedJobs     []executionSummary `json:"queuedJobs"`
	}
	if err := json.Unmarshal(resp.Payload, &result); err != nil {
		return fmt.Errorf("unmarshal response: %w", err) // @0x2d76f6
	}

	c.log.Infow("Queued jobs", "amount", len(result.QueuedJobs))          // @0x2d259c ("Queued jobs"), @0x2d0a70 ("amount")
	c.log.Infow("In progress jobs", "amount", len(result.InProgressJobs)) // @ "In progress jobs"

	// Run the first pending execution: prefer an in-progress job, otherwise the
	// first queued job (the OEM picks queuedJobs[0] when no in-progress job).
	var first *executionSummary
	switch {
	case len(result.InProgressJobs) > 0:
		first = &result.InProgressJobs[0]
	case len(result.QueuedJobs) > 0:
		first = &result.QueuedJobs[0]
	default:
		c.log.Info("No pending jobs") // @ "No pending jobs"
		return nil
	}

	c.log.Info("executing first pending job") // @0x2d9a48
	// Re-describe to pull the full job document before executing.
	exec, err := c.describeExecution(ctx, first.JobID)
	if err != nil {
		return fmt.Errorf("describe execution: %w", err) // @0x2d7226
	}
	if err := c.execute(ctx, exec); err != nil {
		return fmt.Errorf("execute execution: %w", err) // @0x2d6a23
	}
	return nil
}

// updateExecution publishes a status transition to
// $aws/things/<thing>/jobs/<jobID>/update and, on a rejected reply, decodes the
// AWS error.
//
// OEM 0x2b4c60
func (c Client) updateExecution(ctx context.Context, jobID, status string) (executionSummary, error) {
	topic := c.topic(jobID + "/update") // suffix "/update" @0x2d0cae
	body, _ := json.Marshal(&updateExecutionRequest{Status: status})

	resp, err := c.mqtt.Perform(ctx, topic, body)
	if err != nil {
		return executionSummary{}, err
	}
	if strings.HasSuffix(resp.Topic, rejectedSuffix) {
		return executionSummary{}, parseJobErrorResponse(resp.Payload)
	}
	return executionSummary{}, nil
}

// describeExecution fetches a single execution (with its job document) from
// $aws/things/<thing>/jobs/<jobID>/get.
//
// OEM 0x2b4ed0
func (c Client) describeExecution(ctx context.Context, jobID string) (executionSummary, error) {
	topic := c.topic(jobID + "/get") // suffix "/get" @0x2d031e
	body, _ := json.Marshal(&describeExecutionRequest{IncludeJobDocument: true})

	resp, err := c.mqtt.Perform(ctx, topic, body)
	if err != nil {
		return executionSummary{}, err
	}
	if strings.HasSuffix(resp.Topic, rejectedSuffix) {
		return executionSummary{}, fmt.Errorf("parsing job error: %w", parseJobErrorResponse(resp.Payload)) // @0x2d6c6f
	}

	var result struct {
		Execution executionSummary `json:"execution"`
	}
	if err := json.Unmarshal(resp.Payload, &result); err != nil {
		return executionSummary{}, fmt.Errorf("unmarshal response: %w", err) // @0x2d76f6
	}
	return result.Execution, nil
}
