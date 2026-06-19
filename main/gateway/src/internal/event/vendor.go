package event

// This file collects the small vendor shims the handlers lean on. In the OEM
// binary these are direct calls into github.com/fxamacker/cbor and the package
// logger (go.uber.org/zap's SugaredLogger); they are modelled here, not
// reconstructed, so the handler logic above reads cleanly.

import (
	"github.com/fxamacker/cbor/v2"
	"go.uber.org/zap"
)

// log is the package-level structured logger (the gateway passes a *zap.Logger
// down to the event handlers at construction).
var log *zap.SugaredLogger

// cborUnmarshal decodes a CBOR message payload into v.
func cborUnmarshal(data []byte, v any) error {
	return cbor.Unmarshal(data, v)
}

// logf logs a formatted diagnostic at error level (e.g. "unmarshal: %v").
func logf(format string, args ...any) {
	log.Errorf(format, args...)
}
