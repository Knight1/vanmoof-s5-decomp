// Package ca provides the pinned AWS IoT server trust anchor: a custom
// x509.CertPool built from ONLY the two Amazon root CAs embedded in the binary.
// The gateway hands this pool to its tls.Config instead of using the system
// trust store, so the AWS IoT server certificate must chain to one of these two
// roots — trusting any other CA is impossible (root/CA pinning).
//
// OEM source: internal/iot/ca/pool.go
package ca

import "crypto/x509"

// The two Amazon roots are embedded verbatim in the image:
//
//	amazonRootCA1 — RSA 2048,    "Amazon Root CA 1"   (OEM rodata @ 0x663c80)
//	amazonRootCA3 — ECDSA P-256, "Amazon Root CA 3"   (OEM rodata @ 0x65d860)
//
// These are Amazon's published public roots; they are the fixed trust anchors
// the device pins. The full PEM bodies (the exact bytes live at the addresses
// above) are not transcribed here — they are the standard Amazon Root CA 1 / 3
// certificates.
const (
	amazonRootCA1 = `-----BEGIN CERTIFICATE-----
... Amazon Root CA 1 (RSA 2048) — OEM @0x663c80 ...
-----END CERTIFICATE-----`

	amazonRootCA3 = `-----BEGIN CERTIFICATE-----
... Amazon Root CA 3 (ECDSA P-256) — OEM @0x65d860 ...
-----END CERTIFICATE-----`
)

// CertPool returns a fresh pool containing only the two Amazon roots. It panics
// if either PEM fails to append — that would be a build/provisioning fault that
// must never ship, so failing hard is correct.
//
// OEM 0x1ca560
func CertPool() *x509.CertPool {
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM([]byte(amazonRootCA1)) {
		panic("ca: failed to append Amazon Root CA 1")
	}
	if !pool.AppendCertsFromPEM([]byte(amazonRootCA3)) {
		panic("ca: failed to append Amazon Root CA 3")
	}
	return pool
}
