package job

import (
	"encoding/json"
	"fmt"
)

// errorResponse is the AWS IoT Jobs error reply published to a "…/rejected"
// topic.
type errorResponse struct {
	Code           string `json:"code"`
	Message        string `json:"message"`
	ClientToken    string `json:"clientToken"`
	Timestamp      int64  `json:"timestamp"`
	ExecutionState struct {
		Status        string `json:"status"`
		VersionNumber int64  `json:"versionNumber"`
	} `json:"executionState"`
}

// jobsErrorCode is one of the AWS IoT Jobs rejected-response error codes. The
// gateway maps the response "code" string to a known code; an unrecognised code
// is surfaced verbatim.
type jobsErrorCode int

// The AWS IoT Jobs error-code set, in the order the lookup table is built
// (parseJobErrorResponse, OEM 0x2b4430). The string keys are the on-the-wire
// "code" values.
const (
	errInvalidTopic jobsErrorCode = iota
	errInvalidJSON
	errInvalidRequest
	errInvalidStateTransition
	errResourceNotFound
	errVersionMismatch
	errInternalError
	errRequestThrottled
	errTerminalStateReached
)

// jobsErrorCodes maps the on-the-wire AWS error "code" to the enum. Built once
// (the OEM does it inline as a 9-entry map literal).
var jobsErrorCodes = map[string]jobsErrorCode{
	"InvalidTopic":           errInvalidTopic,           // @0x2d... ("InvalidTopic")
	"InvalidJson":            errInvalidJSON,            // @0x2d250d
	"InvalidRequest":         errInvalidRequest,         // @0x2d... ("InvalidRequest")
	"InvalidStateTransition": errInvalidStateTransition, // @0x2d7058
	"ResourceNotFound":       errResourceNotFound,       // @0x2d... ("ResourceNotFound")
	"VersionMismatch":        errVersionMismatch,        // @0x2d... ("VersionMismatch")
	"InternalError":          errInternalError,          // @0x2d... ("InternalError")
	"RequestThrottled":       errRequestThrottled,       // @0x2d... ("RequestThrottled")
	"TerminalStateReached":   errTerminalStateReached,   // @0x2d... ("TerminalStateReached")
}

// jobError is the typed error returned to callers on a rejected Jobs reply.
type jobError struct {
	Code    jobsErrorCode
	Message string
}

func (e jobError) Error() string { return fmt.Sprintf("%s", e.Message) }

// parseJobErrorResponse decodes a rejected Jobs reply body and maps its "code"
// to a known jobsErrorCode. An unrecognised code is wrapped verbatim into a
// formatted error.
//
// OEM 0x2b4430
func parseJobErrorResponse(body []byte) error {
	var resp errorResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		return fmt.Errorf("unmarshal error body: %w", err) // @0x2d8726
	}

	if code, ok := jobsErrorCodes[resp.Code]; ok {
		return jobError{Code: code, Message: resp.Message}
	}
	// Unknown code: surface the raw code and message.
	return fmt.Errorf("unknown error code %s: %s", resp.Code, resp.Message) // @0x2d8f55
}
