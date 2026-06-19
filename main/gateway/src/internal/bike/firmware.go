package bike

import "bytes"

// fileFirmwareVersion is the rootfs path (relative to the fs.FS root) of the
// firmware version stamp written by the image build.
const fileFirmwareVersion = "etc/firmware_version"

// FirmwareVersion reads etc/firmware_version from fsys and returns it with
// surrounding whitespace trimmed. The value is reported to the cloud via the
// telemetry collector (Collector.SetFirmware).
//
// OEM 0x1c69b0
func FirmwareVersion(fsys interface {
	ReadFile(name string) ([]byte, error)
}) (string, error) {
	data, err := fsys.ReadFile(fileFirmwareVersion)
	if err != nil {
		return "", err
	}
	return string(bytes.TrimSpace(data)), nil
}
