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
	// defaultBitrate is the format assumed when a request names none.
	defaultBitrate = 320
)

// Lookup returns metadata for the given track URIs, batching, parallelising and
// caching. Only URIs whose metadata was found are in the reply.
func (m *Manager) Lookup(ctx context.Context, uris []string) (map[string]*trackMeta, error) {
	out := make(map[string]*trackMeta, len(uris))
	missing := make([]string, 0, len(uris))
	seen := make(map[string]bool, len(uris))
	for _, uri := range uris {
		if uri == "" || seen[uri] {
			continue
		}
		seen[uri] = true
		if meta, ok := m.trackMeta.get(uri); ok {
			out[uri] = meta
			continue
		}
		missing = append(missing, uri)
	}
	if len(missing) == 0 {
		return out, nil
	}

	var fetched map[string]*trackMeta
	err := m.Do(ctx, func(env *Env) error {
		var err error
		fetched, err = fetchTracks(ctx, env, missing)
		return err
	})
	if err != nil {
		return nil, err
	}
	for uri, meta := range fetched {
		if meta == nil || meta.Track == nil {
			continue
		}
		m.trackMeta.put(uri, meta)
		out[uri] = meta
	}
	return out, nil
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
// tracks, in playback order, and describes the context it expanded.
func (m *Manager) Resolve(ctx context.Context, ref spotifyuri.Ref) ([]TrackInfo, *ContextInfo, error) {
	var (
		uris []string
		info *ContextInfo
	)
	err := m.Do(ctx, func(env *Env) error {
		var err error
		uris, info, err = m.resolveURIs(ctx, env, ref)
		return err
	})
	if err != nil {
		return nil, nil, err
	}

	metas, err := m.Lookup(ctx, uris)
	if err != nil {
		return nil, nil, err
	}
	return m.buildInfos(uris, metas, defaultBitrate), info, nil
}

// Info returns metadata for individual tracks, in the order given. bitrate is
// the format the client would ask open for, so codec and bitrate describe the
// file that would be played.
func (m *Manager) Info(ctx context.Context, uris []string, bitrate int) ([]TrackInfo, error) {
	if bitrate == 0 {
		bitrate = defaultBitrate
	}
	switch bitrate {
	case 96, 160, 320:
	default:
		return nil, fmt.Errorf("bitrate must be 96, 160 or 320")
	}

	canonical := make([]string, len(uris))
	for i, raw := range uris {
		ref, err := spotifyuri.Parse(raw)
		if err == nil && ref.Kind == spotifyuri.KindTrack {
			canonical[i] = ref.URI
		} else {
			canonical[i] = raw
		}
	}

	infos := make([]TrackInfo, len(canonical))
	var missing []string
	positions := make(map[string][]int, len(canonical))
	for i, uri := range canonical {
		if info, ok := m.trackInfos.get(trackInfoKey{uri: uri, bitrate: bitrate}); ok {
			infos[i] = info
			continue
		}
		if len(positions[uri]) == 0 {
			missing = append(missing, uri)
		}
		positions[uri] = append(positions[uri], i)
	}

	if len(missing) > 0 {
		metas, err := m.Lookup(ctx, missing)
		if err != nil {
			return nil, err
		}
		for uri, at := range positions {
			if metas[uri] == nil {
				// Unknown or unplayable: describe it, but do not cache the
				// absence, as metadata may become available.
				for _, i := range at {
					infos[i] = BuildTrackInfo(uri, nil, bitrate)
				}
				continue
			}
			info := BuildTrackInfo(uri, metas[uri], bitrate)
			m.trackInfos.put(trackInfoKey{uri: uri, bitrate: bitrate}, info)
			for _, i := range at {
				infos[i] = info
			}
		}
	}
	return infos, nil
}

// buildInfos maps metadata to protocol track infos, caching each result.
func (m *Manager) buildInfos(uris []string, metas map[string]*trackMeta, bitrate int) []TrackInfo {
	infos := make([]TrackInfo, 0, len(uris))
	for _, uri := range uris {
		key := trackInfoKey{uri: uri, bitrate: bitrate}
		if info, ok := m.trackInfos.get(key); ok {
			infos = append(infos, info)
			continue
		}
		info := BuildTrackInfo(uri, metas[uri], bitrate)
		if metas[uri] != nil {
			m.trackInfos.put(key, info)
		}
		infos = append(infos, info)
	}
	return infos
}

func (m *Manager) resolveURIs(ctx context.Context, env *Env, ref spotifyuri.Ref) ([]string, *ContextInfo, error) {
	switch ref.Kind {
	case spotifyuri.KindLiked:
		uris, err := likedURIs(ctx, env)
		if err != nil {
			return nil, nil, err
		}
		username := env.Session.Username()
		return uris, &ContextInfo{
			URI:        "spotify:user:" + username + ":collection",
			Name:       "Liked Songs",
			Owner:      username,
			Revision:   revisionHash(uris),
			TrackCount: len(uris),
		}, nil

	case spotifyuri.KindPlaylist:
		return m.playlistContext(ctx, env, ref)

	case spotifyuri.KindAlbum:
		uris, err := contextURIs(ctx, env, m.log, ref)
		if err != nil {
			return nil, nil, err
		}
		item := m.albumItems(ctx, env, []string{ref.URI})[ref.URI]
		info := &ContextInfo{
			URI:        ref.URI,
			Name:       item.Name,
			Owner:      item.Subtitle,
			Revision:   revisionHash(uris),
			TrackCount: len(uris),
		}
		if item.ImageURL != nil {
			info.ImageURL = *item.ImageURL
		}
		return uris, info, nil

	case spotifyuri.KindTrack:
		uris, err := contextURIs(ctx, env, m.log, ref)
		if err != nil {
			return nil, nil, err
		}
		return uris, nil, nil

	default:
		return nil, nil, fmt.Errorf("cannot resolve a %s", ref.Kind)
	}
}

// playlistContext reads a playlist through the playlist service: every page of
// tracks, plus the playlist's name, owner, image and revision.
func (m *Manager) playlistContext(ctx context.Context, env *Env, ref spotifyuri.Ref) ([]string, *ContextInfo, error) {
	info := &ContextInfo{URI: ref.URI}

	var spotCtx *connectpb.Context
	if id, err := librespot.SpotifyIdFromUri(ref.URI); err == nil {
		if playlistCtx, err := env.Session.Spclient().PlaylistContext(ctx, *id); err == nil {
			spotCtx = playlistCtx
		} else {
			m.log.WithField("uri", ref.URI).
				Warnf("reading the playlist directly failed (%v); falling back to context resolve", err)
		}
	}
	if spotCtx == nil {
		uris, err := walkContext(ctx, env, m.log, &connectpb.Context{Uri: ref.URI}, ref.URI)
		if err != nil {
			return nil, nil, err
		}
		info.Revision = revisionHash(uris)
		info.TrackCount = len(uris)
		return uris, info, nil
	}

	metadata := spotCtx.GetMetadata()
	info.Name = metadata["context_description"]
	info.Owner = metadata["context_owner"]
	info.ImageURL = metadata["image_url"]

	uris, err := walkContext(ctx, env, m.log, spotCtx, ref.URI)
	if err != nil {
		return nil, nil, err
	}

	info.Revision = contextRevision(metadata["playlist.revision"], uris)
	info.TrackCount = len(uris)
	return uris, info, nil
}

// contextURIs walks every page of a context (album, bare track).
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
	return walkContext(ctx, env, log, spotCtx, ref.URI)
}

