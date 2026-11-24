// Package creds stores the reusable Spotify credentials between CLI runs.
package creds

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

// Version is bumped when the on-disk shape changes incompatibly.
const Version = 1

// Credentials is the persisted login. Data is the stored-credentials blob
// returned by go-librespot's accesspoint (encoding/json stores []byte as
// base64).
type Credentials struct {
	Version    int       `json:"version"`
	DeviceID   string    `json:"device_id"`
	DeviceType int32     `json:"device_type"`
	Username   string    `json:"username"`
	Data       []byte    `json:"data"`
	SavedAt    time.Time `json:"saved_at"`
}

// Dir returns %APPDATA%\foo_librespot (os.UserConfigDir on Windows).
func Dir() (string, error) {
	base, err := os.UserConfigDir()
	if err != nil {
		return "", fmt.Errorf("resolving user config dir: %w", err)
	}
	return filepath.Join(base, "foo_librespot"), nil
}

// Path returns the credentials file path.
func Path() (string, error) {
	dir, err := Dir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "credentials.json"), nil
}

// Load reads the credentials file. A missing file returns os.ErrNotExist
// wrapped so callers can tell "not logged in" from a damaged file.
func Load() (*Credentials, error) {
	path, err := Path()
	if err != nil {
		return nil, err
	}

	raw, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, fmt.Errorf("no stored credentials at %s: %w", path, os.ErrNotExist)
		}
		return nil, fmt.Errorf("reading credentials: %w", err)
	}

	var c Credentials
	if err := json.Unmarshal(raw, &c); err != nil {
		return nil, fmt.Errorf("parsing %s: %w", path, err)
	}
	if c.Version != Version {
		return nil, fmt.Errorf("%s has version %d, expected %d", path, c.Version, Version)
	}
	if len(c.DeviceID) == 0 || c.Username == "" || len(c.Data) == 0 {
		return nil, fmt.Errorf("%s is incomplete; run login again", path)
	}
	return &c, nil
}

// Save writes the credentials atomically (temp file + rename) with owner-only
// permissions where the platform supports them.
func (c *Credentials) Save() error {
	c.Version = Version
	c.SavedAt = time.Now().UTC()

	dir, err := Dir()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return fmt.Errorf("creating %s: %w", dir, err)
	}

	raw, err := json.MarshalIndent(c, "", "  ")
	if err != nil {
		return fmt.Errorf("encoding credentials: %w", err)
	}

	tmp, err := os.CreateTemp(dir, "credentials-*.tmp")
	if err != nil {
		return fmt.Errorf("creating temp file: %w", err)
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)

	if _, err := tmp.Write(raw); err != nil {
		tmp.Close()
		return fmt.Errorf("writing credentials: %w", err)
	}
	if err := tmp.Close(); err != nil {
		return fmt.Errorf("closing credentials: %w", err)
	}
	_ = os.Chmod(tmpName, 0o600)

	path := filepath.Join(dir, "credentials.json")
	if err := os.Rename(tmpName, path); err != nil {
		return fmt.Errorf("replacing %s: %w", path, err)
	}
	return nil
}

// Remove deletes the credentials file (log out). A missing file is not an
// error.
func Remove() error {
	path, err := Path()
	if err != nil {
		return err
	}
	if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return nil
}

// NewDeviceID returns a fresh 20-byte Spotify device id as lowercase hex.
func NewDeviceID() (string, error) {
	b := make([]byte, 20)
	if _, err := rand.Read(b); err != nil {
		return "", fmt.Errorf("generating device id: %w", err)
	}
	return hex.EncodeToString(b), nil
}
