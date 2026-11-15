// Package config stores the user's device selection between runs.
package config

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

// Roles help the UI label a source.
const (
	RoleMic     = "mic"
	RoleCapture = "capture"
	RoleOther   = "other"
)

// Source is one capture input.
type Source struct {
	UID      string  `json:"uid"`
	Name     string  `json:"name"`
	GainDB   float64 `json:"gain_db"`
	Highpass bool    `json:"highpass"`
	Role     string  `json:"role"`
}

// Config is the persisted mixer setup.
type Config struct {
	Version    int      `json:"version"`
	Sources    []Source `json:"sources"`
	OutputUID  string   `json:"output_uid"`
	OutputName string   `json:"output_name"`
	SampleRate float64  `json:"sample_rate"`
	TargetMS   float64  `json:"target_ms"`
}

// Path is the config file location.
func Path() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "finalmic", "config.json"), nil
}

// Load reads the config, returning os.ErrNotExist when it has not been created.
func Load() (*Config, error) {
	p, err := Path()
	if err != nil {
		return nil, err
	}
	data, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	var c Config
	if err := json.Unmarshal(data, &c); err != nil {
		return nil, fmt.Errorf("配置文件损坏 (%s): %w", p, err)
	}
	return &c, nil
}

// Save writes the config atomically.
func Save(c *Config) error {
	p, err := Path()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	if c.Version == 0 {
		c.Version = 1
	}
	if c.SampleRate == 0 {
		c.SampleRate = 48000
	}
	if c.TargetMS == 0 {
		c.TargetMS = 30
	}
	data, err := json.MarshalIndent(c, "", "  ")
	if err != nil {
		return err
	}
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, append(data, '\n'), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, p)
}

// Validate checks the obvious invariants.
func (c *Config) Validate() error {
	if len(c.Sources) == 0 {
		return errors.New("配置里没有输入设备")
	}
	if c.OutputUID == "" && c.OutputName == "" {
		return errors.New("配置里没有输出设备")
	}
	return nil
}

// RoleLabel is a short Chinese label for a role.
func RoleLabel(role string) string {
	switch role {
	case RoleMic:
		return "麦克风"
	case RoleCapture:
		return "采集卡"
	default:
		return "输入"
	}
}
