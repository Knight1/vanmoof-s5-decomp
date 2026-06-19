package gateway

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"time"

	"go.uber.org/zap"

	"github.com/VanMoof/embedded/gateway/internal/bike"
)

// maxLogRange caps the time window a log_upload job may request. OEM constant
// 86400000000000 ns == 24h.
const maxLogRange = 24 * time.Hour // 86400000000000 ns

// logJobDoc is the shape of the AWS IoT Job document for "log_upload".
// The decode is "unmarshal job document: %w".
type logJobDoc struct {
	URL   string    `json:"url"` // presigned PUT URL (required; "missing URL")
	Start time.Time `json:"start_time"`
	End   time.Time `json:"end_time"`
	Units []string  `json:"units"` // systemd units to include
}

// LogJob is the handler for the single registered AWS IoT Job operation,
// "log_upload" (registered in New @0x2b6680). It is a diagnostic
// log-exfiltration-on-demand routine: collect journald logs for a bounded time
// range into a temp file and PUT it to a presigned URL. It is NOT an OTA or
// arbitrary-command channel.
type LogJob struct {
	log    *zap.Logger
	prov   bike.ProvisioningData
	client *http.Client
}

// NewLogJobHandler builds the log_upload job handler. (Inlined into New
// @0x2b6680, where the key "log_upload" is mapassign'd into the jobs handler
// map.)
func NewLogJobHandler(log *zap.Logger, prov bike.ProvisioningData) *LogJob {
	return &LogJob{
		log:    log,
		prov:   prov,
		client: &http.Client{},
	}
}

// Handle runs a log_upload job. OEM 0x2b8310.
//
// doc is the raw AWS IoT Jobs job-document JSON; the unexported job.document
// type in the settled job seam is not exported, so the handler receives the raw
// bytes and decodes its own fields (as the OEM log_upload handler does).
//
// Flow:
//  1. Decode the job document  -> "unmarshal job document: %w".
//  2. The document must carry an upload URL -> else "missing URL".
//  3. The [start,end] range is capped at 24h -> else
//     "log collection range too large, max %s allowed" (maxLogRange).
//  4. collect(...) runs journalctl over the range/units, writing to a temp
//     file (os.CreateTemp): "create temp log file: %w", then
//     "collect logs: %w", then fsync "sync log file to disk: %w" and
//     "seek to beginning: %w".
//  5. PUT the temp file to the URL ("new request: %w" / "do request: %w");
//     a 200 is success, anything else reads the body and returns
//     "unsuccessful response: status %d, body :%s".
func (j *LogJob) Handle(ctx context.Context, doc []byte) error {
	var d logJobDoc
	if err := json.Unmarshal(doc, &d); err != nil {
		return fmt.Errorf("unmarshal job document: %w", err)
	}

	if d.URL == "" {
		return fmt.Errorf("missing URL")
	}

	if d.End.Sub(d.Start) > maxLogRange {
		return fmt.Errorf("log collection range too large, max %s allowed", maxLogRange)
	}

	f, err := collect(ctx, d.Start, d.End, d.Units)
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	defer f.Close()

	req, err := http.NewRequestWithContext(ctx, http.MethodPut, d.URL, f)
	if err != nil {
		return fmt.Errorf("new request: %w", err)
	}

	resp, err := j.client.Do(req)
	if err != nil {
		return fmt.Errorf("do request: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("unsuccessful response: status %d, body :%s", resp.StatusCode, body)
	}
	return nil
}

// collect runs journalctl over the given range/units and writes its output to
// a temp file, returning the rewound file ready for upload. OEM 0x2b8ca0
// (folded into Handle in the OEM image).
//
// Errors are wrapped: "create temp log file: %w", "collect logs: %w",
// "sync log file to disk: %w", "seek to beginning: %w". The temp file is
// created with the "logs" name token.
func collect(ctx context.Context, start, end time.Time, units []string) (*os.File, error) {
	f, err := os.CreateTemp("", "logs")
	if err != nil {
		return nil, fmt.Errorf("create temp log file: %w", err)
	}

	cmd := exec.CommandContext(ctx, "journalctl", journalctlArgs(start, end, units)...)
	cmd.Stdout = f
	if err := cmd.Run(); err != nil {
		f.Close()
		os.Remove(f.Name())
		return nil, fmt.Errorf("collect logs: %w", err)
	}

	if err := f.Sync(); err != nil {
		f.Close()
		os.Remove(f.Name())
		return nil, fmt.Errorf("sync log file to disk: %w", err)
	}

	if _, err := f.Seek(0, io.SeekStart); err != nil {
		f.Close()
		os.Remove(f.Name())
		return nil, fmt.Errorf("seek to beginning: %w", err)
	}
	return f, nil
}

// journalctlArgs builds the journalctl argument vector for the given time
// range and unit set. OEM 0x2b8f10.
//
// Timestamps use the Go layout "2006-01-02 15:04:05" (literal @0x2d56ff). The
// fixed flags (all confirmed literals) are "--since <t1>" (@0x2d0ca0),
// "--until <t2>" (@0x2d0ca7), "--no-pager" (@0x2d1dbc), "--output <fmt>"
// (@0x2d1133); one "--unit <unit>" pair (token @"--unit") is appended per
// requested unit.
//
// NOTE: the "--output" format VALUE is not a distinctly resolvable literal in
// the image (the decompiler folds it as a self-referencing slice header), so
// the exact format token below ("short-precise") is the most plausible
// reconstruction, not a verified string.
func journalctlArgs(start, end time.Time, units []string) []string {
	const layout = "2006-01-02 15:04:05"

	args := []string{
		"--since", start.Format(layout),
		"--until", end.Format(layout),
		"--no-pager",
		"--output", "short-precise",
	}
	for _, u := range units {
		args = append(args, "--unit", u)
	}
	return args
}

// Handle.func1 (0x2b8c40) / Handle.func2 (0x2b8bd0) are the small io.Reader
// body wrappers Go emits to feed the temp file to http.NewRequestWithContext;
// they carry no VanMoof logic of their own.
