// Package ui renders the live terminal meter.
package ui

import (
	"fmt"
	"io"
	"os"
	"strings"
	"unicode/utf8"
)

const (
	dim    = "\x1b[2m"
	red    = "\x1b[31m"
	green  = "\x1b[32m"
	yellow = "\x1b[33m"
	cyan   = "\x1b[36m"
	bold   = "\x1b[1m"
	reset  = "\x1b[0m"
)

// IsTTY reports whether f is an interactive terminal.
func IsTTY(f *os.File) bool {
	st, err := f.Stat()
	if err != nil {
		return false
	}
	return st.Mode()&os.ModeCharDevice != 0
}

// Renderer repaints a fixed block of lines in place.
type Renderer struct {
	out     io.Writer
	started bool
	lines   int
	Color   bool
}

// NewRenderer creates a renderer writing to out.
func NewRenderer(out io.Writer, color bool) *Renderer {
	return &Renderer{out: out, Color: color}
}

func (r *Renderer) paint(code, s string) string {
	if !r.Color {
		return s
	}
	return code + s + reset
}

// Draw replaces the previously drawn block with lines.
func (r *Renderer) Draw(lines []string) {
	var b strings.Builder
	if r.started {
		fmt.Fprintf(&b, "\x1b[%dA", r.lines)
	}
	for _, ln := range lines {
		b.WriteString("\x1b[2K")
		b.WriteString(ln)
		b.WriteString("\n")
	}
	if len(lines) > r.lines {
		r.lines = len(lines)
	}
	r.started = true
	io.WriteString(r.out, b.String())
}

// Finish leaves the cursor below the block.
func (r *Renderer) Finish() {
	r.started = false
	r.lines = 0
}

// Bar renders a peak meter of width cells for a dBFS value.
func (r *Renderer) Bar(db float64, width int) string {
	const floor = -60.0
	frac := (db - floor) / (0 - floor)
	if frac < 0 {
		frac = 0
	}
	if frac > 1 {
		frac = 1
	}
	filled := int(frac*float64(width) + 0.5)
	if db <= floor {
		filled = 0
	}
	color := green
	if db > -3 {
		color = red
	} else if db > -9 {
		color = yellow
	}
	bar := strings.Repeat("█", filled) + strings.Repeat("░", width-filled)
	if r.Color {
		if filled > 0 {
			return color + strings.Repeat("█", filled) + reset + dim +
				strings.Repeat("░", width-filled) + reset
		}
		return dim + bar + reset
	}
	return bar
}

// Short truncates s to at most n display cells (CJK counts as 2), using an
// ellipsis which is included in the budget.
func Short(s string, n int) string {
	if n <= 0 {
		return ""
	}
	w := 0
	for _, r := range s {
		w += RuneWidth(r)
	}
	if w <= n {
		return s
	}
	var b strings.Builder
	w = 0
	for _, r := range s {
		rw := RuneWidth(r)
		if w+rw > n-1 {
			return b.String() + "…"
		}
		b.WriteRune(r)
		w += rw
	}
	return b.String()
}

// Pad right-pads s to n display cells.
func Pad(s string, n int) string {
	w := 0
	for _, r := range s {
		w += RuneWidth(r)
	}
	if w >= n {
		return s
	}
	return s + strings.Repeat(" ", n-w)
}

// RuneWidth approximates the display width of r.
func RuneWidth(r rune) int {
	if r < 0x20 {
		return 0
	}
	if r < 0x7f {
		return 1
	}
	if r >= 0x1100 && (r <= 0x115f || r == 0x2329 || r == 0x232a ||
		(r >= 0x2e80 && r <= 0xa4cf) || (r >= 0xac00 && r <= 0xd7a3) ||
		(r >= 0xf900 && r <= 0xfaff) || (r >= 0xfe10 && r <= 0xfe19) ||
		(r >= 0xfe30 && r <= 0xfe6f) || (r >= 0xff00 && r <= 0xff60) ||
		(r >= 0xffe0 && r <= 0xffe6) || (r >= 0x1f300 && r <= 0x1f64f) ||
		(r >= 0x1f900 && r <= 0x1f9ff)) {
		return 2
	}
	return 1
}

// DB renders a dBFS value with a fixed width.
func DB(db float64) string {
	if db <= -119 {
		return "  -∞ dBFS"
	}
	return fmt.Sprintf("%6.1f dBFS", db)
}

// Label styles a small dim caption.
func (r *Renderer) Label(s string) string { return r.paint(dim, s) }
func (r *Renderer) Title(s string) string { return r.paint(bold+cyan, s) }
func (r *Renderer) Note(s string) string  { return r.paint(dim, s) }
func (r *Renderer) Warn(s string) string  { return r.paint(yellow, s) }

// Line builds a separator.
func (r *Renderer) Line(width int) string {
	return r.paint(dim, strings.Repeat("─", width))
}

// RuneLen counts runes; utility for tests.
func RuneLen(s string) int { return utf8.RuneCountInString(s) }
