package spotifyx

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	connectpb "github.com/devgianlu/go-librespot/proto/spotify/connectstate"
	extmetadatapb "github.com/devgianlu/go-librespot/proto/spotify/extendedmetadata"
	metadatapb "github.com/devgianlu/go-librespot/proto/spotify/metadata"
	playlist4pb "github.com/devgianlu/go-librespot/proto/spotify/playlist4"
	"github.com/devgianlu/go-librespot/spclient"

	"foo_librespot/bridge/internal/spotifyuri"
)

const (
	// libraryCacheTTL is how long one library reply is reused: opening the
	// Spotify tab again should not hit Spotify again.
	libraryCacheTTL = 30 * time.Second
	// artistCacheTTL keeps an artist page warm for its next visit.
	artistCacheTTL = 5 * time.Minute
	// artistCacheSize bounds the number of cached artist pages.
	artistCacheSize = 128
	// rootlistPageSize is how many rootlist entries one request asks for.
	rootlistPageSize = 200
	// maxRootlistEntries bounds the rootlist walk.
	maxRootlistEntries = 5000
	// maxSearchTracks is what the search context resolves to; asking for more
	// cannot return more.
	maxSearchTracks = 20
	// maxArtistTopTracks bounds the top tracks of an artist page.
	maxArtistTopTracks = 20
	// maxArtistReleases bounds the albums and singles of an artist page.
	maxArtistReleases = 200
)

// SearchResult is one search reply: each list is capped by the request's limit.
type SearchResult struct {
	Tracks    []TrackInfo
	Albums    []Item
	Artists   []Item
	Playlists []Item
}

// ArtistPage is one artist page.
type ArtistPage struct {
	Artist    Item
	TopTracks []TrackInfo
	Albums    []Item
	Singles   []Item
}

// UnsupportedError is a feature or entity the Spotify session cannot serve.
// It is a definitive answer, so it never causes a session recreation.
type UnsupportedError struct {
	Msg string
}

func (e *UnsupportedError) Error() string { return e.Msg }

// Unsupportedf builds an UnsupportedError.
func Unsupportedf(format string, args ...any) error {
	return &UnsupportedError{Msg: fmt.Sprintf(format, args...)}
}

// IsUnsupported reports whether err means the feature or entity is not
// available to this session.
func IsUnsupported(err error) bool {
	var ue *UnsupportedError
	return errors.As(err, &ue)
}

// Library returns the contents of the Spotify tab: Liked Songs, made-for-you
// playlists, the user's playlists, saved albums and followed artists. Every
// section is present, with an empty item list when its source is empty.
func (m *Manager) Library(ctx context.Context) ([]Section, error) {
	if sections, ok := m.cachedLibrary(); ok {
		m.log.Debugf("library answered from cache (%d sections)", len(sections))
		return sections, nil
	}

	start := time.Now()
	var sections []Section
	err := m.Do(ctx, func(env *Env) error {
		var err error
		sections, err = m.buildLibrary(ctx, env)
		return err
	})
	if err != nil {
		return nil, err
	}

	m.storeLibrary(sections)
	m.log.Infof("library: %s in %s", describeSections(sections), time.Since(start).Round(time.Millisecond))
	return sections, nil
}

