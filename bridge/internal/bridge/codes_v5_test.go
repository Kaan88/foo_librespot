package bridge

import (
	"encoding/json"
	"strings"
	"testing"

	"foo_librespot/bridge/internal/spotifyx"
)

func TestV5RequestParsing(t *testing.T) {
	var req request
	if err := json.Unmarshal([]byte(`{"op":"follow","uris":["spotify:artist:a","spotify:playlist:b"],"dry_run":true}`), &req); err != nil ||
		req.Op != "follow" || len(req.URIs) != 2 || !req.DryRun {
		t.Errorf("follow request = %+v, %v", req, err)
	}

	req = request{}
	if err := json.Unmarshal([]byte(`{"op":"playlist_remove","playlist":"spotify:playlist:x","items":[{"uri":"spotify:track:a","index":3},{"uri":"spotify:track:b"}]}`), &req); err != nil ||
		req.Playlist != "spotify:playlist:x" || len(req.Items) != 2 {
		t.Fatalf("playlist_remove request = %+v, %v", req, err)
	}
	if req.Items[0].Index == nil || *req.Items[0].Index != 3 || req.Items[1].Index != nil {
		t.Errorf("items = %+v", req.Items)
	}

	req = request{}
	if err := json.Unmarshal([]byte(`{"op":"playlist_create","name":"My Mix","dry_run":true}`), &req); err != nil ||
		req.Name != "My Mix" || !req.DryRun {
		t.Errorf("playlist_create request = %+v, %v", req, err)
	}
}

func TestV5ResponseShapes(t *testing.T) {
	raw, _ := json.Marshal(followResponse{OK: true, Following: []bool{true, false}})
	if string(raw) != `{"ok":true,"following":[true,false]}` {
		t.Errorf("following reply = %s", raw)
	}

	raw, _ = json.Marshal(followWriteResponse{OK: true})
	if string(raw) != `{"ok":true}` {
		t.Errorf("follow reply = %s", raw)
	}

	raw, _ = json.Marshal(followWriteResponse{OK: true, DryRun: true, Requests: []*spotifyx.PlaylistRequest{
		{Method: "POST", Path: "/collection/v2/write", BodyText: "x", BodyLen: 1},
	}})
	want := `{"ok":true,"dry_run":true,"requests":[{"method":"POST","path":"/collection/v2/write","body_text":"x","body_len":1}]}`
	if string(raw) != want {
		t.Errorf("follow dry run = %s", raw)
	}

	raw, _ = json.Marshal(playlistChangeResponse{OK: true, Revision: "00", DryRun: true, Request: &spotifyx.PlaylistRequest{
		Method: "POST", Path: "/playlist/v2/playlist/x/changes", BodyText: "y", BodyLen: 2,
	}})
	want = `{"ok":true,"revision":"00","dry_run":true,"request":{"method":"POST","path":"/playlist/v2/playlist/x/changes","body_text":"y","body_len":2}}`
	if string(raw) != want {
		t.Errorf("playlist change dry run = %s", raw)
	}

	raw, _ = json.Marshal(playlistCreateResponse{OK: true, DryRun: true, Requests: []*spotifyx.PlaylistRequest{
		{Method: "POST", Path: "/playlist/v2/playlist", BodyText: "z", BodyLen: 3},
	}})
	if !strings.Contains(string(raw), `"dry_run":true`) || !strings.Contains(string(raw), `"requests":`) ||
		strings.Contains(string(raw), `"playlist"`) {
		t.Errorf("playlist create dry run = %s", raw)
	}
}
