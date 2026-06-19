package event

import (
	"fmt"
	"strconv"
)

// timezoneTopic carries the cellular modem's network/registration info, which
// embeds the network-provided local time and timezone offset.
const timezoneTopic = "modem/info/network"

// timezonePayload is the CBOR body of a modem/info/network message. Time is the
// modem's network-time string in the +CCLK form "yy/MM/dd,hh:mm:ss±zz", where
// ±zz is the timezone offset in quarter-hours.
type timezonePayload struct {
	Time string `cbor:"time"`
}

// HandleTimezone subscribes to modem/info/network and, for each message, parses
// the timezone offset out of the modem time string and reports it (in minutes
// east of UTC) via setOffset. Messages with an unparseable time field are
// dropped.
//
// OEM 0x294e40
func HandleTimezone(bus Bus, setOffset func(minutes int)) error {
	err := bus.Subscribe(timezoneTopic, func(_ string, payload []byte) {
		// OEM 0x295140  (HandleTimezone.func1)
		var msg timezonePayload
		if err := cborUnmarshal(payload, &msg); err != nil {
			return
		}
		offset, err := parseOffset(msg.Time)
		if err != nil {
			return
		}
		setOffset(offset)
	})
	if err != nil {
		return fmt.Errorf("subscribe: %w", err)
	}
	return nil
}

// parseOffset extracts the timezone offset from a +CCLK-style modem time string
// "yy/MM/dd,hh:mm:ss±zz" and returns it in minutes east of UTC. The sign byte
// is at index 17 and the quarter-hour count begins at index 18; the count is
// multiplied by 15 to get minutes (and negated for a '-' sign).
//
// OEM 0x295270
func parseOffset(s string) (int, error) {
	if len(s) < 18 {
		return 0, fmt.Errorf("invalid timestamp: %q", s)
	}

	sign := s[17]
	quarters, err := strconv.Atoi(s[18:])
	if err != nil {
		return 0, fmt.Errorf("invalid offset: %w", err)
	}

	minutes := quarters * 15
	if sign == '-' {
		minutes = -minutes
	}
	return minutes, nil
}
