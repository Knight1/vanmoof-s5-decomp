package shadow

import (
	"encoding/json"
	"fmt"
)

// errorResponse is the AWS IoT Device Shadow error reply published to a
// "…/rejected" topic. Unlike Jobs (string codes), shadow errors use a numeric
// HTTP-like code.
type errorResponse struct {
	Code        int    `json:"code"`
	Message     string `json:"message"`
	Timestamp   int64  `json:"timestamp"`
	ClientToken string `json:"clientToken"`
}

// shadowErrorCode is an AWS IoT Device Shadow rejected-response code. The codes
// the gateway recognises are the standard shadow error codes (the lookup table
// is built in parseShadowErrorResponse, OEM 0x278d90, with the codes below).
type shadowErrorCode int

// Recognised AWS Device Shadow error codes (numeric "code" field). The table is
// built in the order the OEM inserts them (entries at metadata offsets 0x190..,
// the standard shadow HTTP-style codes).
const (
	codeBadRequest          shadowErrorCode = 400 // malformed request / invalid JSON
	codeUnauthorized        shadowErrorCode = 401
	codeForbidden           shadowErrorCode = 403
	codeNotFound            shadowErrorCode = 404 // no shadow document
	codeVersionConflict     shadowErrorCode = 409 // version conflict
	codeRequestTooLarge     shadowErrorCode = 413
	codeUnsupportedEncoding shadowErrorCode = 415
	codeTooManyRequests     shadowErrorCode = 429
	codeInternalError       shadowErrorCode = 500
)

// knownShadowCodes is the set of recognised numeric shadow error codes.
var knownShadowCodes = map[int]shadowErrorCode{
	400: codeBadRequest,
	401: codeUnauthorized,
	403: codeForbidden,
	404: codeNotFound,
	409: codeVersionConflict,
	413: codeRequestTooLarge,
	415: codeUnsupportedEncoding,
	429: codeTooManyRequests,
	500: codeInternalError,
}

// shadowError is the typed error returned to callers on a rejected shadow reply.
type shadowError struct {
	Code    shadowErrorCode
	Message string
}

func (e shadowError) Error() string { return e.Message }

// parseShadowErrorResponse decodes a rejected shadow reply body and maps its
// numeric "code" to a known shadowErrorCode. An unrecognised code is surfaced
// verbatim.
//
// OEM 0x278d90
func parseShadowErrorResponse(body []byte) error {
	var resp errorResponse
	if err := json.Unmarshal(body, &resp); err != nil {
		return fmt.Errorf("unmarshal error body: %w", err) // @0x2d8726
	}

	if code, ok := knownShadowCodes[resp.Code]; ok {
		return shadowError{Code: code, Message: resp.Message}
	}
	// Unknown code: surface the raw code and message.
	return fmt.Errorf("unknown error code %d: %s", resp.Code, resp.Message) // @0x2d8f3c
}
