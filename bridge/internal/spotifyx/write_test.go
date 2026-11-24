package spotifyx

import (
	"bytes"
	"errors"
	"strings"
	"testing"
	"time"

	collectionpb "github.com/devgianlu/go-librespot/proto/spotify/collection/v2"
	playlist4pb "github.com/devgianlu/go-librespot/proto/spotify/playlist4"
	"google.golang.org/protobuf/proto"
)

const (
	tA = "spotify:track:0VjIjW4GlUZAMYd2vXMi3b"
	tB = "spotify:track:4uLU6hMCjMI75M1A2tKUQC"
)

func TestTrackURIs(t *testing.T) {
	got, err := trackURIs([]string{tA, "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC?si=x"})
	if err != nil || len(got) != 2 || got[1] != tB {
		t.Fatalf("trackURIs = %v, %v", got, err)
	}
	for _, bad := range [][]string{nil, {"spotify:album:1VDxaxECCJJh4of5htFnmK"}, {"nonsense"}} {
		if _, err := trackURIs(bad); !IsBadInput(err) {
			t.Errorf("trackURIs(%v) err = %v, want bad input", bad, err)
		}
	}
}

func TestBuildCollectionWrite(t *testing.T) {
	now := time.Unix(1700000000, 0)
	body, err := buildCollectionWrite("u1", "collection", []string{tA}, false, now)
	if err != nil {
		t.Fatal(err)
	}
	var req collectionpb.WriteRequest
	if err := proto.Unmarshal(body, &req); err != nil {
		t.Fatal(err)
	}
	if req.GetUsername() != "u1" || req.GetSet() != "collection" || len(req.GetItems()) != 1 {
		t.Fatalf("request = %v", &req)
	}
	it := req.GetItems()[0]
	if it.GetUri() != tA || it.GetIsRemoved() || it.GetAddedAt() != 1700000000 {
		t.Errorf("save item = %v", it)
	}

	body, _ = buildCollectionWrite("u1", "collection", []string{tA}, true, now)
	req.Reset()
	_ = proto.Unmarshal(body, &req)
	if it := req.GetItems()[0]; !it.GetIsRemoved() || it.GetAddedAt() != 0 {
		t.Errorf("unsave item = %v", it)
	}
}

func TestContainsRoundTrip(t *testing.T) {
	body, err := buildCollectionContains("u1", "collection", []string{tA, tB})
	if err != nil {
		t.Fatal(err)
	}
	var req collectionpb.ContainsRequest
	if err := proto.Unmarshal(body, &req); err != nil || len(req.GetItems()) != 2 || req.GetSet() != "collection" {
		t.Fatalf("request = %v, %v", &req, err)
	}

	resp, _ := proto.Marshal(&collectionpb.ContainsResponse{Found: []bool{true, false}})
	found, err := parseContains(resp, 2)
	if err != nil || len(found) != 2 || !found[0] || found[1] {
		t.Errorf("found = %v, %v", found, err)
	}
	if _, err := parseContains(resp, 3); err == nil {
		t.Error("length mismatch accepted")
	}
	if _, err := parseContains([]byte{0xff, 0xff}, 1); err == nil {
		t.Error("garbage accepted")
	}
}

func TestBuildPlaylistAppend(t *testing.T) {
	rev := []byte{0, 0, 0, 7, 0xaa}
	req, err := buildPlaylistAppend("7xFpG0TMUI9eU6JBo1mt5S", "u1", rev, []string{tA, tB}, time.UnixMilli(1700000000123))
	if err != nil {
		t.Fatal(err)
	}
	if req.Method != "POST" || req.Path != "/playlist/v2/playlist/7xFpG0TMUI9eU6JBo1mt5S/changes" {
		t.Errorf("request = %s %s", req.Method, req.Path)
	}
	var msg playlist4pb.ListChanges
	if err := proto.Unmarshal(req.Body, &msg); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(msg.GetBaseRevision(), rev) || len(msg.GetDeltas()) != 1 {
		t.Fatalf("message = %v", &msg)
	}
	d := msg.GetDeltas()[0]
	if d.GetInfo().GetUser() != "u1" || d.GetInfo().GetTimestamp() != 1700000000123 {
		t.Errorf("info = %v", d.GetInfo())
	}
	op := d.GetOps()[0]
	if op.GetKind() != playlist4pb.Op_ADD || !op.GetAdd().GetAddLast() || len(op.GetAdd().GetItems()) != 2 ||
		op.GetAdd().GetItems()[1].GetUri() != tB {
		t.Errorf("op = %v", op)
	}
	if req.BodyLen != len(req.Body) || !strings.Contains(req.BodyText, tA) {
		t.Errorf("body text/len = %q / %d", req.BodyText, req.BodyLen)
	}
}

func TestRejected(t *testing.T) {
	err := rejected("the Liked Songs change", 403, []byte(" nope \n"))
	if !IsUnsupported(err) || !strings.Contains(err.Error(), "403") || !strings.Contains(err.Error(), "nope") {
		t.Errorf("err = %v", err)
	}
	if err := rejected("x", 403, nil); !IsUnsupported(err) || strings.Contains(err.Error(), ": ") {
		t.Errorf("empty body err = %v", err)
	}
	long := rejected("x", 400, bytes.Repeat([]byte("a"), 1000))
	if len(long.Error()) > 400 {
		t.Errorf("long body not trimmed: %d", len(long.Error()))
	}
}

func TestIsPermanentForWrites(t *testing.T) {
	if !IsPermanent(badInputf("x")) || !IsPermanent(&finalError{errors.New("500")}) {
		t.Error("bad input and final errors must not trigger a session reset")
	}
}
