package spotifyx

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"net/http"
	"runtime"
	"strings"
	"sync"
	"time"

	librespot "github.com/devgianlu/go-librespot"
	eventsenderpb "github.com/devgianlu/go-librespot/proto/spotify/event_sender"
	"google.golang.org/protobuf/proto"

	"foo_librespot/bridge/internal/spotifyuri"
)

// Play reporting. go-librespot defines the player's EventManager interface and
// the gabo event-sender protocol (spclient.PublishEvents and the event_sender
// protos), but its open-source build only ships a no-op event manager, and the
// bridge does not run go-librespot's player state machine anyway (the player
// is only a stream factory here). So the bridge reports a play itself, once it
// is over, as one RawCoreStream event, the per-stream event Spotify's own
// clients send and which feeds listening history and Recently played.

// gaboEventsPath is spclient's event receiver (see spclient.PublishEvents).
const gaboEventsPath = "/gabo-receiver-service/v3/events/"

// maxReportedMs bounds ms_played and duration_ms (a day).
const maxReportedMs = 24 * 3600 * 1000

// PlayReport describes one finished play of a track, as the client saw it.
type PlayReport struct {
	URI        string
	ContextURI string
	// StartedAt is when playback of the track began (Unix milliseconds).
	StartedAt  int64
	MsPlayed   int64
	DurationMs int64
	// ReasonStart / ReasonEnd use Spotify's vocabulary: clickrow, trackdone,
	// fwdbtn, backbtn, endplay, ...
	ReasonStart string
	ReasonEnd   string
	Shuffle     bool
	// AudioFormat is the codec and bitrate that played, e.g. "vorbis 320".
	AudioFormat string
}

var reasonWords = map[string]bool{
	"clickrow": true, "trackdone": true, "fwdbtn": true, "backbtn": true,
	"endplay": true, "playbtn": true, "remote": true, "logout": true,
	"trackerror": true, "unknown": true, "appload": true,
}

// validate canonicalises r and refuses reports that make no sense.
func (r *PlayReport) validate() error {
	ref, err := spotifyuri.Parse(r.URI)
	if err != nil {
		return badInputf("%v", err)
	}
	if ref.Kind != spotifyuri.KindTrack {
		return badInputf("%s is not a track", ref.URI)
	}
	r.URI = ref.URI
	if r.ContextURI == "" {
		// A track played on its own is its own context, as in Spotify's clients.
		r.ContextURI = r.URI
	}
	if !strings.HasPrefix(r.ContextURI, "spotify:") {
		return badInputf("context_uri must be a spotify: URI")
	}
	if r.MsPlayed < 0 || r.MsPlayed > maxReportedMs {
		return badInputf("ms_played out of range")
	}
	if r.DurationMs < 0 || r.DurationMs > maxReportedMs {
		return badInputf("duration_ms out of range")
	}
	if r.StartedAt <= 0 {
		return badInputf("started_at (Unix milliseconds) is required")
	}
	for _, reason := range []*string{&r.ReasonStart, &r.ReasonEnd} {
		if *reason == "" {
			*reason = "unknown"
		}
		if !reasonWords[*reason] {
			return badInputf("unknown reason %q", *reason)
		}
	}
	return nil
}

// eventSequence numbers the events of one bridge run: Spotify's clients keep a
// sequence id per run and count their events in it.
type eventSequence struct {
	mu        sync.Mutex
	id        []byte
	next      int64
	sessionID []byte
}

var playSequence eventSequence

func (s *eventSequence) take() (id []byte, number int64, session []byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.id == nil {
		s.id = randomBytes(16)
		s.sessionID = randomBytes(16)
		s.next = 1
	}
	number = s.next
	s.next++
	return s.id, number, s.sessionID
}

func randomBytes(n int) []byte {
	b := make([]byte, n)
	_, _ = rand.Read(b)
	return b
}

func eventFragment(name string, msg proto.Message) (*eventsenderpb.EventEnvelope_EventFragment, error) {
	data, err := proto.Marshal(msg)
	if err != nil {
		return nil, fmt.Errorf("encoding %s: %w", name, err)
	}
	return &eventsenderpb.EventEnvelope_EventFragment{Name: name, Data: data}, nil
}

