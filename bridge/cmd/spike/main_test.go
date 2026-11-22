package main

import (
	"errors"
	"testing"
)

func TestParseDumpArgsFlagsAfterPositionals(t *testing.T) {
	opts, verbose, err := parseDumpArgs([]string{
		"spotify:track:4uLU6hMCjMI75M1A2tKUQC", "out.wav",
		"--start-ms", "60000", "--seconds", "15.5", "--bitrate", "320", "-v",
	})
	if err != nil {
		t.Fatalf("parseDumpArgs: %v", err)
	}
	if !verbose {
		t.Error("verbose = false, want true")
	}
	if opts.Track != "spotify:track:4uLU6hMCjMI75M1A2tKUQC" {
		t.Errorf("Track = %q", opts.Track)
	}
	if opts.OutPath != "out.wav" {
		t.Errorf("OutPath = %q", opts.OutPath)
	}
	if opts.StartMs != 60000 || opts.Seconds != 15.5 || opts.Bitrate != 320 {
		t.Errorf("opts = %+v", opts)
	}
}

func TestParseDumpArgsDefaultsAndEqualsForm(t *testing.T) {
	opts, verbose, err := parseDumpArgs([]string{"--bitrate=96", "abc", "out.wav", "--start-ms=0"})
	if err != nil {
		t.Fatalf("parseDumpArgs: %v", err)
	}
	if verbose || opts.Bitrate != 96 || opts.StartMs != 0 || opts.Seconds != 0 {
		t.Errorf("opts = %+v verbose=%v", opts, verbose)
	}
}

func TestParseDumpArgsErrors(t *testing.T) {
	if _, _, err := parseDumpArgs([]string{"a"}); err == nil {
		t.Error("expected error for one positional")
	}
	if _, _, err := parseDumpArgs([]string{"a", "b", "--bitrate", "128"}); err == nil {
		t.Error("expected error for unsupported bitrate")
	}
	if _, _, err := parseDumpArgs([]string{"a", "b", "--seconds", "-1"}); err == nil {
		t.Error("expected error for negative seconds")
	}
	if _, _, err := parseDumpArgs([]string{"a", "b", "--start-ms"}); err == nil {
		t.Error("expected error for missing value")
	}
	if _, _, err := parseDumpArgs([]string{"a", "b", "--bogus"}); err == nil {
		t.Error("expected error for unknown flag")
	}
	if _, _, err := parseDumpArgs([]string{"--help"}); !errors.Is(err, errHelp) {
		t.Errorf("err = %v, want errHelp", err)
	}
}
