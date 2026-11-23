package bridge

import (
	"encoding/json"
	"errors"
	"testing"

	"foo_librespot/bridge/internal/spotifyx"
)

func TestCodeForWriteErrors(t *testing.T) {
	if got := codeFor(spotifyx.Unsupportedf("Spotify rejected it (HTTP 403)")); got != CodeUnsupported {
		t.Errorf("unsupported -> %q", got)
	}
	if got := codeFor(&spotifyx.ErrBadInput{Msg: "no"}); got != CodeBadRequest {
		t.Errorf("bad input -> %q", got)
	}
	if got := codeFor(errors.New("boom")); got != CodeInternal {
		t.Errorf("other -> %q", got)
	}
}

func TestPlaylistAddResponseShape(t *testing.T) {
	raw, _ := json.Marshal(playlistAddResponse{OK: true, Revision: "00000007"})
	if string(raw) != `{"ok":true,"revision":"00000007"}` {
		t.Errorf("live reply = %s", raw)
	}
	raw, _ = json.Marshal(savedResponse{OK: true, Saved: []bool{true, false}})
	if string(raw) != `{"ok":true,"saved":[true,false]}` {
		t.Errorf("saved reply = %s", raw)
	}
	var req request
	if err := json.Unmarshal([]byte(`{"op":"playlist_add","playlist":"spotify:playlist:x","uris":["a"],"dry_run":true}`), &req); err != nil ||
		req.Playlist != "spotify:playlist:x" || !req.DryRun || len(req.URIs) != 1 {
		t.Errorf("request = %+v, %v", req, err)
	}
}