func (m *Manager) buildLibrary(ctx context.Context, env *Env) ([]Section, error) {
	username := env.Session.Username()

	var (
		collection, artistSet []collectionEntry
		rootlist              *playlist4pb.SelectedListContent
		recentURIs            []string
		collErr, artistErr    error
		rootErr, recentErr    error
	)
	var wg sync.WaitGroup
	wg.Add(4)
	go func() {
		defer wg.Done()
		collection, collErr = fetchCollection(ctx, env, spclient.CollectionSetLikedSongs)
	}()
	go func() {
		defer wg.Done()
		artistSet, artistErr = fetchCollection(ctx, env, spclient.CollectionSetArtists)
	}()
	go func() {
		defer wg.Done()
		rootlist, rootErr = fetchRootlist(ctx, env, username)
	}()
	go func() {
		defer wg.Done()
		recentURIs, recentErr = fetchRecentlyPlayed(ctx, env, username)
	}()
	wg.Wait()

	if collErr != nil && artistErr != nil && rootErr != nil {
		return nil, collErr
	}
	if collErr != nil {
		m.log.WithError(collErr).Warnf("library: reading Liked Songs and saved albums failed")
	}
	if artistErr != nil {
		m.log.WithError(artistErr).Warnf("library: reading followed artists failed")
	}
	if rootErr != nil {
		m.log.WithError(rootErr).Warnf("library: reading the playlist rootlist failed")
	}
	if recentErr != nil {
		m.log.WithError(recentErr).Warnf("library: reading recently played failed")
	}

	liked := collectionURIs(collection, "spotify:track:")
	albumURIs := collectionURIs(collection, "spotify:album:")
	artistURIs := collectionURIs(artistSet, "spotify:artist:")
	madeFor, playlists := splitRootlist(rootlist, username)

	albumByURI := m.albumItems(ctx, env, albumURIs)
	artistByURI := m.artistItems(ctx, env, artistURIs)

	// Top tracks and top artists have no source reachable from this session:
	// the TOPTracks/TOPARTISTS context upstream (hm://context-user/...) is not
	// deployed (context resolve answers 404 "Could not find an upstream
	// service"), the user-profile-view top routes answer 404, the profile view
	// carries no top lists even with artist_limit, and the Web API answers 429.
	// The sections stay present, empty, so the client can render them the same
	// way whatever the account can serve.
	m.log.Debugf("library: top tracks/artists have no reachable source on this login")

	return []Section{
		{ID: "liked", Title: "Liked Songs", Items: []Item{likedItem(username, liked)}},
		{ID: "made_for_you", Title: "Made for you", Items: madeFor},
		{ID: "playlists", Title: "Your playlists", Items: playlists},
		{ID: "albums", Title: "Albums", Items: itemsOrdered(albumURIs, albumByURI)},
		{ID: "artists", Title: "Artists", Items: itemsOrdered(artistURIs, artistByURI)},
		{ID: "recently_played", Title: "Recently played", Items: m.recentlyPlayedItems(ctx, env, recentURIs, username)},
		{ID: "top_tracks", Title: "Top tracks", Items: []Item{}},
		{ID: "top_artists", Title: "Top artists", Items: []Item{}},
	}, nil
}

// fetchRootlist walks the user's rootlist. Items and their decorated metadata
// come back in parallel slices; folders are skipped by splitRootlist. Every
// page reports the whole list's revision; the first one is kept because
// rootlist changes have to be based on it.
func fetchRootlist(ctx context.Context, env *Env, username string) (*playlist4pb.SelectedListContent, error) {
	all := &playlist4pb.SelectedListContent{Contents: &playlist4pb.ListItems{}}

	from := 0
	for {
		page, err := env.Session.Spclient().Rootlist(ctx, username, from, rootlistPageSize)
		if err != nil {
			return nil, err
		}
		items := page.GetContents().GetItems()
		all.Contents.Items = append(all.Contents.Items, items...)
		all.Contents.MetaItems = append(all.Contents.MetaItems, page.GetContents().GetMetaItems()...)
		if len(all.GetRevision()) == 0 && len(page.GetRevision()) > 0 {
			all.Revision = page.GetRevision()
		}

		from += len(items)
		if len(items) == 0 || from >= int(page.GetLength()) || from >= maxRootlistEntries {
			break
		}
	}
	return all, nil
}

func itemsOrdered(uris []string, byURI map[string]Item) []Item {
	out := make([]Item, 0, len(uris))
	for _, uri := range uris {
		out = append(out, byURI[uri])
	}
	return out
}

