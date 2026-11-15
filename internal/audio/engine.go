//go:build darwin

// Package audio is a thin Go control layer over the C CoreAudio engine.
//
// Everything that touches a realtime audio thread lives in engine.c; this
// package only enumerates devices, configures the engine and polls meters.
package audio

/*
#cgo CFLAGS: -O2 -Wall -Wno-unused-parameter
#cgo LDFLAGS: -framework CoreAudio -framework AudioToolbox -framework AudioUnit -framework CoreFoundation
#include <stdlib.h>
#include "engine.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"strings"
	"unsafe"
)

// Device is one CoreAudio device.
type Device struct {
	Index           int
	ID              uint32
	Name            string
	UID             string
	InChannels      int
	OutChannels     int
	SampleRate      float64
	Transport       string
	IsDefaultInput  bool
	IsDefaultOutput bool
}

// HasInput reports whether the device can capture audio.
func (d Device) HasInput() bool { return d.InChannels > 0 }

// HasOutput reports whether the device can play audio.
func (d Device) HasOutput() bool { return d.OutChannels > 0 }

// Virtual reports whether the device is a virtual/aggregate device.
func (d Device) Virtual() bool {
	return d.Transport == "virt" || d.Transport == "agg " || d.Transport == "aggr"
}

func (d Device) String() string {
	return fmt.Sprintf("%s (%d in / %d out, %.0f Hz)", d.Name, d.InChannels, d.OutChannels, d.SampleRate)
}

// Devices returns every CoreAudio device in system order.
func Devices() ([]Device, error) {
	const max = 64
	cdevs := (*C.fm_device)(C.malloc(C.size_t(max) * C.size_t(unsafe.Sizeof(C.fm_device{}))))
	if cdevs == nil {
		return nil, errors.New("out of memory")
	}
	defer C.free(unsafe.Pointer(cdevs))

	n := int(C.fm_list_devices(cdevs, max))
	if n < 0 {
		return nil, fmt.Errorf("CoreAudio device enumeration failed (%d)", n)
	}
	raw := unsafe.Slice(cdevs, n)
	out := make([]Device, 0, n)
	for i := range raw {
		d := &raw[i]
		out = append(out, Device{
			Index:           int(d.index),
			ID:              uint32(d.id),
			Name:            C.GoString(&d.name[0]),
			UID:             C.GoString(&d.uid[0]),
			InChannels:      int(d.in_channels),
			OutChannels:     int(d.out_channels),
			SampleRate:      float64(d.sample_rate),
			Transport:       fourCC(uint32(d.transport)),
			IsDefaultInput:  d.is_default_input != 0,
			IsDefaultOutput: d.is_default_output != 0,
		})
	}
	return out, nil
}

func fourCC(v uint32) string {
	if v == 0 {
		return ""
	}
	b := []byte{byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)}
	for i := range b {
		if b[i] < 0x20 || b[i] > 0x7e {
			return ""
		}
	}
	return string(b)
}

// Inputs returns capture-capable devices.
func Inputs() ([]Device, error) {
	all, err := Devices()
	if err != nil {
		return nil, err
	}
	var out []Device
	for _, d := range all {
		if d.HasInput() {
			out = append(out, d)
		}
	}
	return out, nil
}

// Outputs returns playback-capable devices.
func Outputs() ([]Device, error) {
	all, err := Devices()
	if err != nil {
		return nil, err
	}
	var out []Device
	for _, d := range all {
		if d.HasOutput() {
			out = append(out, d)
		}
	}
	return out, nil
}

// DefaultInput returns the system default input device.
func DefaultInput() (Device, error) {
	all, err := Devices()
	if err != nil {
		return Device{}, err
	}
	for _, d := range all {
		if d.IsDefaultInput && d.HasInput() {
			return d, nil
		}
	}
	return Device{}, errors.New("no default input device")
}

// Resolve finds a device by UID, exact name, or unique name substring.
func Resolve(key string) (Device, error) {
	key = strings.TrimSpace(key)
	if key == "" {
		return Device{}, errors.New("empty device name")
	}
	all, err := Devices()
	if err != nil {
		return Device{}, err
	}
	for _, d := range all {
		if d.UID == key {
			return d, nil
		}
	}
	var exact []Device
	for _, d := range all {
		if strings.EqualFold(d.Name, key) {
			exact = append(exact, d)
		}
	}
	if len(exact) == 1 {
		return exact[0], nil
	}
	var partial []Device
	for _, d := range all {
		if strings.Contains(strings.ToLower(d.Name), strings.ToLower(key)) {
			partial = append(partial, d)
		}
	}
	if len(partial) == 1 {
		return partial[0], nil
	}
	if len(partial) > 1 {
		var names []string
		for _, d := range partial {
			names = append(names, d.Name)
		}
		return Device{}, fmt.Errorf("%q is ambiguous: %s", key, strings.Join(names, ", "))
	}
	return Device{}, fmt.Errorf("no audio device matching %q", key)
}

// ResolveInput is Resolve restricted to capture-capable devices.
func ResolveInput(key string) (Device, error) {
	d, err := Resolve(key)
	if err != nil {
		return Device{}, err
	}
	if !d.HasInput() {
		return Device{}, fmt.Errorf("%s has no input channels", d.Name)
	}
	return d, nil
}

// ResolveOutput is Resolve restricted to playback-capable devices.
func ResolveOutput(key string) (Device, error) {
	d, err := Resolve(key)
	if err != nil {
		return Device{}, err
	}
	if !d.HasOutput() {
		return Device{}, fmt.Errorf("%s cannot render audio (no output channels)", d.Name)
	}
	return d, nil
}

// TransportLabel is a human friendly name for the device transport.
func (d Device) TransportLabel() string {
	switch d.Transport {
	case "bltn":
		return "built-in"
	case "usb ":
		return "USB"
	case "virt":
		return "virtual"
	case "agg ":
		return "aggregate"
	case "ccwd":
		return "Continuity"
	case "ear ":
		return "receiver"
	case "spkr":
		return "speaker"
	case "hdmi":
		return "HDMI"
	case "dp  ":
		return "DisplayPort"
	case "blea":
		return "Bluetooth"
	case "":
		return "unknown"
	default:
		return strings.TrimSpace(d.Transport)
	}
}

func cString(s string) (*C.char, func()) {
	c := C.CString(s)
	return c, func() { C.free(unsafe.Pointer(c)) }
}

const uidMax = 256

// setUID copies a Go string into a fixed size C char buffer.
func setUID(dst *[uidMax]C.char, s string) {
	b := (*[uidMax]byte)(unsafe.Pointer(dst))
	for i := range b {
		b[i] = 0
	}
	copy(b[:uidMax-1], s)
}

// Source describes one capture input of the mixer.
type Source struct {
	Device   Device
	GainDB   float64
	Highpass bool
}

// Config is an engine configuration.
type Config struct {
	Sources    []Source
	Output     Device
	SampleRate float64
	TargetMS   float64
}

// Engine owns a running mixer.
type Engine struct {
	ptr      *C.fm_engine
	nsrc     int
	outName  string
	rate     float64
	started  bool
	closed   bool
	recorder bool
}

// NewEngine validates the configuration, opens every device and prepares the
// graph. It does not start audio; call Start for that.
func NewEngine(cfg Config) (*Engine, error) {
	if len(cfg.Sources) == 0 {
		return nil, errors.New("no input sources")
	}
	if len(cfg.Sources) > 4 {
		return nil, errors.New("at most 4 input sources are supported")
	}
	for _, s := range cfg.Sources {
		if !s.Device.HasInput() {
			return nil, fmt.Errorf("%s has no input channels", s.Device.Name)
		}
	}
	if !cfg.Output.HasOutput() {
		return nil, fmt.Errorf("%s cannot render audio", cfg.Output.Name)
	}

	var cc C.fm_config
	if cfg.SampleRate <= 0 {
		cfg.SampleRate = 48000
	}
	if cfg.TargetMS <= 0 {
		cfg.TargetMS = 30
	}
	cc.sample_rate = C.double(cfg.SampleRate)
	cc.target_ms = C.double(cfg.TargetMS)
	cc.buffer_frames = 512
	cc.num_sources = C.int(len(cfg.Sources))

	setUID(&cc.output_uid, cfg.Output.UID)
	for i, s := range cfg.Sources {
		setUID(&cc.sources[i].uid, s.Device.UID)
		cc.sources[i].gain_db = C.double(s.GainDB)
		if s.Highpass {
			cc.sources[i].highpass = 1
		}
	}

	var cerr [512]C.char
	e := C.fm_engine_create(&cc, &cerr[0], C.size_t(len(cerr)))
	if e == nil {
		return nil, errors.New(C.GoString(&cerr[0]))
	}
	return &Engine{
		ptr:     e,
		nsrc:    len(cfg.Sources),
		outName: cfg.Output.Name,
		rate:    cfg.SampleRate,
	}, nil
}

// OutputName is the device the mix is rendered to.
func (e *Engine) OutputName() string { return e.outName }

// SampleRate is the engine rate.
func (e *Engine) SampleRate() float64 { return e.rate }

// Start begins capture and rendering.
func (e *Engine) Start() error {
	if e.closed {
		return errors.New("engine closed")
	}
	if e.started {
		return nil
	}
	var cerr [512]C.char
	if rc := C.fm_engine_start(e.ptr, &cerr[0], C.size_t(len(cerr))); rc != 0 {
		return errors.New(C.GoString(&cerr[0]))
	}
	e.started = true
	return nil
}

// Stop halts audio but keeps the engine usable.
func (e *Engine) Stop() {
	if e.closed || !e.started {
		return
	}
	C.fm_engine_stop(e.ptr)
	e.started = false
}

// Close releases every device.
func (e *Engine) Close() {
	if e.closed {
		return
	}
	if e.started {
		C.fm_engine_stop(e.ptr)
	}
	C.fm_engine_destroy(e.ptr)
	e.closed = true
	e.started = false
	e.ptr = nil
}

// SetGainDB adjusts a source gain while running (click-free ramp).
func (e *Engine) SetGainDB(index int, db float64) {
	if e.closed || index < 0 || index >= e.nsrc {
		return
	}
	C.fm_engine_set_gain_db(e.ptr, C.int(index), C.double(db))
}

// StartRecording writes the mixed stereo float stream to a WAV file.
func (e *Engine) StartRecording(path string) error {
	cp, free := cString(path)
	defer free()
	C.fm_engine_set_record_path(e.ptr, cp)
	var cerr [512]C.char
	if rc := C.fm_engine_record_start(e.ptr, &cerr[0], C.size_t(len(cerr))); rc != 0 {
		return errors.New(C.GoString(&cerr[0]))
	}
	e.recorder = true
	return nil
}

// StopRecording flushes and closes the WAV file.
func (e *Engine) StopRecording() {
	if e.closed || !e.recorder {
		return
	}
	C.fm_engine_record_stop(e.ptr)
	e.recorder = false
}

// Stats is a snapshot of meters and health counters for one polling interval.
type Stats struct {
	PeakDB   []float64
	RMSDB    []float64
	FillMS   []float64
	DriftPPM []float64
	XRuns    []int64
	Drops    []int64
	Primed   []bool

	OutPeakDB float64
	OutRMSDB  float64
	Frames    int64
	Running   bool
}

// Stats reads and resets the per-interval meter accumulators.
func (e *Engine) Stats() Stats {
	st := Stats{
		PeakDB:   make([]float64, e.nsrc),
		RMSDB:    make([]float64, e.nsrc),
		FillMS:   make([]float64, e.nsrc),
		DriftPPM: make([]float64, e.nsrc),
		XRuns:    make([]int64, e.nsrc),
		Drops:    make([]int64, e.nsrc),
		Primed:   make([]bool, e.nsrc),
	}
	if e.closed {
		return st
	}
	var cs C.fm_stats
	C.fm_engine_get_stats(e.ptr, &cs)
	for i := 0; i < e.nsrc; i++ {
		st.PeakDB[i] = float64(cs.peak_db[i])
		st.RMSDB[i] = float64(cs.rms_db[i])
		st.FillMS[i] = float64(cs.fill_ms[i])
		st.DriftPPM[i] = float64(cs.drift_ppm[i])
		st.XRuns[i] = int64(cs.xruns[i])
		st.Drops[i] = int64(cs.drops[i])
		st.Primed[i] = cs.primed[i] != 0
	}
	st.OutPeakDB = float64(cs.out_peak_db)
	st.OutRMSDB = float64(cs.out_rms_db)
	st.Frames = int64(cs.frames)
	st.Running = cs.running != 0
	return st
}

// DeviceAlive reports whether the device is still present and alive.
func DeviceAlive(d Device) bool {
	return C.fm_device_alive(C.uint32_t(d.ID)) != 0
}

// Probe captures from a device for ms milliseconds and reports levels. It is
// used by `finalmic doctor` to verify permission and signal.
func Probe(d Device, ms int) (peakDB, rmsDB float64, frames int64, err error) {
	cuid, free := cString(d.UID)
	defer free()
	var cp, cr C.double
	var cf C.longlong
	var cerr [512]C.char
	rc := C.fm_probe_input(cuid, C.int(ms), &cp, &cr, &cf, &cerr[0], C.size_t(len(cerr)))
	if rc != 0 {
		return 0, 0, 0, errors.New(C.GoString(&cerr[0]))
	}
	return float64(cp), float64(cr), int64(cf), nil
}
