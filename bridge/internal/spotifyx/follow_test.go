package spotifyx

import (
	"strings"
	"testing"
	"time"

	playlist4pb "github.com/devgianlu/go-librespot/proto/spotify/playlist4"
	"google.golang.org/protobuf/proto"
)

const (
	pA = "spotify:playlist:7xFpG0TMUI9eU6JBo1mt5S"
	pB = "spotify:playlist:37i9dQZF1E8M5Z6GDxnHFg"
)

func TestFollowTargets(t *testing.T) {
	got, err := followTargets([]string{
		"https://open.spotify.com/artist/5JWBow4ywgKNQ5HBxY8hcz?si=x",
		"spotify:album:1VDxaxECCJJh4of5htFnmK",
		pA,
	})
	if err != nil {
		t.Fatal(err)
	}
	want := []followTarget{
		{URI: "spotify:artist:5JWBow4ywgKNQ5HBxY8hcz", Kind: "artist"},
		{URI: "spotify:album:1VDxaxECCJJh4of5htFnmK", Kind: "album"},
		{URI: pA, Kind: "playlist"},
	}
	if len(got) != len(want) {
		t.Fatalf("targets = %v", got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("target %d = %+v, want %+v", i, got[i], want[i])
		}
	}

	for _, bad := range [][]string{nil, {tA}, {"spotify:user:u:collection"}, {"nonsense"}} {
		if _, err := followTargets(bad); !IsBadInput(err) {
			t.Errorf("followTargets(%v) err = %v, want bad input", bad, err)
		}
	}
}

func TestSplitAndFollowFlags(t *testing.T) {
	targets, err := followTargets([]string{
		"spotify:artist:5JWBow4ywgKNQ5HBxY8hcz", "spotify:album:1VDxaxECCJJh4of5htFnmK", pA, "spotify:artist:2cCUtGK9sDU2EoElnk0GNB", pB,
	})
	if err != nil {
		t.Fatal(err)
	}
	artists, albums, playlists := splitFollowTargets(targets)
	if len(artists) != 2 || len(albums) != 1 || len(playlists) != 2 {
		t.Fatalf("split = %d/%d/%d", len(artists), len(albums), len(playlists))
	}

	flags := followedFlags(targets,
		map[string]bool{"spotify:artist:5JWBow4ywgKNQ5HBxY8hcz": true},
		map[string]bool{"spotify:album:1VDxaxECCJJh4of5htFnmK": true},
		map[string]bool{pB: true})
	want := []bool{true, true, false, false, true}
	for i := range want {
		if flags[i] != want[i] {
			t.Errorf("flag %d = %v, want %v (all %v)", i, flags[i], want[i], flags)
		}
	}
}

func TestRootlistIndexAndMembership(t *testing.T) {
	rl := &playlist4pb.SelectedListContent{
		Contents: &playlist4pb.ListItems{Items: []*playlist4pb.Item{
			{Uri: proto.String("spotify:start-group:folder")},
			{Uri: proto.String(pA)},
			{Uri: proto.String(pB)},
			{Uri: proto.String("spotify:end-group:folder")},
		}},
	}
	if idx := rootlistIndex(rl, pB); idx != 2 {
		t.Errorf("index of pB = %d, want 2 (folder markers count)", idx)
	}
	if idx := rootlistIndex(rl, "spotify:playlist:nope"); idx != -1 {
		t.Errorf("index of a missing playlist = %d", idx)
	}
	members := rootlistMembership(rl)
	if !members[pA] || !members[pB] || len(members) != 2 {
		t.Errorf("membership = %v", members)
	}
	if rootlistIndex(nil, pA) != -1 || len(rootlistMembership(nil)) != 0 {
		t.Error("nil rootlist not handled")
	}
}

func TestBuildRootlistAdd(t *testing.T) {
	revision := []byte{0, 0, 0, 7, 0xaa}
	req, err := rootlistChangesRequest("u1", revision,
		[]*playlist4pb.Op{rootlistAddOp([]string{pA, pB})}, time.UnixMilli(1700000000123))
	if err != nil {
		t.Fatal(err)
	}
	if req.Method != "POST" || req.Path != "/playlist/v2/user/u1/rootlist/changes" {
		t.Errorf("request = %s %s", req.Method, req.Path)
	}
	if req.BodyLen != len(req.Body) || !strings.Contains(req.BodyText, pA) {
		t.Errorf("body text/len = %q / %d", req.BodyText, req.BodyLen)
	}

	var msg playlist4pb.ListChanges
	if err := proto.Unmarshal(req.Body, &msg); err != nil {
		t.Fatal(err)
	}
	if string(msg.GetBaseRevision()) != string(revision) || len(msg.GetDeltas()) != 1 {
		t.Fatalf("message = %v", &msg)
	}
	delta := msg.GetDeltas()[0]
	if delta.GetInfo().GetUser() != "u1" || delta.GetInfo().GetTimestamp() != 1700000000123 {
		t.Errorf("info = %v", delta.GetInfo())
	}
	op := delta.GetOps()[0]
	if op.GetKind() != playlist4pb.Op_ADD || !op.GetAdd().GetAddFirst() ||
		len(op.GetAdd().GetItems()) != 2 || op.GetAdd().GetItems()[1].GetUri() != pB {
		t.Errorf("op = %v", op)
	}
}

func TestRemoveByIndexOps(t *testing.T) {
	ops := removeByIndexOps([]int{2, 5, 5, -1})
	if len(ops) != 2 {
		t.Fatalf("ops = %v", ops)
	}
	if ops[0].GetKind() != playlist4pb.Op_REM || ops[0].GetRem().GetFromIndex() != 5 ||
		ops[0].GetRem().GetLength() != 1 {
		t.Errorf("first op = %v", ops[0])
	}
	if ops[1].GetRem().GetFromIndex() != 2 {
		t.Errorf("second op = %v", ops[1])
	}
	if len(removeByIndexOps(nil)) != 0 {
		t.Error("no indices produced ops")
	}
}
