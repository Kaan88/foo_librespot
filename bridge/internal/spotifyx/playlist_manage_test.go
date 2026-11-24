package spotifyx

import (
	"strings"
	"testing"
	"time"

	playlist4pb "github.com/devgianlu/go-librespot/proto/spotify/playlist4"
	"google.golang.org/protobuf/proto"
)

func TestPlaylistName(t *testing.T) {
	if got, err := playlistName("  My Mix  "); err != nil || got != "My Mix" {
		t.Errorf("name = %q, %v", got, err)
	}
	for _, bad := range []string{"", "   ", strings.Repeat("a", maxPlaylistNameBytes+1)} {
		if _, err := playlistName(bad); !IsBadInput(err) {
			t.Errorf("playlistName(%q) err = %v", bad, err)
		}
	}
}

func TestPlaylistRef(t *testing.T) {
	id, base62, err := playlistRef(pA)
	if err != nil || id.Base62() != "7xFpG0TMUI9eU6JBo1mt5S" || base62 != "7xFpG0TMUI9eU6JBo1mt5S" {
		t.Errorf("playlistRef = %v, %q, %v", id, base62, err)
	}
	for _, bad := range []string{tA, "spotify:artist:x", "nonsense"} {
		if _, _, err := playlistRef(bad); !IsBadInput(err) {
			t.Errorf("playlistRef(%q) err = %v", bad, err)
		}
	}
}

func TestBuildPlaylistCreate(t *testing.T) {
	req, err := buildPlaylistCreate("My Mix", "u1", time.UnixMilli(1700000000123))
	if err != nil {
		t.Fatal(err)
	}
	if req.Method != "POST" || req.Path != "/playlist/v2/playlist" {
		t.Errorf("request = %s %s", req.Method, req.Path)
	}
	if req.BodyLen != len(req.Body) || !strings.Contains(req.BodyText, "My Mix") {
		t.Errorf("body text/len = %q / %d", req.BodyText, req.BodyLen)
	}
	var msg playlist4pb.ListUpdateRequest
	if err := proto.Unmarshal(req.Body, &msg); err != nil {
		t.Fatal(err)
	}
	if msg.GetAttributes().GetName() != "My Mix" || msg.GetInfo().GetUser() != "u1" ||
		msg.GetInfo().GetTimestamp() != 1700000000123 {
		t.Errorf("message = %v", &msg)
	}
}

func TestParseCreateListReply(t *testing.T) {
	body, _ := proto.Marshal(&playlist4pb.CreateListReply{
		Uri:      proto.String(pA),
		Revision: []byte{0, 0, 0, 7},
	})
	uri, revision, err := parseCreateListReply(body)
	if err != nil || uri != pA || string(revision) != "\x00\x00\x00\x07" {
		t.Errorf("reply = %q, %v, %v", uri, revision, err)
	}
	for _, bad := range [][]byte{
		{0xff, 0xff},
		mustMarshal(&playlist4pb.CreateListReply{Uri: proto.String(tA)}),
		mustMarshal(&playlist4pb.CreateListReply{}),
	} {
		if _, _, err := parseCreateListReply(bad); err == nil {
			t.Errorf("parseCreateListReply(%v) accepted it", bad)
		}
	}
}

func mustMarshal(msg proto.Message) []byte {
	body, _ := proto.Marshal(msg)
	return body
}

func TestBuildPlaylistRename(t *testing.T) {
	revision := []byte{0, 0, 0, 7, 0xbb}
	req, err := buildPlaylistRename("7xFpG0TMUI9eU6JBo1mt5S", "u1", "New name", revision, time.UnixMilli(1700000000456))
	if err != nil {
		t.Fatal(err)
	}
	if req.Path != "/playlist/v2/playlist/7xFpG0TMUI9eU6JBo1mt5S/changes" {
		t.Errorf("path = %s", req.Path)
	}
	var msg playlist4pb.ListChanges
	if err := proto.Unmarshal(req.Body, &msg); err != nil {
		t.Fatal(err)
	}
	if string(msg.GetBaseRevision()) != string(revision) {
		t.Errorf("base revision = %v", msg.GetBaseRevision())
	}
	op := msg.GetDeltas()[0].GetOps()[0]
	if op.GetKind() != playlist4pb.Op_UPDATE_LIST_ATTRIBUTES ||
		op.GetUpdateListAttributes().GetNewAttributes().GetValues().GetName() != "New name" {
		t.Errorf("op = %v", op)
	}
	if msg.GetDeltas()[0].GetInfo().GetTimestamp() != 1700000000456 {
		t.Errorf("info = %v", msg.GetDeltas()[0].GetInfo())
	}
}

