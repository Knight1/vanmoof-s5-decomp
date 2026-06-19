package gateway

import (
	"go.uber.org/zap"
	"go.uber.org/zap/zapcore"
)

// NewLogger builds the process-wide zap logger. OEM 0x2b92a0.
//
// The debug flag selects the level: debug=true -> DebugLevel, otherwise
// InfoLevel (the OEM encodes this as a leveler set from a bool). The encoder
// is the production console/JSON encoder writing to "stdout"; the time and
// level encoders are the capital short forms. The resulting logger is also
// installed as the global (zap.ReplaceGlobals), so the per-component
// sub-loggers in New() come from zap.L().Named(...).
func NewLogger(debug bool) *zap.Logger {
	level := zap.InfoLevel
	if debug {
		level = zap.DebugLevel
	}

	cfg := zap.Config{
		Level:       zap.NewAtomicLevelAt(level),
		Encoding:    "json",
		OutputPaths: []string{"stdout"},
		EncoderConfig: zapcore.EncoderConfig{
			TimeKey:        "T",
			LevelKey:       "L",
			MessageKey:     "M",
			CallerKey:      "C",
			NameKey:        "N",
			StacktraceKey:  "S",
			FunctionKey:    "F",
			EncodeLevel:    zapcore.CapitalLevelEncoder,
			EncodeTime:     zapcore.ISO8601TimeEncoder,
			EncodeDuration: zapcore.SecondsDurationEncoder,
			EncodeCaller:   zapcore.ShortCallerEncoder,
		},
	}

	log, err := cfg.Build()
	if err != nil {
		panic(err)
	}
	zap.ReplaceGlobals(log)
	return log
}
