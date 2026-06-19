// Package bike loads the per-device cloud identity and firmware metadata that
// VanMoof provisions onto the persistent eMMC config partition.
//
// Everything the gateway needs to reach AWS IoT Core as a particular bike —
// the device serial, the bike id, the mTLS client certificate + key and the
// IoT endpoint — is written to /run/media/mmcblk2p6/ during manufacturing.
// None of it is baked into the binary.
package bike

import (
	"fmt"
	"io/fs"

	"github.com/peterbourgon/ff/v3"
)

// mmcblk2p6 is the eMMC config partition mount where provisioning lives. The
// five files below are read relative to this filesystem root.
//
// /run/media/mmcblk2p6/
//
//	serial          device serial number
//	bike_id         bike identifier (kept in sync with the cloud)
//	config.cfg      peterbourgon/ff key/value file holding the IoT endpoint
//	certificate.pem mTLS client certificate presented to AWS IoT
//	private.key     mTLS client private key
const (
	fileSerial      = "serial"
	fileBikeID      = "bike_id"
	fileConfig      = "config.cfg"
	fileCertificate = "certificate.pem"
	filePrivateKey  = "private.key"
)

// ProvisioningData is the device's complete cloud identity, assembled from the
// files on mmcblk2p6. It is consumed by gateway.New, which feeds Endpoint +
// Certificate/PrivateKey into the TLS config (see iot.NewClient) and uses
// Serial / BikeID for telemetry and the bike-id shadow sync.
type ProvisioningData struct {
	Serial      string // contents of serial
	BikeID      string // contents of bike_id
	Endpoint    string // external-endpoint / iot_endpoint from config.cfg
	Certificate []byte // contents of certificate.pem (PEM)
	PrivateKey  []byte // contents of private.key (PEM)
}

// ProvisioningData reads the five provisioning files from fsys (the mmcblk2p6
// filesystem) and returns the assembled identity. Any read failure is wrapped
// with the name of the field that could not be loaded.
//
// OEM 0x1c6bb0  (provisioning_load_from_mmcblk2p6)
func ProvisioningData(fsys fs.FS) (ProvisioningData, error) {
	serial, err := readFile(fsys, fileSerial)
	if err != nil {
		return ProvisioningData{}, fmt.Errorf("read serial: %w", err)
	}

	bikeID, err := readFile(fsys, fileBikeID)
	if err != nil {
		return ProvisioningData{}, fmt.Errorf("open provisioning data: %w", err)
	}

	endpoint, err := readEndpoint(fsys, fileConfig)
	if err != nil {
		return ProvisioningData{}, fmt.Errorf("read endpoint: %w", err)
	}

	cert, err := readFile(fsys, fileCertificate)
	if err != nil {
		return ProvisioningData{}, fmt.Errorf("read certificate: %w", err)
	}

	key, err := readFile(fsys, filePrivateKey)
	if err != nil {
		return ProvisioningData{}, fmt.Errorf("read private key: %w", err)
	}

	return ProvisioningData{
		Serial:      string(serial),
		BikeID:      string(bikeID),
		Endpoint:    endpoint,
		Certificate: cert,
		PrivateKey:  key,
	}, nil
}

// Config-file keys. config.cfg is a plain `key value` file parsed by
// peterbourgon/ff; the IoT endpoint may be given under either name. (The same
// file also carries the transport-preference flags consumed elsewhere:
// prefer-modem / prefer-proxy / auto-disconnect.)
const (
	keyExternalEndpoint = "external-endpoint"
	keyIotEndpoint      = "iot_endpoint"
)

// readEndpoint opens config.cfg from fsys, parses it as a peterbourgon/ff
// flag file and returns the AWS IoT endpoint. It is an error for the endpoint
// flag to be absent.
//
// OEM 0x1c70d0  (config_cfg_parse / readEndpoint)
func readEndpoint(fsys fs.FS, name string) (string, error) {
	if _, err := fsys.Open(name); err != nil {
		return "", fmt.Errorf("open config: %w", err)
	}

	var endpoint string
	flags := ff.NewFlagSet()
	flags.StringVar(&endpoint, keyExternalEndpoint, "", "AWS IoT endpoint")

	if err := ff.Parse(flags, nil,
		ff.WithConfigFile(name),
		ff.WithConfigFileParser(ff.PlainParser),
	); err != nil {
		return "", fmt.Errorf("parse endpoint: %w", err)
	}

	if endpoint == "" {
		return "", fmt.Errorf("%s: not found", keyExternalEndpoint)
	}
	return endpoint, nil
}

// readFile reads the whole named file from fsys.
//
// OEM 0x1c7400
func readFile(fsys fs.FS, name string) ([]byte, error) {
	return fs.ReadFile(fsys, name)
}
