// Command bridgectl is a small client for manually exercising the bridge
// protocol: status, resolve, info and open-to-wav.
package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"math"
	"net"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/Microsoft/go-winio"

	"foo_librespot/bridge/internal/wav"
)

const usage = `Usage:
  bridgectl [--pipe <name>] status
  bridgectl [--pipe <name>] resolve <uri-or-url>
  bridgectl [--pipe <name>] info <uri-or-url>... [--bitrate 96|160|320]
  bridgectl [--pipe <name>] library
  bridgectl [--pipe <name>] search <query> [--limit N]
  bridgectl [--pipe <name>] artist <uri-or-url>
  bridgectl [--pipe <name>] saved <track-uri>...
  bridgectl [--pipe <name>] save <track-uri>...
  bridgectl [--pipe <name>] unsave <track-uri>...
  bridgectl [--pipe <name>] playlist-add <playlist-uri> <track-uri>... [--dry-run]
  bridgectl [--pipe <name>] follow <artist|album|playlist uri>... [--dry-run]
  bridgectl [--pipe <name>] unfollow <artist|album|playlist uri>... [--dry-run]
  bridgectl [--pipe <name>] following <artist|album|playlist uri>...
  bridgectl [--pipe <name>] playlist-create <name> [--dry-run]
  bridgectl [--pipe <name>] playlist-rename <playlist-uri> <name> [--dry-run]
  bridgectl [--pipe <name>] playlist-delete <playlist-uri> [--dry-run]
  bridgectl [--pipe <name>] playlist-remove <playlist-uri> <index>:<track-uri>... [--dry-run]
  bridgectl [--pipe <name>] config [--cache-mb N] [--clear-cache] [--debug on|off]
  bridgectl [--pipe <name>] liked
  bridgectl [--pipe <name>] report-play <track-uri> --started-at MS --ms-played MS [--duration-ms MS]
                 [--context URI] [--reason-start R] [--reason-end R] [--shuffle] [--format F] --dry-run
  bridgectl [--pipe <name>] logout
  bridgectl [--pipe <name>] shutdown
  bridgectl [--pipe <name>] radio <track|artist|album|playlist uri>
  bridgectl [--pipe <name>] open-to-wav <uri-or-url> <out.wav> [--start-ms N] [--seconds S] [--bitrate 96|160|320]

Commands:
  status         Ask the bridge for its login and session state.
  resolve        Expand a track, album, playlist or Liked Songs reference.
  info           Fetch metadata for one or more tracks.
  library        List the Spotify tab's sections.
  search         Search Spotify (tracks, albums, artists, playlists).
  artist         Fetch an artist page: top tracks, albums, singles.
  saved          Tell which tracks are in Liked Songs.
  save, unsave   Add tracks to / remove them from Liked Songs.
  playlist-add   Append tracks to a playlist (--dry-run: show the request, send nothing).
  follow         Follow artists/albums (library) and playlists (library list).
  unfollow       The reverse of follow.
  following      Tell which of the URIs the account follows.
  playlist-create   Create an empty playlist at the top of the library.
  playlist-rename   Rename a playlist.
  playlist-delete   Remove a playlist from the library (unfollow).
  playlist-remove   Remove tracks from a playlist by position (index:uri).
  liked          List the track URIs in Liked Songs.
  report-play    Show the play event the bridge would send (only --dry-run is allowed here).
  logout         Forget the login: drop the session and delete credentials.json.
  shutdown       Stop the bridge (only the process named by --parent-pid may).
  radio          Tracks of a radio seeded by a track, artist, album or playlist.
  open-to-wav    Open a track, print the header and write the PCM to a WAV.
`

type header struct {
	OK         bool            `json:"ok"`
	Error      string          `json:"error"`
	Code       string          `json:"code"`
	Track      json.RawMessage `json:"track"`
	SampleRate int32           `json:"sample_rate"`
	Channels   int32           `json:"channels"`
	StartFrame int64           `json:"start_frame"`
	FromCache  bool            `json:"from_cache"`
	RetryAfter int             `json:"retry_after"`
}

func main() {
	os.Exit(run(os.Args[1:]))
}

