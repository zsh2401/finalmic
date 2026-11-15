package ui

import (
	"strings"
	"testing"
)

func TestBarWidth(t *testing.T) {
	r := &Renderer{}
	for _, db := range []float64{-120, -60, -30, -6, 0, 6} {
		bar := r.Bar(db, 10)
		if got := len([]rune(bar)); got != 10 {
			t.Errorf("Bar(%v) has %d cells, want 10", db, got)
		}
	}
	if got := r.Bar(-120, 6); got != "░░░░░░" {
		t.Errorf("silence bar = %q", got)
	}
	if got := r.Bar(0, 4); !strings.HasPrefix(got, "████") {
		t.Errorf("full bar = %q", got)
	}
}

func TestShort(t *testing.T) {
	if got := Short("MacBook Pro Microphone", 10); got != "MacBook P…" {
		t.Errorf("Short = %q", got)
	}
	if got := Short("短", 10); got != "短" {
		t.Errorf("Short short = %q", got)
	}
}

func TestDB(t *testing.T) {
	if got := DB(-120); !strings.Contains(got, "∞") {
		t.Errorf("DB(-120) = %q", got)
	}
	if got := DB(-18.25); !strings.Contains(got, "-18.2") {
		t.Errorf("DB(-18.25) = %q", got)
	}
}
