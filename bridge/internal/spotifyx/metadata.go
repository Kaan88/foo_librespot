package spotifyx

import (
	"context"
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"
	"sync"

	librespot "github.com/devgianlu/go-librespot"
	connectpb "github.com/devgianlu/go-librespot/proto/spotify/connectstate"
	extmetadatapb "github.com/devgianlu/go-librespot/proto/spotify/extendedmetadata"
	audiofilespb "github.com/devgianlu/go-librespot/proto/spotify/extendedmetadata/audiofiles"
	metadatapb "github.com/devgianlu/go-librespot/proto/spotify/metadata"
	"github.com/devgianlu/go-librespot/spclient"

	"foo_librespot/bridge/internal/spotifyuri"
)

const (
	// metaBatchSize is how many entities one extended-metadata request asks
	// for; the backend refuses very large ones.
	metaBatchSize = 100
	// metaConcurrency bounds the requests in flight for one lookup.
	metaConcurrency = 4
	// maxResolveTracks caps how many tracks one context may expand to, so a
	// generated context cannot page forever.
	maxResolveTracks = 10000
	// collectionPageSize and collectionMaxPages mirror go-librespot's own
	// Liked Songs paging.
	collectionPageSize = 500
	collectionMaxPages = 100
)

// Lookup returns metadata for the given track URIs, batching and parallelising.
func (m *Manager) Lookup(ctx context.Context, uris []string) (map[string]*trackMeta, error) {
	var out map[string]*trackMeta
	err := m.Do(ctx, func(env *Env) error {
		var err error
		out, err = fetchTracks(ctx, env, uris)
		return err
	})
	return out, err
}

func fetchTracks(ctx context.Context, env *Env, uris []string) (map[string]*trackMeta, error) {
	unique := make([]string, 0, len(uris))
	seen := make(map[string]bool, len(uris))
	for _, uri := range uris {
		if uri == "" || seen[uri] || !strings.HasPrefix(uri, "spotify:track:") {
			continue
		}
		seen[uri] = true
		unique = append(unique, uri)
	}

	out := make(map[string]*trackMeta, len(unique))
	if len(unique) == 0 {
		return out, nil
	}

	batches := make([][]string, 0, (len(unique)+metaBatchSize-1)/metaBatchSize)
	for start := 0; start < len(unique); start += metaBatchSize {
		end := start + metaBatchSize
		if end > len(unique) {
			end = len(unique)
		}
		batches = append(batches, unique[start:end])
	}

	var (
		mu       sync.Mutex
		firstErr error
		wg       sync.WaitGroup
		sem      = make(chan struct{}, metaConcurrency)
	)
	for _, batch := range batches {
		if ctx.Err() != nil {
			break
		}
		wg.Add(1)
		sem <- struct{}{}
		go func(batch []string) {
			defer wg.Done()
			defer func() { <-sem }()

			found, err := fetchBatch(ctx, env, batch)
			mu.Lock()
			defer mu.Unlock()
			if err != nil {
				if firstErr == nil {
					firstErr = err
				}
				return
			}
			for uri, meta := range found {
				out[uri] = meta
			}
		}(batch)
	}
	wg.Wait()

	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return out, firstErr
}

func fetchBatch(ctx context.Context, env *Env, uris []string) (map[string]*trackMeta, error) {
	req := &extmetadatapb.BatchedEntityRequest{}
	for _, uri := range uris {
		req.EntityRequest = append(req.EntityRequest, &extmetadatapb.EntityRequest{
			EntityUri: uri,
			Query: []*extmetadatapb.ExtensionQuery{
				{ExtensionKind: extmetadatapb.ExtensionKind_TRACK_V4},
				{ExtensionKind: extmetadatapb.ExtensionKind_AUDIO_FILES},
			},
		})
	}

	resp, err := env.Session.Spclient().ExtendedMetadata(ctx, req)
	if err != nil {
		return nil, fmt.Errorf("fetching track metadata: %w", err)
	}

	out := make(map[string]*trackMeta, len(uris))
	for _, arr := range resp.GetExtendedMetadata() {
		for _, data := range arr.GetExtensionData() {
			if data.GetHeader().GetStatusCode() != 200 || data.GetExtensionData() == nil {
				continue
			}
			uri := data.GetEntityUri()
			meta := out[uri]
			if meta == nil {
				meta = &trackMeta{}
				out[uri] = meta
			}

			switch arr.GetExtensionKind() {
			case extmetadatapb.ExtensionKind_TRACK_V4:
				var track metadatapb.Track
				if err := data.GetExtensionData().UnmarshalTo(&track); err == nil {
					meta.Track = &track
				}
			case extmetadatapb.ExtensionKind_AUDIO_FILES:
				var audio audiofilespb.AudioFilesExtensionResponse
				if err := data.GetExtensionData().UnmarshalTo(&audio); err == nil {
					meta.Audio = &audio
				}
			}
		}
	}

	return out, nil
}