func run(args []string) int {
	fs := flag.NewFlagSet("bridgectl", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	pipe := fs.String("pipe", "foo_librespot", "named pipe of the bridge")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	rest := fs.Args()
	if len(rest) == 0 {
		fmt.Fprint(os.Stderr, usage)
		return 2
	}

	switch rest[0] {
	case "help", "-h", "--help":
		fmt.Print(usage)
		return 0
	case "status":
		return doJSON(*pipe, map[string]any{"op": "status"})
	case "resolve":
		if len(rest) != 2 {
			fmt.Fprintln(os.Stderr, "bridgectl: resolve needs one uri")
			return 2
		}
		return doJSON(*pipe, map[string]any{"op": "resolve", "uri": rest[1]})
	case "info":
		return doInfo(*pipe, rest[1:])
	case "library":
		if len(rest) != 1 {
			fmt.Fprintln(os.Stderr, "bridgectl: library takes no arguments")
			return 2
		}
		return doJSON(*pipe, map[string]any{"op": "library"})
	case "search":
		return doSearch(*pipe, rest[1:])
	case "artist":
		if len(rest) != 2 {
			fmt.Fprintln(os.Stderr, "bridgectl: artist needs one uri")
			return 2
		}
		return doJSON(*pipe, map[string]any{"op": "artist", "uri": rest[1]})
	case "saved", "save", "unsave":
		if len(rest) < 2 {
			fmt.Fprintf(os.Stderr, "bridgectl: %s needs at least one track uri\n", rest[0])
			return 2
		}
		return doJSON(*pipe, map[string]any{"op": rest[0], "uris": rest[1:]})
	case "playlist-add":
		return doPlaylistAdd(*pipe, rest[1:])
	case "follow", "unfollow":
		return doFollow(*pipe, rest[0], rest[1:])
	case "following":
		if len(rest) < 2 {
			fmt.Fprintln(os.Stderr, "bridgectl: following needs at least one uri")
			return 2
		}
		return doJSON(*pipe, map[string]any{"op": "following", "uris": rest[1:]})
	case "playlist-create":
		return doPlaylistCreate(*pipe, rest[1:])
	case "playlist-rename":
		return doPlaylistRename(*pipe, rest[1:])
	case "playlist-delete":
		return doPlaylistDelete(*pipe, rest[1:])
	case "playlist-remove":
		return doPlaylistRemove(*pipe, rest[1:])
	case "radio":
		if len(rest) != 2 {
			fmt.Fprintln(os.Stderr, "bridgectl: radio needs one uri")
			return 2
		}
		return doJSON(*pipe, map[string]any{"op": "radio", "uri": rest[1]})
	case "open-to-wav":
		return doOpen(*pipe, rest[1:])
	case "config":
		return doConfig(*pipe, rest[1:])
	case "liked", "logout", "shutdown":
		if len(rest) != 1 {
			fmt.Fprintf(os.Stderr, "bridgectl: %s takes no arguments\n", rest[0])
			return 2
		}
		return doJSON(*pipe, map[string]any{"op": rest[0]})
	case "report-play":
		return doReportPlay(*pipe, rest[1:])
	default:
		fmt.Fprintf(os.Stderr, "bridgectl: unknown command %q\n\n%s", rest[0], usage)
		return 2
	}
}

// doConfig sends the config op: `config [--cache-mb N] [--clear-cache]`.
func doConfig(pipe string, args []string) int {
	fs := flag.NewFlagSet("config", flag.ContinueOnError)
	cacheMB := fs.Int("cache-mb", -1, "audio cache limit in MiB (0 = off)")
	clear := fs.Bool("clear-cache", false, "delete the cached audio")
	debug := fs.String("debug", "", "debug logging: on or off")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	req := map[string]any{"op": "config"}
	switch *debug {
	case "":
	case "on":
		req["debug"] = true
	case "off":
		req["debug"] = false
	default:
		fmt.Fprintln(os.Stderr, "bridgectl: --debug takes on or off")
		return 2
	}
	if *cacheMB >= 0 {
		req["cache_mb"] = *cacheMB
	}
	if *clear {
		req["clear_cache"] = true
	}
	return doJSON(pipe, req)
}

// doReportPlay previews a report_play request. bridgectl only ever sends it
// as a dry run, so trying the command out cannot put a play into anyone's
// listening history.
func doReportPlay(pipe string, args []string) int {
	var rest []string
	shuffle, dryRun := false, false
	for _, a := range args {
		switch a {
		case "--shuffle", "-shuffle":
			shuffle = true
		case "--dry-run", "-dry-run":
			dryRun = true
		default:
			rest = append(rest, a)
		}
	}
	flags, positional := splitFlags(rest)
	fs := flag.NewFlagSet("report-play", flag.ContinueOnError)
	startedAt := fs.Int64("started-at", 0, "unix ms when playback started")
	msPlayed := fs.Int64("ms-played", 0, "ms actually played")
	durationMs := fs.Int64("duration-ms", 0, "track length in ms")
	contextURI := fs.String("context", "", "context uri (playlist, album...)")
	reasonStart := fs.String("reason-start", "", "start reason, e.g. clickrow")
	reasonEnd := fs.String("reason-end", "", "end reason, e.g. trackdone")
	format := fs.String("format", "", "audio format, e.g. vorbis 320")
	if err := fs.Parse(flags); err != nil {
		return 2
	}
	if len(positional) != 1 {
		fmt.Fprintln(os.Stderr, "bridgectl: report-play needs one track uri")
		return 2
	}
	if !dryRun {
		fmt.Fprintln(os.Stderr, "bridgectl: report-play only runs with --dry-run")
		return 2
	}
	return doJSON(pipe, map[string]any{
		"op": "report_play", "uri": positional[0], "context_uri": *contextURI,
		"started_at": *startedAt, "ms_played": *msPlayed, "duration_ms": *durationMs,
		"reason_start": *reasonStart, "reason_end": *reasonEnd, "shuffle": shuffle,
		"audio_format": *format, "dry_run": true,
	})
}

func doInfo(pipe string, args []string) int {
	fs := flag.NewFlagSet("info", flag.ContinueOnError)
	bitrate := fs.Int("bitrate", 320, "preferred bitrate (96, 160 or 320)")

	flags, positional := splitFlags(args)
	if err := fs.Parse(flags); err != nil {
		return 2
	}
	if len(positional) == 0 {
		fmt.Fprintln(os.Stderr, "bridgectl: info needs at least one uri")
		return 2
	}
	return doJSON(pipe, map[string]any{"op": "info", "uris": positional, "bitrate": *bitrate})
}

func doPlaylistAdd(pipe string, args []string) int {
	dryRun := false
	var positional []string
	for _, arg := range args {
		switch arg {
		case "--dry-run", "-dry-run":
			dryRun = true
		default:
			positional = append(positional, arg)
		}
	}
	if len(positional) < 2 {
		fmt.Fprintln(os.Stderr, "bridgectl: playlist-add needs <playlist-uri> <track-uri>... [--dry-run]")
		return 2
	}
	return doJSON(pipe, map[string]any{
		"op":       "playlist_add",
		"playlist": positional[0],
		"uris":     positional[1:],
		"dry_run":  dryRun,
	})
}

// splitDryRun separates --dry-run from a command's positionals.
func splitDryRun(args []string) (positional []string, dryRun bool) {
	for _, arg := range args {
		switch arg {
		case "--dry-run", "-dry-run":
			dryRun = true
		default:
			positional = append(positional, arg)
		}
	}
	return positional, dryRun
}

func doFollow(pipe, op string, args []string) int {
	positional, dryRun := splitDryRun(args)
	if len(positional) == 0 {
		fmt.Fprintf(os.Stderr, "bridgectl: %s needs at least one uri [--dry-run]\n", op)
		return 2
	}
	return doJSON(pipe, map[string]any{"op": op, "uris": positional, "dry_run": dryRun})
}

func doPlaylistCreate(pipe string, args []string) int {
	positional, dryRun := splitDryRun(args)
	if len(positional) != 1 {
		fmt.Fprintln(os.Stderr, "bridgectl: playlist-create needs <name> [--dry-run]")
		return 2
	}
	return doJSON(pipe, map[string]any{"op": "playlist_create", "name": positional[0], "dry_run": dryRun})
}

func doPlaylistRename(pipe string, args []string) int {
	positional, dryRun := splitDryRun(args)
	if len(positional) != 2 {
		fmt.Fprintln(os.Stderr, "bridgectl: playlist-rename needs <playlist-uri> <name> [--dry-run]")
		return 2
	}
	return doJSON(pipe, map[string]any{
		"op":       "playlist_rename",
		"playlist": positional[0],
		"name":     positional[1],
		"dry_run":  dryRun,
	})
}

func doPlaylistDelete(pipe string, args []string) int {
	positional, dryRun := splitDryRun(args)
	if len(positional) != 1 {
		fmt.Fprintln(os.Stderr, "bridgectl: playlist-delete needs <playlist-uri> [--dry-run]")
		return 2
	}
	return doJSON(pipe, map[string]any{"op": "playlist_delete", "playlist": positional[0], "dry_run": dryRun})
}

func doPlaylistRemove(pipe string, args []string) int {
	positional, dryRun := splitDryRun(args)
	if len(positional) < 2 {
		fmt.Fprintln(os.Stderr, "bridgectl: playlist-remove needs <playlist-uri> <index>:<track-uri>... [--dry-run]")
		return 2
	}
	items := make([]map[string]any, 0, len(positional)-1)
	for _, arg := range positional[1:] {
		index, uri, ok := strings.Cut(arg, ":")
		if !ok {
			fmt.Fprintf(os.Stderr, "bridgectl: %q is not <index>:<track-uri>\n", arg)
			return 2
		}
		n, err := strconv.Atoi(index)
		if err != nil {
			fmt.Fprintf(os.Stderr, "bridgectl: %q is not <index>:<track-uri>\n", arg)
			return 2
		}
		items = append(items, map[string]any{"uri": uri, "index": n})
	}
	return doJSON(pipe, map[string]any{
		"op":       "playlist_remove",
		"playlist": positional[0],
		"items":    items,
		"dry_run":  dryRun,
	})
}

func doSearch(pipe string, args []string) int {
	fs := flag.NewFlagSet("search", flag.ContinueOnError)
	limit := fs.Int("limit", 20, "results per category (max 50)")

	flags, positional := splitFlags(args)
	if err := fs.Parse(flags); err != nil {
		return 2
	}
	if len(positional) == 0 {
		fmt.Fprintln(os.Stderr, "bridgectl: search needs a query")
		return 2
	}
	return doJSON(pipe, map[string]any{"op": "search", "query": strings.Join(positional, " "), "limit": *limit})
}

// splitFlags separates flag arguments from positionals so a command's flags can
// follow its positionals, as users write "search nation of language --limit 5".
func splitFlags(args []string) (flags, positional []string) {
	for i := 0; i < len(args); i++ {
		arg := args[i]
		if !strings.HasPrefix(arg, "-") || arg == "-" {
			positional = append(positional, arg)
			continue
		}
		flags = append(flags, arg)
		if !strings.Contains(arg, "=") && i+1 < len(args) {
			flags = append(flags, args[i+1])
			i++
		}
	}
	return flags, positional
}

func dial(pipe string) (net.Conn, error) {
	timeout := 5 * time.Second
	return winio.DialPipe(`\\.\pipe\`+pipe, &timeout)
}

func doJSON(pipe string, req map[string]any) int {
	conn, err := dial(pipe)
	if err != nil {
		fmt.Fprintf(os.Stderr, "bridgectl: %v\n", err)
		return 1
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(120 * time.Second))

	raw, _ := json.Marshal(req)
	if _, err := conn.Write(append(raw, '\n')); err != nil {
		fmt.Fprintf(os.Stderr, "bridgectl: %v\n", err)
		return 1
	}

	line, err := bufio.NewReaderSize(conn, 1<<20).ReadBytes('\n')
	if err != nil {
		fmt.Fprintf(os.Stderr, "bridgectl: reading response: %v\n", err)
		return 1
	}
	return printResponse(line)
}

func printResponse(line []byte) int {
	var probe struct {
		OK    bool   `json:"ok"`
		Error string `json:"error"`
		Code  string `json:"code"`
	}
	_ = json.Unmarshal(line, &probe)

	var pretty bytes.Buffer
	if err := json.Indent(&pretty, bytes.TrimSpace(line), "", "  "); err == nil {
		fmt.Println(pretty.String())
	} else {
		fmt.Println(string(line))
	}
	if !probe.OK {
		fmt.Fprintf(os.Stderr, "bridgectl: request failed (%s): %s\n", probe.Code, probe.Error)
		return 1
	}
	return 0
}

func doOpen(pipe string, args []string) int {
	fs := flag.NewFlagSet("open-to-wav", flag.ContinueOnError)
	startMs := fs.Int64("start-ms", 0, "start position in milliseconds")
	seconds := fs.Float64("seconds", 0, "stop after this many seconds (0 = whole track)")
	bitrate := fs.Int("bitrate", 320, "preferred bitrate")

	// Accept the flags before or after the two positionals, as users write
	// "open-to-wav <uri> <out.wav> --start-ms 60000".
	var flags, positional []string
	for i := 0; i < len(args); i++ {
		arg := args[i]
		if !strings.HasPrefix(arg, "-") || arg == "-" {
			positional = append(positional, arg)
			continue
		}
		flags = append(flags, arg)
		if !strings.Contains(arg, "=") && i+1 < len(args) {
			flags = append(flags, args[i+1])
			i++
		}
	}
	if err := fs.Parse(flags); err != nil {
		return 2
	}
	if len(positional) != 2 {
		fmt.Fprintln(os.Stderr, "bridgectl: open-to-wav needs <uri> <out.wav>")
		return 2
	}
	uri, outPath := positional[0], positional[1]

	conn, err := dial(pipe)
	if err != nil {
		fmt.Fprintf(os.Stderr, "bridgectl: %v\n", err)
		return 1
	}
	defer conn.Close()

	began := time.Now()
	req, _ := json.Marshal(map[string]any{
		"op":       "open",
		"uri":      uri,
		"start_ms": *startMs,
		"bitrate":  *bitrate,
	})
	if _, err := conn.Write(append(req, '\n')); err != nil {
		fmt.Fprintf(os.Stderr, "bridgectl: %v\n", err)
		return 1
	}

	reader := bufio.NewReaderSize(conn, 1<<20)
	line, err := reader.ReadBytes('\n')
	if err != nil {
		fmt.Fprintf(os.Stderr, "bridgectl: reading header: %v\n", err)
		return 1
	}

	var hdr header
	if err := json.Unmarshal(line, &hdr); err != nil {
		fmt.Fprintf(os.Stderr, "bridgectl: invalid header: %v\n%s", err, line)
		return 1
	}
	if !hdr.OK {
		fmt.Fprintf(os.Stderr, "bridgectl: open failed (%s, retry_after %d): %s\n", hdr.Code, hdr.RetryAfter, hdr.Error)
		return 1
	}

	fmt.Printf("uri:         %s\n", uri)
	fmt.Printf("header in:   %s, from_cache: %v\n", time.Since(began).Round(time.Millisecond), hdr.FromCache)
	fmt.Printf("start_frame: %d (%.3f ms)\n", hdr.StartFrame, float64(hdr.StartFrame)/44.1)
	fmt.Printf("sample_rate: %d, channels: %d\n", hdr.SampleRate, hdr.Channels)
	var pretty bytes.Buffer
	if err := json.Indent(&pretty, hdr.Track, "", "  "); err == nil {
		fmt.Printf("track:\n%s\n", pretty.String())
	}

	out, err := os.Create(outPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "bridgectl: %v\n", err)
		return 1
	}
	ww, err := wav.NewWriter(out, hdr.SampleRate, hdr.Channels)
	if err != nil {
		fmt.Fprintf(os.Stderr, "bridgectl: %v\n", err)
		return 1
	}

	const chunkFloats = 16384
	pcm := make([]byte, chunkFloats*4)
	samples := make([]float32, chunkFloats)

	maxFrames := int64(-1)
	if *seconds > 0 {
		maxFrames = int64(math.Round(*seconds * float64(hdr.SampleRate)))
	}

	var frames int64
	var peak float64
	var sumSquares float64
	started := time.Now()

readLoop:
	for {
		want := len(pcm)
		if maxFrames >= 0 {
			remaining := int((maxFrames - frames) * int64(hdr.Channels) * 4)
			if remaining <= 0 {
				break
			}
			if remaining < want {
				want = remaining
			}
		}

		n, err := io.ReadFull(reader, pcm[:want])
		if n > 0 {
			n -= n % 4
			count := n / 4
			for i := 0; i < count; i++ {
				v := math.Float32frombits(binary.LittleEndian.Uint32(pcm[i*4:]))
				samples[i] = v
				a := math.Abs(float64(v))
				if a > peak {
					peak = a
				}
				sumSquares += float64(v) * float64(v)
			}
			if err := ww.WriteSamples(samples[:count]); err != nil {
				fmt.Fprintf(os.Stderr, "bridgectl: writing wav: %v\n", err)
				return 1
			}
			frames += int64(count) / int64(hdr.Channels)
		}
		if err != nil {
			if err != io.EOF && err != io.ErrUnexpectedEOF {
				fmt.Fprintf(os.Stderr, "bridgectl: reading pcm: %v\n", err)
			}
			break readLoop
		}
		if maxFrames >= 0 && frames >= maxFrames {
			break
		}
	}

	if err := ww.Close(); err != nil {
		fmt.Fprintf(os.Stderr, "bridgectl: %v\n", err)
		return 1
	}
	if err := out.Close(); err != nil {
		fmt.Fprintf(os.Stderr, "bridgectl: %v\n", err)
		return 1
	}

	elapsed := time.Since(started)
	fmt.Printf("wrote:       %s (%d frames, %.3f s of audio in %s)\n",
		outPath, frames, float64(frames)/float64(hdr.SampleRate), elapsed.Round(time.Millisecond))
	if frames > 0 {
		rms := math.Sqrt(sumSquares / float64(frames*int64(hdr.Channels)))
		fmt.Printf("levels:      peak %.4f, rms %.4f (linear float)\n", peak, rms)
	}
	if hdr.StartFrame != int64(math.Round(float64(*startMs)*44.1)) {
		fmt.Fprintf(os.Stderr, "bridgectl: WARNING: start_frame %d does not match start_ms %d\n", hdr.StartFrame, *startMs)
	}
	return 0
}