func TestPlaylistItemRefs(t *testing.T) {
	got, err := playlistItemRefs([]PlaylistItemRef{
		{URI: "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC", Index: 3},
		{URI: tA, Index: 0},
	})
	if err != nil || len(got) != 2 || got[0].URI != tB || got[1].Index != 0 {
		t.Fatalf("refs = %v, %v", got, err)
	}
	for _, bad := range [][]PlaylistItemRef{
		nil,
		{{URI: "spotify:album:1VDxaxECCJJh4of5htFnmK", Index: 0}},
		{{URI: tA, Index: -1}},
		{{URI: tA, Index: 2}, {URI: tB, Index: 2}},
	} {
		if _, err := playlistItemRefs(bad); !IsBadInput(err) {
			t.Errorf("playlistItemRefs(%v) err = %v", bad, err)
		}
	}
}

func TestValidateRemoval(t *testing.T) {
	contents := []string{tA, tB, tA}
	indices, err := validateRemoval(contents, []PlaylistItemRef{{URI: tA, Index: 0}, {URI: tB, Index: 1}})
	if err != nil || len(indices) != 2 || indices[0] != 1 || indices[1] != 0 {
		t.Errorf("indices = %v, %v (want highest first)", indices, err)
	}

	// tB is in the playlist once: a wrong index falls back to its position.
	if indices, err := validateRemoval(contents, []PlaylistItemRef{{URI: tB, Index: 0}}); err != nil ||
		len(indices) != 1 || indices[0] != 1 {
		t.Errorf("unique uri at a wrong index = %v, %v (want [1])", indices, err)
	}
	// tA is there twice and not at index 1: ambiguous, refused.
	if _, err := validateRemoval(contents, []PlaylistItemRef{{URI: tA, Index: 1}}); !IsBadInput(err) {
		t.Errorf("ambiguous uri accepted: %v", err)
	}
	if _, err := validateRemoval(contents, []PlaylistItemRef{{URI: tA, Index: 3}}); !IsBadInput(err) {
		t.Errorf("ambiguous uri past the end accepted: %v", err)
	}
	// Not in the playlist at all.
	if _, err := validateRemoval([]string{tA}, []PlaylistItemRef{{URI: tB, Index: 0}}); !IsBadInput(err) {
		t.Errorf("missing uri accepted: %v", err)
	}
	// Two requests resolving to the same position.
	if _, err := validateRemoval([]string{tA, tB}, []PlaylistItemRef{{URI: tB, Index: 1}, {URI: tB, Index: 0}}); !IsBadInput(err) {
		t.Errorf("same position twice accepted: %v", err)
	}
}

func TestBuildPlaylistRemove(t *testing.T) {
	revision := []byte{0, 0, 0, 9}
	req, err := buildPlaylistRemove("7xFpG0TMUI9eU6JBo1mt5S", "u1", revision, []int{3, 1}, time.UnixMilli(1))
	if err != nil {
		t.Fatal(err)
	}
	if req.Path != "/playlist/v2/playlist/7xFpG0TMUI9eU6JBo1mt5S/changes" {
		t.Errorf("path = %s", req.Path)
	}
	var msg playlist4pb.ListChanges
	if err := proto.Unmarshal(req.Body, &msg); err != nil {
		t.Fatal(err)
	}
	ops := msg.GetDeltas()[0].GetOps()
	if len(ops) != 2 || ops[0].GetRem().GetFromIndex() != 3 || ops[1].GetRem().GetFromIndex() != 1 {
		t.Errorf("ops = %v", ops)
	}
	if string(msg.GetBaseRevision()) != string(revision) {
		t.Errorf("base revision = %v", msg.GetBaseRevision())
	}
}
