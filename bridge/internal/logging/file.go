package logging

import (
	"fmt"
	"os"
	"path/filepath"
	"sync"
)

// File is an append-only log file that keeps itself bounded: when a write would
// push it past MaxBytes it is rotated once to "<path>.1" (replacing the previous
// rotation) and started again.
type File struct {
	mu   sync.Mutex
	path string
	max  int64
	f    *os.File
	size int64
}

// OpenFile opens path for appending, creating its directory. maxBytes is the
// size at which the file is rotated; zero means 5 MiB.
func OpenFile(path string, maxBytes int64) (*File, error) {
	if maxBytes <= 0 {
		maxBytes = 5 << 20
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, fmt.Errorf("creating log directory: %w", err)
	}

	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
	if err != nil {
		return nil, fmt.Errorf("opening log file: %w", err)
	}
	var size int64
	if info, err := f.Stat(); err == nil {
		size = info.Size()
	}
	w := &File{path: path, max: maxBytes, f: f, size: size}
	if size >= maxBytes {
		if err := w.rotate(); err != nil {
			f.Close()
			return nil, err
		}
	}
	return w, nil
}

func (w *File) Write(p []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()

	if w.f == nil {
		return 0, os.ErrClosed
	}
	if w.size+int64(len(p)) > w.max {
		if err := w.rotateLocked(); err != nil {
			return 0, err
		}
	}
	n, err := w.f.Write(p)
	w.size += int64(n)
	return n, err
}

// Close closes the file. It is safe to call more than once.
func (w *File) Close() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.f == nil {
		return nil
	}
	err := w.f.Close()
	w.f = nil
	return err
}

func (w *File) rotate() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.rotateLocked()
}

func (w *File) rotateLocked() error {
	if w.f != nil {
		_ = w.f.Close()
		w.f = nil
	}
	_ = os.Remove(w.path + ".1")
	if err := os.Rename(w.path, w.path+".1"); err != nil && !os.IsNotExist(err) {
		// Reopen the original so logging keeps working.
		f, openErr := os.OpenFile(w.path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
		if openErr == nil {
			w.f = f
			if info, statErr := f.Stat(); statErr == nil {
				w.size = info.Size()
			}
		}
		return fmt.Errorf("rotating log file: %w", err)
	}

	f, err := os.OpenFile(w.path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
	if err != nil {
		return fmt.Errorf("reopening log file: %w", err)
	}
	w.f = f
	w.size = 0
	return nil
}