// Resolve expands a track, album, playlist or Liked Songs reference to its
// tracks, in playback order.
func (m *Manager) Resolve(ctx context.Context, ref spotifyuri.Ref) ([]TrackInfo, error) {
	var uris []string
	err := m.Do(ctx, func(env *Env) error {
		var err error
		if ref.Kind == spotifyuri.KindLiked {
			uris, err = likedURIs(ctx, env)
		} else {
			uris, err = contextURIs(ctx, env, m.log, ref)
		}
		return err
	})
	if err != nil {
		return nil, err
	}

	metas, err := m.Lookup(ctx, uris)
	if err != nil {
		return nil, err
	}

	infos := make([]TrackInfo, 0, len(uris))
	for _, uri := range uris {
		infos = append(infos, BuildTrackInfo(uri, metas[uri], 320))
	}
	return infos, nil
}

// Info returns metadata for individual tracks, in the order given.
func (m *Manager) Info(ctx context.Context, uris []string) ([]TrackInfo, error) {
	canonical := make([]string, len(uris))
	for i, raw := range uris {
		ref, err := spotifyuri.Parse(raw)
		if err == nil && ref.Kind == spotifyuri.KindTrack {
			canonical[i] = ref.URI
		} else {
			canonical[i] = raw
		}
	}

	metas, err := m.Lookup(ctx, canonical)
	if err != nil {
		return nil, err
	}

	infos := make([]TrackInfo, 0, len(canonical))
	for _, uri := range canonical {
		infos = append(infos, BuildTrackInfo(uri, metas[uri], 320))
	}
	return infos, nil
}

// contextURIs walks every page of a context (album, playlist, bare track).
//
// Playlists are read through the playlist service rather than context resolve:
// Spotify's generated playlists (Daily Mixes and the like) and playlists that
// mix episodes with tracks are not reliably described by context resolve,
// while the playlist service lists every item with its uid.
func contextURIs(ctx context.Context, env *Env, log librespot.Logger, ref spotifyuri.Ref) ([]string, error) {
	spotCtx := &connectpb.Context{Uri: ref.URI}
	if ref.Kind == spotifyuri.KindPlaylist {
		if id, err := librespot.SpotifyIdFromUri(ref.URI); err == nil {
			if playlistCtx, err := env.Session.Spclient().PlaylistContext(ctx, *id); err == nil {
				spotCtx = playlistCtx
			} else {
				log.WithField("uri", ref.URI).
					Warnf("reading the playlist directly failed (%v); falling back to context resolve", err)
			}
		}
	}

	resolver, err := spclient.NewContextResolver(ctx, log, env.Session.Spclient(), spotCtx)
	if err != nil {
		return nil, err
	}

	var uris []string
	for page := 0; ; page++ {
		tracks, err := resolver.Page(ctx, page)
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return nil, fmt.Errorf("resolving %s: %w", ref.URI, err)
		}
		for _, track := range tracks {
			if u := track.GetUri(); strings.HasPrefix(u, "spotify:track:") {
				uris = append(uris, u)
			}
		}
		if len(uris) > maxResolveTracks {
			return nil, fmt.Errorf("%s has more than %d tracks", ref.URI, maxResolveTracks)
		}
	}
	if len(uris) == 0 {
		return nil, fmt.Errorf("%s contains no tracks", ref.URI)
	}
	return uris, nil
}

// likedURIs pages through the account's Liked Songs, newest first.
func likedURIs(ctx context.Context, env *Env) ([]string, error) {
	username := env.Session.Username()
	type item struct {
		uri     string
		addedAt int64
	}
	var items []item
	token := ""
	for page := 0; ; page++ {
		if page >= collectionMaxPages {
			return nil, fmt.Errorf("Liked Songs still paging after %d pages", collectionMaxPages)
		}
		resp, err := env.Session.Spclient().CollectionPage(ctx, username, spclient.CollectionSetLikedSongs, token, collectionPageSize)
		if err != nil {
			return nil, fmt.Errorf("reading Liked Songs: %w", err)
		}
		for _, it := range resp.GetItems() {
			if it.GetIsRemoved() || !strings.HasPrefix(it.GetUri(), "spotify:track:") {
				continue
			}
			items = append(items, item{uri: it.GetUri(), addedAt: it.GetAddedAt()})
		}
		token = resp.GetNextPageToken()
		if token == "" {
			break
		}
	}

	sort.SliceStable(items, func(i, j int) bool { return items[i].addedAt > items[j].addedAt })
	uris := make([]string, 0, len(items))
	for _, it := range items {
		uris = append(uris, it.uri)
	}
	if len(uris) == 0 {
		return nil, errors.New("Liked Songs is empty")
	}
	return uris, nil
}
