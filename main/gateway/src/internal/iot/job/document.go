package job

import (
	"context"
	"encoding/json"
	"time"
)

// document is the parsed AWS IoT Jobs job document — the operator-authored JSON
// payload attached to a job. The gateway only reads a handful of fields here;
// handlers (e.g. log_upload) read their own fields directly from the raw bytes.
type document struct {
	Type             string          `json:"type"`
	TimeoutInMinutes int64           `json:"timeoutInMinutes"`
	Raw              json.RawMessage `json:"-"`
}

// defaultJobTimeout is used when the job document does not set (a positive)
// timeoutInMinutes.
const defaultJobTimeout = time.Hour // OEM 3600000000000 ns

// Context derives a per-job context from the parent, with a deadline taken from
// the document's timeoutInMinutes (minutes → ns). A non-positive timeout falls
// back to a 1-hour default.
//
// OEM 0x2b4340
func (d document) Context(parent context.Context) (context.Context, context.CancelFunc) {
	if d.TimeoutInMinutes > 0 {
		// OEM: timeoutInMinutes * 60_000_000_000 ns.
		return context.WithTimeout(parent, time.Duration(d.TimeoutInMinutes)*time.Minute)
	}
	return context.WithTimeout(parent, defaultJobTimeout)
}