// Search asks Spotify for tracks matching query and describes the albums and
// artists those tracks belong to. Spotify's internal search context only lists
// tracks; the playlists come from Spotify's search service, the albums and
// artists are derived from the matching tracks.
func (m *Manager) Search(ctx context.Context, query string, limit int) (*SearchResult, error) {
	query = strings.TrimSpace(query)
	if query == "" {
		return nil, fmt.Errorf("search query is empty")
	}
	if limit <= 0 {
		limit = maxSearchTracks
	}
	if limit > 50 {
		limit = 50
	}

	start := time.Now()
	var uris []string
	err := m.Do(ctx, func(env *Env) error {
		var err error
		uris, err = searchURIs(ctx, env, query)
		return err
	})
	if err != nil {
		return nil, err
	}
	if len(uris) > maxSearchTracks {
		uris = uris[:maxSearchTracks]
	}

	metas, err := m.Lookup(ctx, uris)
	if err != nil {
		return nil, err
	}
	// The albums and artists come from every track Spotify returned, capped by
	// the request's limit; the track list itself is capped the same way.
	albumURIs, artistURIs := relatedURIs(uris, metas)
	if len(albumURIs) > limit {
		albumURIs = albumURIs[:limit]
	}
	if len(artistURIs) > limit {
		artistURIs = artistURIs[:limit]
	}
	replyURIs := uris
	if len(replyURIs) > limit {
		replyURIs = replyURIs[:limit]
	}
	tracks := m.buildInfos(replyURIs, metas, defaultBitrate)

	var albumByURI, artistByURI map[string]Item
	var playlists []Item
	err = m.Do(ctx, func(env *Env) error {
		albumByURI = m.albumItems(ctx, env, albumURIs)
		artistByURI = m.artistItems(ctx, env, artistURIs)
		playlists = m.searchPlaylistItems(ctx, env, query, limit)
		return nil
	})
	if err != nil {
		return nil, err
	}

	m.log.Debugf("search %q: %d tracks, %d albums, %d artists, %d playlists in %s",
		query, len(tracks), len(albumURIs), len(artistURIs), len(playlists), time.Since(start).Round(time.Millisecond))

	return &SearchResult{
		Tracks:    tracks,
		Albums:    itemsOrdered(albumURIs, albumByURI),
		Artists:   itemsOrdered(artistURIs, artistByURI),
		Playlists: playlists,
	}, nil
}

// searchURIs asks the context resolver for the tracks of a search query.
func searchURIs(ctx context.Context, env *Env, query string) ([]string, error) {
	uri := "spotify:search:" + url.QueryEscape(query)

	resp, err := env.Session.Spclient().Request(ctx, "GET", "/context-resolve/v1/"+uri, nil, nil, nil)
	if err != nil {
		return nil, err
	}
	defer func() { _ = resp.Body.Close() }()

	if resp.StatusCode != http.StatusOK {
		if resp.StatusCode >= 400 && resp.StatusCode < 500 {
			return nil, Unsupportedf("Spotify search is unavailable (context resolve answered %d)", resp.StatusCode)
		}
		return nil, fmt.Errorf("context resolve answered %d", resp.StatusCode)
	}

	body, err := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if err != nil {
		return nil, fmt.Errorf("reading search results: %w", err)
	}
	var spotCtx connectpb.Context
	if err := json.Unmarshal(body, &spotCtx); err != nil {
		return nil, fmt.Errorf("decoding search results: %w", err)
	}

	var uris []string
	seen := make(map[string]bool)
	for _, page := range spotCtx.GetPages() {
		for _, track := range page.GetTracks() {
			uri := track.GetUri()
			if !strings.HasPrefix(uri, "spotify:track:") || seen[uri] {
				continue
			}
			seen[uri] = true
			uris = append(uris, uri)
		}
	}
	return uris, nil
}

// relatedURIs lists the albums and artists of the given tracks, in order of
// first appearance.
func relatedURIs(uris []string, metas map[string]*trackMeta) (albums, artists []string) {
	seenAlbums := make(map[string]bool)
	seenArtists := make(map[string]bool)
	for _, uri := range uris {
		meta := metas[uri]
		if meta == nil || meta.Track == nil {
			continue
		}
		if album := albumURI(meta.Track.GetAlbum().GetGid()); album != "" && !seenAlbums[album] {
			seenAlbums[album] = true
			albums = append(albums, album)
		}
		for _, artist := range meta.Track.GetArtist() {
			if uri := artistURI(artist.GetGid()); uri != "" && !seenArtists[uri] {
				seenArtists[uri] = true
				artists = append(artists, uri)
			}
		}
	}
	return albums, artists
}

