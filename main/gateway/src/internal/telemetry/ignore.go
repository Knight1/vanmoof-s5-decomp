package telemetry

import "github.com/VanMoof/embedded/gateway/internal/mqtt"

// ignorePatterns is the package-level deny-list of bus topics that must never
// be forwarded to the cloud, even if a subscription would otherwise match them.
// The set covers the BLE proxy control plane and the security-sensitive
// auth / unlock-code topics. It is compiled once in the package init from
// mqtt.ParsePattern.
//
// OEM source: ignore.go (built by telemetry.init, 0x2b03d0; stored at the
// package global DAT_00691f60). The six pattern literals are reproduced
// verbatim from the image. Three are clearly anchored regex topic patterns
// ("^ble/proxy$", "^ble/findmy/fmna_auth$", "^settings/backup_unlock_code$");
// the remaining three ("/ftp", "_app$", "/set$") are stored as-is.
var ignorePatterns []mqtt.Pattern

func init() {
	for _, p := range []string{
		"^ble/proxy$",
		"/ftp",
		"_app$",
		"/set$",
		"^ble/findmy/fmna_auth$",
		"^settings/backup_unlock_code$",
	} {
		ignorePatterns = append(ignorePatterns, mqtt.ParsePattern(p))
	}
}

// IgnoreTopic reports whether topic is on the deny-list and must be dropped
// rather than buffered into a telemetry batch.
//
// OEM 0x2af3a0
func IgnoreTopic(topic string) bool {
	for i := range ignorePatterns {
		if ignorePatterns[i].Match(topic) {
			return true
		}
	}
	return false
}
