// Package logging provides the small logger go-librespot expects.
package logging

import (
	"fmt"
	"io"
	"strings"
	"sync"

	librespot "github.com/devgianlu/go-librespot"
)

// Logger writes leveled lines to a single sink. Fields added through WithField
// and WithError are prefixed to every line.
type Logger struct {
	mu     sync.Mutex
	out    io.Writer
	debug  bool
	fields []string
}

// New returns a logger. With debug enabled, go-librespot's Debugf lines are
// printed too; trace lines are always dropped.
func New(out io.Writer, debug bool) *Logger {
	return &Logger{out: out, debug: debug}
}

func (l *Logger) logf(level, format string, args ...interface{}) {
	if level == "TRACE" || (level == "DEBUG" && !l.debug) {
		return
	}

	msg := fmt.Sprintf(format, args...)
	prefix := ""
	if len(l.fields) > 0 {
		prefix = "[" + strings.Join(l.fields, " ") + "] "
	}

	l.mu.Lock()
	defer l.mu.Unlock()
	fmt.Fprintf(l.out, "%s: %s%s\n", level, prefix, msg)
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

func (l *Logger) with(field string) *Logger {
	fields := make([]string, len(l.fields), len(l.fields)+1)
	copy(fields, l.fields)
	fields = append(fields, field)
	return &Logger{out: l.out, debug: l.debug, fields: fields}
}
