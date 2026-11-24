// Command bridge serves decoded Spotify audio to the foo_librespot foobar2000
// component over a Windows named pipe. The protocol is proto/PROTOCOL.md.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
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
  librespot-bridge.exe [--pipe <name>] [--parent-pid <pid>] [--cache-mb <n>] [--debug]
  librespot-bridge.exe login

Commands:
  login   Authenticate with the Spotify device-authorization flow and save
          reusable credentials to %APPDATA%\foo_librespot\credentials.json.

Flags:
  --pipe <name>        Named pipe to serve on (default foo_librespot).
  --parent-pid <pid>   Exit when the given process exits.
  --cache-mb <n>       Audio cache limit in MiB (0 = off, the default; the
                       component passes its setting). Can be changed while
                       running with the "config" op.
  --cache-dir <dir>    Audio cache folder (default
                       %LOCALAPPDATA%\foo_librespot\cache).
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
	cacheMB := fs.Int("cache-mb", 0, "audio cache limit in MiB (0 = off)")
	cacheDir := fs.String("cache-dir", "", "audio cache folder")
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

	// Several bridges append to the same log file, so every line names the
	// process that wrote it.
	self := "pid=" + strconv.Itoa(os.Getpid())
	log = log.WithPrefix(self)

	path := bridge.PipePath(*pipeName)

	// A second instance (another component, or a duplicate spawn) must not
	// fight over the pipe. Pipe connectability is the whole test: if a server
	// is listening, this instance is redundant and exits quietly.
	if pipeAlive(path, 500*time.Millisecond) {
		log.Infof("another bridge already serves %s; exiting", path)
		return 0
	}

	if *parentPID > 0 {
		go watchParent(ctx, *parentPID, stop, log)
	}

	// The session starts connecting right away, in the background: the first
	// status and the first request then already have something to report.
	mgr := spotifyx.NewManager(ctx, log)
	dir := *cacheDir
	if dir == "" {
		dir = defaultCacheDir()
	}
	mgr.SetCacheDir(dir)
	if *cacheMB < 0 {
		*cacheMB = 0
	}
	if err := mgr.ConfigureCache(*cacheMB); err != nil {
		log.WithError(err).Warnf("audio cache disabled")
	}
	mgr.Start()
	defer mgr.Close()

	srv := bridge.NewServer(ctx, log, mgr)
	// The owning foobar2000 stops its bridge on quit (shutdown op) instead of
	// leaving it to notice the parent's exit, so the exe is not locked during a
	// component update.
	srv.SetOwner(*parentPID, stop)
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
// the pipe: whoever listens keeps serving, the other one exits quietly.
func serve(srv *bridge.Server, name, path string, log *logging.Logger) error {
	err := srv.ListenAndServe(name)
	for attempt := 0; err != nil && attempt < 4; attempt++ {
		time.Sleep(300 * time.Millisecond)
		if pipeAlive(path, time.Second) {
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

// defaultCacheDir is %LOCALAPPDATA%\foo_librespot\cache.
func defaultCacheDir() string {
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		if dir, err := os.UserCacheDir(); err == nil {
			base = dir
		}
	}
	if base == "" {
		return ""
	}
	return filepath.Join(base, "foo_librespot", "cache")
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

// pipeAlive reports whether a server is accepting connections on path. A
// successful dial is proof enough; no request is sent.
func pipeAlive(path string, timeout time.Duration) bool {
	conn, err := winio.DialPipe(path, &timeout)
	if err != nil {
		return false
	}
	_ = conn.Close()
	return true
}

// watchParent shuts the bridge down when pid exits.
//
// The wait result is verified before it is acted on: a process handle is
// signaled only by termination, but GetExitCodeProcess is the authoritative
// answer (STILL_ACTIVE means the process is alive), and it also supplies the
// exit code that makes the log line unambiguous. A failure to open the process
// (access denied, a process that is still starting) is retried instead of being
// treated as death, so an unreadable parent can never kill a healthy bridge.
func watchParent(ctx context.Context, pid int, cancel func(), log *logging.Logger) {
	handle, err := openParent(ctx, pid, log)
	if err != nil {
		if errors.Is(err, errParentGone) {
			log.Warnf("parent process %d does not exist; shutting down", pid)
			cancel()
		}
		return
	}
	defer windows.CloseHandle(handle) //nolint:errcheck

	log.Infof("watching parent process %d", pid)

	for {
		event, err := windows.WaitForSingleObject(handle, 1000)
		switch {
		case err != nil:
			log.WithError(err).Warnf("waiting for parent process %d failed; still watching", pid)
		case event == windows.WAIT_OBJECT_0:
			var code uint32
			if err := windows.GetExitCodeProcess(handle, &code); err != nil {
				log.WithError(err).Warnf("parent process %d wait signaled, exit code unknown; shutting down", pid)
				cancel()
				return
			}
			if code == stillActive {
				// Should not happen, but never shut down on a doubtful signal.
				log.Warnf("parent process %d wait signaled but it is still active; continuing", pid)
				break
			}
			log.Infof("parent process %d exited (exit code %d); shutting down", pid, code)
			cancel()
			return
		case event == uint32(windows.WAIT_TIMEOUT):
			// The parent is still running.
		default:
			log.Warnf("unexpected wait result %#x for parent process %d; still watching", event, pid)
		}

		select {
		case <-ctx.Done():
			return
		default:
		}
	}
}

// stillActive is STILL_ACTIVE from GetExitCodeProcess.
const stillActive = 259

var errParentGone = errors.New("parent process does not exist")

// openParent retries opening the parent handle when the failure is transient
// (access denied or a pid that is still being created), and reports
// errParentGone only when Windows says no such process exists.
func openParent(ctx context.Context, pid int, log *logging.Logger) (windows.Handle, error) {
	for attempt := 0; ; attempt++ {
		h, err := windows.OpenProcess(
			windows.SYNCHRONIZE|windows.PROCESS_QUERY_LIMITED_INFORMATION, false, uint32(pid))
		if err == nil {
			return h, nil
		}
		if errors.Is(err, windows.ERROR_INVALID_PARAMETER) {
			// OpenProcess reports a pid that names no process this way.
			return 0, errParentGone
		}
		log.WithError(err).Warnf("cannot open parent process %d; retrying", pid)
		select {
		case <-ctx.Done():
			return 0, err
		case <-time.After(500 * time.Millisecond):
		}
		if attempt >= 20 {
			// Ten seconds of failures: keep serving anyway rather than kill a
			// bridge that is only unable to query its parent.
			log.Warnf("giving up watching parent process %d after repeated open failures", pid)
			return 0, err
		}
	}
}
