// Package shadow implements the AWS IoT Device Shadow client used by the gateway
// to exchange declarative config/state with the cloud.
//
// The gateway uses a *named* shadow (the classic-shadow variant with a name
// segment in the topic):
//
//	$aws/things/<thing>/shadow/name/<name>/get/{accepted,rejected}
//	$aws/things/<thing>/shadow/name/<name>/update/{accepted,rejected,delta}
//
// State is carried as opaque JSON under the shadow's reported / desired /
// delta sections. The Document type is a thin accessor over a single shadow
// document keyed by field name (built on tidwall gjson/sjson), tracking a
// per-field "$.metadata.<field>.timestamp" so callers can see when a field last
// changed.
//
// Reconstructed from the stripped go1.19 gateway binary (Ghidra program
// `gateway`, image base 0x10000). OEM symbol names come from the gopclntab.
// NOTE: in this go1.19 build several of these methods were folded by the Ghidra
// analyzer into one carved body (entry 0x278d90); each carries its own OEM entry
// address from the symbol table.
package shadow

import (
	"fmt"
	"time"

	"github.com/tidwall/gjson"
	"github.com/tidwall/sjson"
)

// Document is a parsed shadow document. It keeps the raw shadow state JSON plus
// the metadata JSON (which records, per field, the last-updated timestamp).
type Document struct {
	state    []byte // the shadow "state" object (reported/desired), raw JSON
	metadata []byte // the shadow "metadata" object, raw JSON
}

// timestampSuffix is appended to a metadata field path to read its update time:
// "<field>.timestamp" (AWS records metadata as {<field>:{timestamp:<epoch>}}).
const timestampSuffix = ".timestamp" // OEM literal @0x2d1dd0 (".timestamp")

// NewDocument builds a Document from raw shadow state + metadata JSON.
//
// OEM 0x... (shadow.NewDocument)
func NewDocument(state, metadata []byte) Document {
	return Document{state: state, metadata: metadata}
}

// Empty reports whether the document carries no state and no metadata.
//
// OEM 0x278cf0
func (d Document) Empty() bool {
	return len(d.state) == 0 && len(d.metadata) == 0
}

// JSON returns the raw shadow state JSON of the document.
//
// OEM 0x... (shadow.Document.JSON)
func (d Document) JSON() []byte { return d.state }

// Field is a single shadow field: its name, current JSON value, and the time it
// was last updated (from the shadow metadata).
type Field struct {
	Name      string
	Value     gjson.Result
	UpdatedAt time.Time
}

// String renders the field's value as a string.
//
// OEM 0x... (shadow.(*Field).String)
func (f *Field) String() string { return f.Value.String() }

// Field looks a field up by name in the document. It returns nil when the field
// is absent from the state. When present, the field's UpdatedAt is taken from
// the corresponding "<name>.timestamp" entry in the document metadata (a Unix
// timestamp), and the value from the state object.
//
// OEM 0x278720
func (d Document) Field(name string) *Field {
	value := gjson.GetBytes(d.state, name)
	if !value.Exists() {
		return nil
	}

	// Last-updated time from metadata "<name>.timestamp" (epoch seconds).
	var updatedAt time.Time
	if ts := gjson.GetBytes(d.metadata, name+timestampSuffix); ts.Exists() {
		updatedAt = time.Unix(ts.Int(), 0)
	}

	return &Field{Name: name, Value: value, UpdatedAt: updatedAt}
}

// SetField sets a field's value in the document state and stamps the matching
// "<name>.timestamp" metadata entry with the current time.
//
// OEM 0x2789c0
func (d *Document) SetField(name string, value interface{}) error {
	state, err := sjson.SetBytes(d.state, name, value)
	if err != nil {
		return fmt.Errorf("set state: %w", err) // @0x... ("set state: %w")
	}
	d.state = state

	metadata, err := sjson.SetBytes(d.metadata, name+timestampSuffix, time.Now().Unix())
	if err != nil {
		return fmt.Errorf("set meta: %w", err) // @0x... ("set meta: %w")
	}
	d.metadata = metadata
	return nil
}
