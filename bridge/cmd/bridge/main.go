// Command bridge serves decoded Spotify audio to the foo_librespot foobar2000
// component over a Windows named pipe. The protocol is proto/PROTOCOL.md.
package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"github.com/Microsoft/go-winio"
	"golang.org/x/sys/windows"

	"foo_librespot/bridge/internal/bridge"
	"foo_librespot/bridge/internal/creds"
	"foo_librespot/bridge/internal/logging"
	"foo_librespot/bridge/internal/spotifyx"
)

const usage = `Usage:
  librespot-bridge.exe [--pipe <name>] [--parent-pid <pid>] [--debug]
  librespot-bridge.exe login

Commands:
  login   Authenticate with the Spotify device-authorization flow and save
          reusable credentials to %APPDATA%\foo_librespot\credentials.json.

Flags:
  --pipe <name>        Named pipe to serve on (default foo_librespot).
  --parent-pid <pid>   Exit when the given process exits.
  --debug              Log debug output to %APPDATA%\foo_librespot\bridge.log.
  --version            Print the version and exit.
`

func main() {
	os.Exit(run(os.Args[1:]))
}

func run(args []string) int {
	if len(args) > 0 {
		switch args[0] {
		case "login":
			return runLogin(args[1:])
		case "help", "-h", "--help":
			fmt.Print(usage)
			return 0
		}
	}

	fs := flag.NewFlagSet("librespot-bridge", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	pipeName := fs.String("pipe", "foo_librespot", "named pipe to serve on")
	parentPID := fs.Int("parent-pid", 0, "exit when this process id exits")
	debug := fs.Bool("debug", false, "log debug output")
	version := fs.Bool("version", false, "print the version and exit")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if *version {
		fmt.Printf("librespot-bridge %s\n", bridge.Version)
		return 0
	}
	if fs.NArg() > 0 {
		fmt.Fprintf(os.Stderr, "librespot-bridge: unknown command %q\n\n%s", fs.Arg(0), usage)
		return 2
	}
	if *pipeName == "" {
		fmt.Fprintln(os.Stderr, "librespot-bridge: --pipe must not be empty")
		return 2
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	log, closeLog, err := openLog(*debug)
	if err != nil {
		fmt.Fprintf(os.Stderr, "librespot-bridge: %v\n", err)
		return 1
	}
	defer closeLog()

	path := bridge.PipePath(*pipeName)

	// A second instance started by another component must not fight over the
	// pipe: if the existing bridge answers, this one is done.
	if otherAnswers(path, 1500*time.Millisecond) {
		log.Infof("another bridge already answers on %s; exiting", path)
		return 0
	}

	if *parentPID > 0 {
		go watchParent(ctx, *parentPID, stop, log)
	}

	mgr := spotifyx.NewManager(ctx, log)
	defer mgr.Close()

	srv := bridge.NewServer(ctx, log, mgr)
	if err := serve(srv, *pipeName, path, log); err != nil {
		log.Errorf("serve: %v", err)
		fmt.Fprintf(os.Stderr, "librespot-bridge: %v\n", err)
		return 1
	}

	srv.Wait(3 * time.Second)
	log.Infof("bridge stopped")
	return 0
}

// serve listens, retrying when a concurrently starting bridge wins the race for
// the pipe: whoever answers status keeps serving, the other one exits quietly.
func serve(srv *bridge.Server, name, path string, log *logging.Logger) error {
	err := srv.ListenAndServe(name)
	for attempt := 0; err != nil && attempt < 4; attempt++ {
		time.Sleep(300 * time.Millisecond)
		if otherAnswers(path, time.Second) {
			log.Infof("another bridge answered on %s; exiting", path)
			return nil
		}
		log.WithError(err).Warnf("retrying to listen on %s", path)
		err = srv.ListenAndServe(name)
	}
	return err
}

func runLogin(args []string) int {
	if len(args) > 0 {
		fmt.Fprintln(os.Stderr, "librespot-bridge: login takes no arguments")
		return 2
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	log := logging.New(os.Stderr, false)
	if _, err := spotifyx.Login(ctx, log, os.Stdout); err != nil {
		fmt.Fprintf(os.Stderr, "librespot-bridge: login failed: %v\n", err)
		return 1
	}
	return 0
}

// openLog opens %APPDATA%\foo_librespot\bridge.log.
func openLog(debug bool) (*logging.Logger, func(), error) {
	dir, err := creds.Dir()
	if err != nil {
		return nil, nil, err
	}
	path := filepath.Join(dir, "bridge.log")

	f, err := logging.OpenFile(path, 5<<20)
	if err != nil {
		return nil, nil, err
	}
	log := logging.New(f, debug)
	return log, func() { _ = f.Close() }, nil
}

// otherAnswers reports whether a bridge is already serving on path.
func otherAnswers(path string, timeout time.Duration) bool {
	conn, err := winio.DialPipe(path, &timeout)
	if err != nil {
		return false
	}
	defer conn.Close()

	if err := conn.SetDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return false
	}
	if _, err := conn.Write([]byte(`{"op":"status"}` + "\n")); err != nil {
		return false
	}
	line, err := bufio.NewReaderSize(conn, 64<<10).ReadBytes('\n')
	return err == nil && json.Valid(bytes.TrimSpace(line))
}

// watchParent cancels the bridge when pid exits.
func watchParent(ctx context.Context, pid int, cancel func(), log *logging.Logger) {
	h, err := windows.OpenProcess(windows.SYNCHRONIZE, false, uint32(pid))
	if err != nil {
		log.Warnf("parent process %d is not running; exiting", pid)
		cancel()
		return
	}
	defer windows.CloseHandle(h)

	log.Infof("watching parent process %d", pid)
	for {
		event, err := windows.WaitForSingleObject(h, 1000)
		if err == nil && event == windows.WAIT_OBJECT_0 {
			log.Infof("parent process %d exited", pid)
			cancel()
			return
		}
		select {
		case <-ctx.Done():
			return
		default:
		}
	}
}