// buildPlayEvent builds the PublishEventsRequest for one play. deviceID is the
// credentials' device id (hex), seqID/seqNo the event sequence, now the send
// time.
func buildPlayEvent(r PlayReport, deviceID string, seqID []byte, seqNo int64, sessionID, playbackID []byte, now time.Time) (*eventsenderpb.PublishEventsRequest, error) {
	stream := &eventsenderpb.RawCoreStream{
		PlaybackId:          playbackID,
		MediaType:           proto.String("audio"),
		FeatureIdentifier:   proto.String("foobar2000"),
		FeatureVersion:      proto.String(librespot.VersionNumberString()),
		ViewUri:             proto.String(r.ContextURI),
		SourceStart:         proto.String("unknown"),
		ReasonStart:         proto.String(r.ReasonStart),
		SourceEnd:           proto.String("unknown"),
		ReasonEnd:           proto.String(r.ReasonEnd),
		PlaybackStartTime:   proto.Int64(r.StartedAt),
		MsPlayed:            proto.Int32(int32(r.MsPlayed)),
		MsPlayedNominal:     proto.Int32(int32(r.MsPlayed)),
		MsPlayedOverlapping: proto.Int32(0),
		MsPlayedBackground:  proto.Int32(int32(r.MsPlayed)),
		Live:                proto.Bool(false),
		Shuffle:             proto.Bool(r.Shuffle),
		AudioFormat:         proto.String(r.AudioFormat),
		PlayContext:         proto.String(r.ContextURI),
		ContentUri:          proto.String(r.URI),
		DisplayedContentUri: proto.String(r.URI),
		ContentIsDownloaded: proto.Bool(false),
		IncognitoMode:       proto.Bool(false),
		Provider:            proto.String("context"),
		IsAssumedPremium:    proto.Bool(true),
	}

	devID, _ := hex.DecodeString(deviceID)
	parts := []struct {
		name string
		msg  proto.Message
	}{
		{"context_application_desktop", &eventsenderpb.ApplicationDesktop{
			VersionString: librespot.SpotifyLikeClientVersion(),
			SessionId:     sessionID,
		}},
		{"context_client_id", &eventsenderpb.ClientId{Value: librespot.ClientId}},
		{"context_device_desktop", &eventsenderpb.DeviceDesktop{
			PlatformType: runtime.GOOS,
			DeviceId:     deviceID,
		}},
		{"context_installation_id", &eventsenderpb.InstallationId{Value: devID}},
		{"context_time", &eventsenderpb.Time{Value: now.UnixMilli()}},
		{"message", stream},
	}
	envelope := &eventsenderpb.EventEnvelope{
		EventName:      "RawCoreStream",
		SequenceId:     seqID,
		SequenceNumber: seqNo,
	}
	for _, part := range parts {
		f, err := eventFragment(part.name, part.msg)
		if err != nil {
			return nil, err
		}
		envelope.EventFragment = append(envelope.EventFragment, f)
	}
	return &eventsenderpb.PublishEventsRequest{Event: []*eventsenderpb.EventEnvelope{envelope}}, nil
}

// ReportPlay sends one finished play to Spotify's event receiver, or with
// dryRun returns the request without sending it. The event is sent exactly
// once: a transport failure is not retried (Spotify may have recorded it), so
// a play can be lost but is never counted twice.
func (m *Manager) ReportPlay(ctx context.Context, r PlayReport, dryRun bool) (*PlaylistRequest, error) {
	if err := r.validate(); err != nil {
		return nil, err
	}
	var preview *PlaylistRequest
	err := m.Do(ctx, func(env *Env) error {
		deviceID := ""
		m.mu.Lock()
		if m.creds != nil {
			deviceID = m.creds.DeviceID
		}
		m.mu.Unlock()

		seqID, seqNo, sessionID := playSequence.take()
		msg, err := buildPlayEvent(r, deviceID, seqID, seqNo, sessionID, randomBytes(16), time.Now())
		if err != nil {
			return &finalError{err}
		}
		req, err := previewMessage("POST", gaboEventsPath, msg, "the play event")
		if err != nil {
			return &finalError{err}
		}
		preview = req
		if dryRun {
			return nil
		}

		resp, err := sendOnce(ctx, env.Session.Spclient(), req, nil)
		if err != nil {
			return err
		}
		defer func() { _ = resp.Body.Close() }()
		body := readBody(resp)
		if resp.StatusCode != http.StatusOK {
			return writeFailure("the play event", resp.StatusCode, body)
		}
		var reply eventsenderpb.PublishEventsResponse
		if err := proto.Unmarshal(body, &reply); err == nil && len(reply.GetError()) > 0 {
			e := reply.GetError()[0]
			return &finalError{fmt.Errorf("Spotify refused the play event (reason %d, transient %v)",
				e.GetReason(), e.GetTransient())}
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	if !dryRun {
		m.log.Infof("reported play of %s (%d of %d ms, %s/%s)", r.URI, r.MsPlayed, r.DurationMs,
			r.ReasonStart, r.ReasonEnd)
	}
	return preview, nil
}
