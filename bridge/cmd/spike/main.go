// Command spike is the Phase 0 proof-of-concept: it logs in to Spotify with
// go-librespot used as a library and writes one decoded track to a WAV file
// without the Connect state machine or an audio output device.
package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"

	"foo_librespot/bridge/internal/creds"
	"foo_librespot/bridge/internal/logging"
	"foo_librespot/bridge/internal/spike"
	"foo_librespot/bridge/internal/spotifyx"
)

const usage = `Usage:
  spike login
  spike dump <spotify:track:ID | open.spotify.com/track/ID | ID> <out.wav> [flags]

Commands:
  login   Authenticate with the Spotify device-authorization flow and save
          reusable credentials to %APPDATA%\foo_librespot\credentials.json.
  dump    Fetch and decode one track to 16-bit PCM WAV (44100 Hz stereo).

Dump flags:
  --start-ms N        Start decoding at N milliseconds (default 0).
  --seconds S         Decode at most S seconds (default: whole track).
  --bitrate N         Preferred bitrate: 96, 160 or 320 kbps (default 160).
  -v, --verbose       Print go-librespot debug logs.

Examples:
  spike login
  spike dump spotify:track:4uLU6hMCjMI75M1A2tKUQC out.wav
  spike dump https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC out.wav --start-ms 60000 --seconds 15 --bitrate 320
`

func main() {
	os.Exit(run(os.Args[1:]))
}

func run(args []string) int {
	if len(args) == 0 {
		fmt.Fprint(os.Stderr, usage)
		return 2
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	switch args[0] {
	case "help", "-h", "--help":
		fmt.Print(usage)
		return 0
	case "login":
		if len(args) > 1 && isHelp(args[1]) {
			fmt.Print(usage)
			return 0
		}
		if len(args) > 1 {
			fmt.Fprintf(os.Stderr, "spike: login takes no arguments\n")
			return 2
		}
		log := logging.New(os.Stderr, false)
		if _, err := spotifyx.Login(ctx, log, os.Stdout); err != nil {
			fmt.Fprintf(os.Stderr, "spike: login failed: %v\n", err)
			return 1
		}
		return 0
	case "dump":
		opts, verbose, err := parseDumpArgs(args[1:])
		if errors.Is(err, errHelp) {
			fmt.Print(usage)
			return 0
		}
		if err != nil {
			fmt.Fprintf(os.Stderr, "spike: %v\n\n%s", err, usage)
			return 2
		}

		log := logging.New(os.Stderr, verbose)
		c, err := creds.Load()
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				fmt.Fprintf(os.Stderr, "spike: not logged in (%v)\nspike: run \"spike login\" first\n", err)
			} else {
				fmt.Fprintf(os.Stderr, "spike: %v\n", err)
			}
			return 1
		}

		if _, err := spike.Dump(ctx, log, c, opts); err != nil {
			if errors.Is(err, context.Canceled) {
				fmt.Fprintln(os.Stderr, "spike: cancelled")
				return 130
			}
			fmt.Fprintf(os.Stderr, "spike: dump failed: %v\n", err)
			return 1
		}
		return 0
	default:
		fmt.Fprintf(os.Stderr, "spike: unknown command %q\n\n", args[0])
		fmt.Fprint(os.Stderr, usage)
		return 2
	}
}

func isHelp(s string) bool {
	return s == "-h" || s == "--help" || s == "help"
}

var errHelp = errors.New("help requested")

// parseDumpArgs accepts flags before or after the two positionals, since users
// naturally write "dump <track> <out.wav> --start-ms 5000".
func parseDumpArgs(args []string) (spike.DumpOptions, bool, error) {
	opts := spike.DumpOptions{Bitrate: 160}
	verbose := false
	var positional []string

	for i := 0; i < len(args); i++ {
		arg := args[i]
		if isHelp(arg) && len(positional) == 0 {
			return opts, verbose, errHelp
		}
		if !strings.HasPrefix(arg, "-") || arg == "-" {
			positional = append(positional, arg)
			continue
		}

		name, inline, hasInline := strings.Cut(arg, "=")
		value := func() (string, error) {
			if hasInline {
				if inline == "" {
					return "", fmt.Errorf("%s requires a value", name)
				}
				return inline, nil
			}
			if i+1 >= len(args) {
				return "", fmt.Errorf("%s requires a value", name)
			}
			i++
			return args[i], nil
		}

		switch name {
		case "-v", "--verbose":
			verbose = true
		case "--start-ms":
			v, err := value()
			if err != nil {
				return opts, verbose, err
			}
			n, err := strconv.ParseInt(v, 10, 64)
			if err != nil || n < 0 {
				return opts, verbose, fmt.Errorf("--start-ms must be a non-negative integer, got %q", v)
			}
			opts.StartMs = n
		case "--seconds":
			v, err := value()
			if err != nil {
				return opts, verbose, err
			}
			f, err := strconv.ParseFloat(v, 64)
			if err != nil || f < 0 {
				return opts, verbose, fmt.Errorf("--seconds must be a non-negative number, got %q", v)
			}
			opts.Seconds = f
		case "--bitrate":
			v, err := value()
			if err != nil {
				return opts, verbose, err
			}
			n, err := strconv.Atoi(v)
			if err != nil || (n != 96 && n != 160 && n != 320) {
				return opts, verbose, fmt.Errorf("--bitrate must be 96, 160 or 320, got %q", v)
			}
			opts.Bitrate = n
		default:
			return opts, verbose, fmt.Errorf("unknown flag %q", arg)
		}
	}

	if len(positional) != 2 {
		return opts, verbose, fmt.Errorf("dump needs exactly two arguments: <track> <out.wav> (got %d)", len(positional))
	}
	opts.Track = positional[0]
	opts.OutPath = positional[1]
	return opts, verbose, nil
}
