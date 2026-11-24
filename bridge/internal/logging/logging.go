// Package logging provides the small logger go-librespot expects.
package logging

import (
	"fmt"
	"io"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	librespot "github.com/devgianlu/go-librespot"
)

// Logger writes leveled lines to a single sink. Fields added through WithField
// and WithError are prefixed to every line, after the logger's own prefix.
//
// Every line starts with a millisecond timestamp, and the bridge prefixes its
// lines with its process id: several bridges (and several generations of one)
// append to the same log file, so a line without both is impossible to place.
type Logger struct {
	mu     *sync.Mutex
	out    io.Writer
	debug  *atomic.Bool // shared by every logger derived from the same New
	prefix string
	fields []string
}

// New returns a logger. With debug enabled, go-librespot's Debugf lines are
// printed too; trace lines are always dropped.
func New(out io.Writer, debug bool) *Logger {
	l := &Logger{mu: &sync.Mutex{}, out: out, debug: &atomic.Bool{}}
	l.debug.Store(debug)
	return l
}

// SetDebug switches debug output on or off for this logger and every logger
// derived from it (WithPrefix, WithField, WithError), while running.
func (l *Logger) SetDebug(debug bool) { l.debug.Store(debug) }

// DebugEnabled reports whether debug lines are written.
func (l *Logger) DebugEnabled() bool { return l.debug.Load() }

func (l *Logger) logf(level, format string, args ...interface{}) {
	if level == "TRACE" || (level == "DEBUG" && !l.debug.Load()) {
		return
	}

	msg := fmt.Sprintf(format, args...)
	prefix := ""
	if l.prefix != "" {
		prefix = "[" + l.prefix + "] "
	}
	if len(l.fields) > 0 {
		prefix += "[" + strings.Join(l.fields, " ") + "] "
	}

	l.mu.Lock()
	defer l.mu.Unlock()
	fmt.Fprintf(l.out, "%s %s: %s%s\n", time.Now().Format("2006-01-02 15:04:05.000"), level, prefix, msg)
}

func (l *Logger) Tracef(format string, args ...interface{}) { l.logf("TRACE", format, args...) }
func (l *Logger) Debugf(format string, args ...interface{}) { l.logf("DEBUG", format, args...) }
func (l *Logger) Infof(format string, args ...interface{})  { l.logf("INFO", format, args...) }
func (l *Logger) Warnf(format string, args ...interface{})  { l.logf("WARN", format, args...) }
func (l *Logger) Errorf(format string, args ...interface{}) { l.logf("ERROR", format, args...) }

func (l *Logger) Trace(args ...interface{}) { l.logf("TRACE", "%s", fmt.Sprint(args...)) }
func (l *Logger) Debug(args ...interface{}) { l.logf("DEBUG", "%s", fmt.Sprint(args...)) }
func (l *Logger) Info(args ...interface{})  { l.logf("INFO", "%s", fmt.Sprint(args...)) }
func (l *Logger) Warn(args ...interface{})  { l.logf("WARN", "%s", fmt.Sprint(args...)) }
func (l *Logger) Error(args ...interface{}) { l.logf("ERROR", "%s", fmt.Sprint(args...)) }

func (l *Logger) WithField(key string, value interface{}) librespot.Logger {
	return l.with(fmt.Sprintf("%s=%v", key, value))
}

func (l *Logger) WithError(err error) librespot.Logger {
	if err == nil {
		return l
	}
	return l.with("err=" + err.Error())
}

// WithPrefix returns a logger that puts "prefix" in front of every line, so
// lines from different processes writing to one file stay distinguishable.
func (l *Logger) WithPrefix(prefix string) *Logger {
	return &Logger{mu: l.mu, out: l.out, debug: l.debug, prefix: prefix, fields: l.fields}
}

func (l *Logger) with(field string) *Logger {
	fields := make([]string, len(l.fields), len(l.fields)+1)
	copy(fields, l.fields)
	fields = append(fields, field)
	return &Logger{mu: l.mu, out: l.out, debug: l.debug, prefix: l.prefix, fields: fields}
}
