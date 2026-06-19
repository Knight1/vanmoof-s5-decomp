package shadow

import "encoding/json"

// State is an AWS Device Shadow state object as seen by the gateway. AWS returns
// up to three sections on a get/update response; the gateway reads the
// reported/desired/delta JSON opaquely.
//
// The wire shape is fixed by the OEM reflection-type metadata embedded in the
// binary:
//
//	struct {
//	  Desired  json.RawMessage "json:\"desired,omitempty\""
//	  Reported json.RawMessage "json:\"reported,omitempty\""
//	  Delta    json.RawMessage "json:\"delta,omitempty\""
//	}
type State struct {
	Desired  json.RawMessage
	Reported json.RawMessage
	Delta    json.RawMessage
}

// stateJSON is the on-the-wire form of State, with the OEM json tags.
type stateJSON struct {
	Desired  json.RawMessage `json:"desired,omitempty"`
	Reported json.RawMessage `json:"reported,omitempty"`
	Delta    json.RawMessage `json:"delta,omitempty"`
}

// MarshalJSON emits the desired/reported/delta form, omitting empty sections.
//
// The binary carries both shadow.State.MarshalJSON (this method) and the
// compiler-synthesized pointer wrapper shadow.(*State).MarshalJSON.
//
// OEM 0x279b90 (value), 0x27af00 (pointer wrapper)
func (s State) MarshalJSON() ([]byte, error) {
	return json.Marshal(stateJSON{Desired: s.Desired, Reported: s.Reported, Delta: s.Delta})
}

// UnmarshalJSON parses the desired/reported/delta form returned by AWS on a
// shadow get/update response.
//
// OEM 0x279d70
func (s *State) UnmarshalJSON(data []byte) error {
	var aux stateJSON
	if err := json.Unmarshal(data, &aux); err != nil {
		return err
	}
	s.Desired = aux.Desired
	s.Reported = aux.Reported
	s.Delta = aux.Delta
	return nil
}
