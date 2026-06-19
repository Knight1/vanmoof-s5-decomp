package ble

// CBOR encoding shim. In the OEM binary the proxy wraps the marshalled batch in
// a CBOR envelope before publishing; this models github.com/fxamacker/cbor and
// is not reconstructed.

import "github.com/fxamacker/cbor/v2"

func cborMarshal(v any) ([]byte, error) {
	return cbor.Marshal(v)
}
