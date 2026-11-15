package cli

import "testing"

func TestParseInputSpec(t *testing.T) {
	cases := []struct {
		in   string
		name string
		gain float64
		ok   bool
	}{
		{"MacBook Pro Microphone", "MacBook Pro Microphone", 0, false},
		{"UGREEN 25173@-6", "UGREEN 25173", -6, true},
		{"UGREEN 25173@-6dB", "UGREEN 25173", -6, true},
		{"UGREEN 25173@3.5db", "UGREEN 25173", 3.5, true},
		{"Strange@Name", "Strange@Name", 0, false},
		{"@-3", "@-3", 0, false},
	}
	for _, c := range cases {
		name, gain, ok := parseInputSpec(c.in)
		if name != c.name || gain != c.gain || ok != c.ok {
			t.Errorf("parseInputSpec(%q) = (%q, %v, %v), want (%q, %v, %v)",
				c.in, name, gain, ok, c.name, c.gain, c.ok)
		}
	}
}

func TestComma(t *testing.T) {
	if got := comma(1234567); got != "1,234,567" {
		t.Errorf("comma = %q", got)
	}
	if got := comma(0); got != "0" {
		t.Errorf("comma(0) = %q", got)
	}
}

func TestFit(t *testing.T) {
	if got := fit("abc", 5); got != "abc  " {
		t.Errorf("fit = %q", got)
	}
	if got := fit("abcdef", 4); got != "abc…" {
		t.Errorf("fit = %q", got)
	}
}