// Artist returns an artist page: the artist, its top tracks (for the account's
// country) and its albums and singles.
func (m *Manager) Artist(ctx context.Context, uri string) (*ArtistPage, error) {
	ref, err := spotifyuri.Parse(uri)
	if err != nil {
		return nil, err
	}
	if ref.Kind != spotifyuri.KindArtist {
		return nil, fmt.Errorf("%s is not an artist", ref.URI)
	}

	if page, ok := m.artistPages.get(ref.URI); ok {
		return page, nil
	}

	start := time.Now()
	var (
		artist                *metadatapb.Artist
		albumURIs, singleURIs []string
		topURIs               []string
	)
	err = m.Do(ctx, func(env *Env) error {
		data, err := fetchExtension(ctx, env, []string{ref.URI}, extmetadatapb.ExtensionKind_ARTIST_V4)
		if err != nil {
			return err
		}
		payload := data[ref.URI]
		if payload == nil {
			return Unsupportedf("artist %s was not found", ref.URI)
		}
		var meta metadatapb.Artist
		if err := payload.UnmarshalTo(&meta); err != nil {
			return fmt.Errorf("decoding artist metadata: %w", err)
		}
		artist = &meta
		albumURIs = albumGroupURIs(meta.GetAlbumGroup())
		singleURIs = albumGroupURIs(meta.GetSingleGroup())
		topURIs = topTrackURIs(&meta, m.Country())
		return nil
	})
	if err != nil {
		return nil, err
	}
	if len(albumURIs) > maxArtistReleases {
		albumURIs = albumURIs[:maxArtistReleases]
	}
	if len(singleURIs) > maxArtistReleases {
		singleURIs = singleURIs[:maxArtistReleases]
	}
	if len(topURIs) > maxArtistTopTracks {
		topURIs = topURIs[:maxArtistTopTracks]
	}

	metas, err := m.Lookup(ctx, topURIs)
	if err != nil {
		return nil, err
	}
	topTracks := m.buildInfos(topURIs, metas, defaultBitrate)

	var albumByURI, singleByURI map[string]Item
	err = m.Do(ctx, func(env *Env) error {
		albumByURI = m.albumItems(ctx, env, albumURIs)
		singleByURI = m.albumItems(ctx, env, singleURIs)
		return nil
	})
	if err != nil {
		return nil, err
	}

	page := &ArtistPage{
		Artist:    artistItem(ref.URI, artist),
		TopTracks: topTracks,
		Albums:    itemsOrdered(albumURIs, albumByURI),
		Singles:   itemsOrdered(singleURIs, singleByURI),
	}
	m.artistPages.put(ref.URI, page)
	m.log.Infof("artist %s: %d top tracks, %d albums, %d singles in %s",
		page.Artist.Name, len(page.TopTracks), len(page.Albums), len(page.Singles),
		time.Since(start).Round(time.Millisecond))
	return page, nil
}

// albumGroupURIs flattens the albums of artist metadata groups.
func albumGroupURIs(groups []*metadatapb.AlbumGroup) []string {
	var uris []string
	seen := make(map[string]bool)
	for _, group := range groups {
		for _, album := range group.GetAlbum() {
			if uri := albumURI(album.GetGid()); uri != "" && !seen[uri] {
				seen[uri] = true
				uris = append(uris, uri)
			}
		}
	}
	return uris
}

// topTrackURIs picks the top-track list of the given country, or the first one.
func topTrackURIs(artist *metadatapb.Artist, country string) []string {
	var chosen *metadatapb.TopTracks
	for _, list := range artist.GetTopTrack() {
		if chosen == nil {
			chosen = list
		}
		if country != "" && strings.EqualFold(list.GetCountry(), country) {
			chosen = list
			break
		}
	}
	if chosen == nil {
		return nil
	}
	var uris []string
	for _, track := range chosen.GetTrack() {
		if uri := trackURI(track.GetGid()); uri != "" {
			uris = append(uris, uri)
		}
	}
	return uris
}

func (m *Manager) cachedLibrary() ([]Section, bool) {
	m.libMu.Lock()
	defer m.libMu.Unlock()
	if m.libSections == nil || time.Since(m.libAt) > libraryCacheTTL {
		return nil, false
	}
	return m.libSections, true
}

func (m *Manager) storeLibrary(sections []Section) {
	m.libMu.Lock()
	defer m.libMu.Unlock()
	m.libSections = sections
	m.libAt = time.Now()
}

// describeSections is the one-line summary the bridge log uses.
func describeSections(sections []Section) string {
	parts := make([]string, 0, len(sections))
	for _, section := range sections {
		parts = append(parts, fmt.Sprintf("%s=%d", section.ID, len(section.Items)))
	}
	return strings.Join(parts, " ")
}