// walkContext pages through a context and returns its track URIs in order.
func walkContext(ctx context.Context, env *Env, log librespot.Logger, spotCtx *connectpb.Context, uri string) ([]string, error) {
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
			return nil, fmt.Errorf("resolving %s: %w", uri, err)
		}
		for _, track := range tracks {
			if u := track.GetUri(); strings.HasPrefix(u, "spotify:track:") {
				uris = append(uris, u)
			}
		}
		if len(uris) > maxResolveTracks {
			return nil, fmt.Errorf("%s has more than %d tracks", uri, maxResolveTracks)
		}
	}
	if len(uris) == 0 {
		return nil, fmt.Errorf("%s contains no tracks", uri)
	}
	return uris, nil
}

// collectionEntry is one live item of a collection set.
type collectionEntry struct {
	uri     string
	addedAt int64
}

// fetchCollection pages through one collection set. The set "collection" holds
// both Liked Songs and saved albums; "artist" holds the followed artists.
func fetchCollection(ctx context.Context, env *Env, set string) ([]collectionEntry, error) {
	username := env.Session.Username()
	var entries []collectionEntry
	token := ""
	for page := 0; ; page++ {
		if page >= collectionMaxPages {
			return nil, fmt.Errorf("collection set %s still paging after %d pages", set, collectionMaxPages)
		}
		resp, err := env.Session.Spclient().CollectionPage(ctx, username, set, token, collectionPageSize)
		if err != nil {
			return nil, fmt.Errorf("reading collection %s: %w", set, err)
		}
		for _, it := range resp.GetItems() {
			if it.GetIsRemoved() || it.GetUri() == "" {
				continue
			}
			entries = append(entries, collectionEntry{uri: it.GetUri(), addedAt: it.GetAddedAt()})
		}
		token = resp.GetNextPageToken()
		if token == "" {
			break
		}
	}
	return entries, nil
}

// collectionURIs returns the URIs of one entity type, most recently added
// first.
func collectionURIs(entries []collectionEntry, prefix string) []string {
	var kept []collectionEntry
	for _, entry := range entries {
		if strings.HasPrefix(entry.uri, prefix) {
			kept = append(kept, entry)
		}
	}
	sort.SliceStable(kept, func(i, j int) bool { return kept[i].addedAt > kept[j].addedAt })

	uris := make([]string, len(kept))
	for i, entry := range kept {
		uris[i] = entry.uri
	}
	return uris
}

// likedURIs pages through the account's Liked Songs, newest first.
func likedURIs(ctx context.Context, env *Env) ([]string, error) {
	entries, err := fetchCollection(ctx, env, spclient.CollectionSetLikedSongs)
	if err != nil {
		return nil, err
	}
	uris := collectionURIs(entries, "spotify:track:")
	if len(uris) == 0 {
		return nil, errors.New("Liked Songs is empty")
	}
	return uris, nil
}
