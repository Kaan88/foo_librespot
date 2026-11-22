package bridge

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"sync"
	"time"

	"github.com/Microsoft/go-winio"

	"foo_librespot/bridge/internal/logging"
	"foo_librespot/bridge/internal/spotifyuri"
	"foo_librespot/bridge/internal/spotifyx"
)

const (
	// maxRequestBytes bounds one request line (info may carry 500 URIs).
	maxRequestBytes = 4 << 20
	// pcmChunkFloats is how many interleaved float32 samples are written per
	// pipe write (~93 ms of stereo audio).
	pcmChunkFloats = 16384
)

// Server answers requests on one named pipe.
type Server struct {
	log *logging.Logger
	mgr *spotifyx.Manager
	ctx context.Context

	wg sync.WaitGroup
}

// NewServer returns a server bound to ctx.
func NewServer(ctx context.Context, log *logging.Logger, mgr *spotifyx.Manager) *Server {
	return &Server{log: log, mgr: mgr, ctx: ctx}
}

// PipePath is the Windows path of a pipe name.
func PipePath(name string) string {
	return `\\.\pipe\` + name
}

// ListenAndServe serves until ctx is cancelled or the listener fails.
func (s *Server) ListenAndServe(name string) error {
	l, err := winio.ListenPipe(PipePath(name), &winio.PipeConfig{
		MessageMode:      false,
		InputBufferSize:  64 << 10,
		OutputBufferSize: 64 << 10,
	})
	if err != nil {
		return fmt.Errorf("listening on pipe %s: %w", name, err)
	}
	defer l.Close()

	s.log.Infof("listening on %s", PipePath(name))

	done := make(chan struct{})
	go func() {
		select {
		case <-s.ctx.Done():
			_ = l.Close()
		case <-done:
		}
	}()
	defer close(done)

	for {
		conn, err := l.Accept()
		if err != nil {
			if s.ctx.Err() != nil || errors.Is(err, winio.ErrPipeListenerClosed) {
				break
			}
			s.log.WithError(err).Warnf("accepting pipe connection")
			continue
		}

		s.wg.Add(1)
		go func() {
			defer s.wg.Done()
			s.handle(conn)
		}()
	}

	return nil
}

// Wait blocks until every connection handler has finished or the timeout
// expires.
func (s *Server) Wait(timeout time.Duration) {
	done := make(chan struct{})
	go func() {
		s.wg.Wait()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(timeout):
		s.log.Warnf("timed out waiting for connections to finish")
	}
}

func (s *Server) handle(conn net.Conn) {
	defer conn.Close()

	ctx, cancel := context.WithCancel(s.ctx)
	defer cancel()

	line, err := readLine(bufio.NewReaderSize(conn, 64<<10), maxRequestBytes)
	if err != nil {
		s.writeError(conn, badRequest("malformed request: "+err.Error()))
		return
	}

	var req request
	if err := json.Unmarshal(line, &req); err != nil {
		s.writeError(conn, badRequest("invalid JSON: "+err.Error()))
		return
	}
	if req.Op == "" {
		s.writeError(conn, badRequest("missing op"))
		return
	}

	start := time.Now()
	s.log.Debugf("request: %s", describe(&req))
	defer func() {
		s.log.Infof("%s finished in %s", req.Op, time.Since(start).Round(time.Millisecond))
	}()

	switch req.Op {
	case "status":
		s.status(ctx, conn)
	case "resolve":
		s.resolve(ctx, conn, &req)
	case "info":
		s.info(ctx, conn, &req)
	case "open":
		s.open(ctx, conn, &req)
	default:
		s.writeError(conn, badRequest(fmt.Sprintf("unknown op %q", req.Op)))
	}
}

func (s *Server) status(ctx context.Context, conn net.Conn) {
	resp := statusResponse{OK: true, LoggedIn: true, Version: Version}

	if err := s.mgr.Do(ctx, func(env *spotifyx.Env) error { return nil }); err != nil {
		s.writeError(conn, err)
		return
	}
	resp.Username = s.mgr.Username()
	resp.Country = s.mgr.WaitCountry(10 * time.Second)
	s.writeJSON(conn, resp)
}

func (s *Server) resolve(ctx context.Context, conn net.Conn, req *request) {
	ref, err := spotifyuri.Parse(req.URI)
	if err != nil {
		s.writeError(conn, badRequest(err.Error()))
		return
	}

	tracks, err := s.mgr.Resolve(ctx, ref)
	if err != nil {
		s.writeError(conn, err)
		return
	}
	s.writeJSON(conn, tracksResponse{OK: true, Tracks: tracks})
}

func (s *Server) info(ctx context.Context, conn net.Conn, req *request) {
	if len(req.URIs) == 0 {
		s.writeError(conn, badRequest("info needs a non-empty uris array"))
		return
	}
	if len(req.URIs) > 1000 {
		s.writeError(conn, badRequest("info accepts at most 1000 uris"))
		return
	}

	tracks, err := s.mgr.Info(ctx, req.URIs)
	if err != nil {
		s.writeError(conn, err)
		return
	}
	s.writeJSON(conn, tracksResponse{OK: true, Tracks: tracks})
}

func (s *Server) open(ctx context.Context, conn net.Conn, req *request) {
	ref, err := spotifyuri.Parse(req.URI)
	if err != nil {
		s.writeError(conn, badRequest(err.Error()))
		return
	}
	if ref.Kind != spotifyuri.KindTrack {
		s.writeError(conn, badRequest(fmt.Sprintf("%s is not a track", ref.URI)))
		return
	}

	startMs := int64(0)
	if req.StartMs != nil {
		if *req.StartMs < 0 {
			s.writeError(conn, badRequest("start_ms must not be negative"))
			return
		}
		startMs = *req.StartMs
	}
	bitrate := 320
	if req.Bitrate != nil {
		switch *req.Bitrate {
		case 96, 160, 320:
			bitrate = *req.Bitrate
		default:
			s.writeError(conn, badRequest("bitrate must be 96, 160 or 320"))
			return
		}
	}

	result, err := s.mgr.Open(ctx, ref, startMs, bitrate)
	if err != nil {
		s.writeError(conn, err)
		return
	}
	defer result.Close()

	if err := s.writeJSON(conn, openHeader{
		OK:         true,
		Track:      result.Info,
		SampleRate: result.SampleRate,
		Channels:   channels,
		StartFrame: result.StartFrame,
	}); err != nil {
		return
	}

	if result.EndOfTrack || result.Source == nil {
		return
	}

	s.stream(ctx, conn, result)
}

const (
	// channels is the interleaved channel count of every decoded stream.
	channels = 2
)

// stream copies decoded float32 PCM to the client until the track ends or the
// connection breaks.
func (s *Server) stream(ctx context.Context, conn net.Conn, result *spotifyx.OpenResult) {
	samples := make([]float32, pcmChunkFloats)
	pcm := make([]byte, pcmChunkFloats*4)

	var frames int64
	start := time.Now()

	for {
		n, err := result.Source.Read(samples)
		if n > 0 {
			for i, v := range samples[:n] {
				binary.LittleEndian.PutUint32(pcm[i*4:], math.Float32bits(v))
			}
			if _, werr := conn.Write(pcm[:n*4]); werr != nil {
				s.log.Debugf("client disconnected after %d frames: %v", frames, werr)
				return
			}
			frames += int64(n) / channels
		}
		if err != nil {
			if !errors.Is(err, io.EOF) {
				s.log.WithError(err).Warnf("streaming %s", result.Info.URI)
			}
			break
		}
		select {
		case <-ctx.Done():
			return
		default:
		}
	}

	s.log.Infof("streamed %d frames (%.3f s) in %s", frames, float64(frames)/44100, time.Since(start).Round(time.Millisecond))
}

func (s *Server) writeJSON(w io.Writer, v any) error {
	raw, err := json.Marshal(v)
	if err != nil {
		s.log.WithError(err).Errorf("encoding response")
		return err
	}
	raw = append(raw, '\n')
	_, err = w.Write(raw)
	return err
}

func (s *Server) writeError(w io.Writer, err error) {
	code := codeFor(err)
	s.log.Debugf("error (%s): %v", code, err)
	_ = s.writeJSON(w, errorResponse{OK: false, Error: err.Error(), Code: code})
}

// readLine reads one newline-terminated line of at most max bytes.
func readLine(r *bufio.Reader, max int) ([]byte, error) {
	var buf []byte
	for {
		chunk, err := r.ReadSlice('\n')
		if len(buf)+len(chunk) > max {
			return nil, fmt.Errorf("request larger than %d bytes", max)
		}
		buf = append(buf, chunk...)
		switch {
		case err == nil:
			return bytes.TrimSpace(buf), nil
		case errors.Is(err, bufio.ErrBufferFull):
			continue
		case err == io.EOF && len(buf) > 0:
			return bytes.TrimSpace(buf), nil
		default:
			return nil, err
		}
	}
}

func describe(req *request) string {
	uri := req.URI
	if len(uri) > 120 {
		uri = uri[:120] + "..."
	}
	switch req.Op {
	case "info":
		return fmt.Sprintf("info with %d uris", len(req.URIs))
	case "open":
		start := int64(0)
		if req.StartMs != nil {
			start = *req.StartMs
		}
		return fmt.Sprintf("open %s at %d ms", uri, start)
	default:
		return fmt.Sprintf("%s %s", req.Op, uri)
	}
}
