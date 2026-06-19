// Package mqtt is the gateway's loopback MQTT v5 client. It wraps
// github.com/eclipse/paho.golang/paho with auto (re)connect, topic-pattern
// dispatch of inbound PUBLISH messages, and zap logging.
//
// pattern.go implements MQTT topic-filter matching (the "+" single-level and
// "#" multi-level wildcards) used to route received messages to handlers.
package mqtt

import "strings"

// Pattern is a parsed MQTT topic filter, held as its individual levels
// (the topic split on "/"). A level may be a literal, the single-level
// wildcard "+", or the multi-level wildcard "#" (only valid as the last level).
type Pattern []string

// ParsePattern splits an MQTT topic filter into levels and validates wildcard
// placement: "+" must occupy a whole level, and "#" must be the final level.
// It returns nil for an invalid filter.
//
// OEM 0x262eb0
func ParsePattern(filter string) Pattern {
	levels := strings.Split(strings.Trim(filter, "/"), "/")
	for i, level := range levels {
		// A "+" is only legal when it is the entire level.
		if strings.Index(level, "+") >= 0 && level != "+" {
			return nil
		}
		// A "#" is only legal as the whole, final level.
		if strings.Index(level, "#") >= 0 && (level != "#" || i != len(levels)-1) {
			return nil
		}
	}
	return Pattern(levels)
}

// String renders the pattern back into its "/"-joined topic-filter form.
//
// OEM 0x262e50
func (p Pattern) String() string {
	return strings.Join(p, "/")
}

// Match reports whether topic matches the filter, applying the "+" (one level)
// and "#" (zero or more trailing levels) wildcards.
//
// OEM 0x263070
func (p Pattern) Match(topic string) bool {
	levels := strings.Split(topic, "/")

	// An empty pattern only matches an empty topic.
	if len(p) == 0 {
		return len(levels) == len(p)
	}

	for i, level := range p {
		// "#" matches the remainder of the topic (including nothing).
		if level == "#" {
			return true
		}
		// Ran out of topic levels before exhausting the pattern.
		if i >= len(levels) {
			return false
		}
		// "+" matches exactly one level; a literal must compare equal.
		if level != "+" && level != levels[i] {
			return false
		}
	}

	// Every pattern level consumed exactly one topic level: lengths must agree.
	return len(levels) == len(p)
}
