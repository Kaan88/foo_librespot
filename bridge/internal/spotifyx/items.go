package spotifyx

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"strings"
	"sync"

	librespot "github.com/devgianlu/go-librespot"
	extmetadatapb "github.com/devgianlu/go-librespot/proto/spotify/extendedmetadata"
	metadatapb "github.com/devgianlu/go-librespot/proto/spotify/metadata"
	playlist4pb "github.com/devgianlu/go-librespot/proto/spotify/playlist4"
	"google.golang.org/protobuf/types/known/anypb"
)

// Item kinds of the protocol's <item>.
const (
	ItemKindPlaylist   = "playlist"
	ItemKindAlbum      = "album"
	ItemKindArtist     = "artist"
	ItemKindCollection = "collection"
)

// Item is one entry of a library section, a search result or an artist page.
type Item struct {
	URI        string  `json:"uri"`
	Kind       string  `json:"kind"`
	Name       string  `json:"name"`
	Subtitle   string  `json:"subtitle"`
	ImageURL   *string `json:"image_url"`
	TrackCount *int    `json:"track_count"`
	Revision   *string `json:"revision"`
	// Editable is set on playlist items only: whether the user can add tracks.
	Editable *bool `json:"editable,omitempty"`
}

// ContextInfo is the optional "context" object of a resolve reply.
type ContextInfo struct {
	URI        string `json:"uri"`
	Name       string `json:"name"`
	Owner      string `json:"owner"`
	ImageURL   string `json:"image_url"`
	Revision   string `json:"revision"`
	TrackCount int    `json:"track_count"`
}

// Section is one group of the library reply.
type Section struct {
	ID    string `json:"id"`
	Title string `json:"title"`
	Items []Item `json:"items"`
}

// revisionHash is a stable, opaque revision for a track list whose contents
// Spotify gives no revision for (albums, Liked Songs).
func revisionHash(uris []string) string {
	h := sha256.New()
	for _, uri := range uris {
		_, _ = h.Write([]byte(uri))
		_, _ = h.Write([]byte{'\n'})
	}
	return hex.EncodeToString(h.Sum(nil)[:16])
}

// contextRevision returns the playlist service's revision when it is usable and
// falls back to a hash of the track list otherwise. Spotify-generated
// playlists report an all-zero revision, which never changes and would make a
// client skip every sync; those get the content hash instead.
func contextRevision(revision string, uris []string) string {
	if revision != "" && strings.Trim(revision, "0") != "" {
		return revision
	}
	return revisionHash(uris)
}

// itemRevision is the revision an item carries: nil when the playlist service
// has none, because a wrong revision would make a client skip a real change.
func itemRevision(revision []byte) *string {
	if len(revision) == 0 {
		return nil
	}
	hexRev := hex.EncodeToString(revision)
	if strings.Trim(hexRev, "0") == "" {
		return nil
	}
	return &hexRev
}

func stringPtr(s string) *string { return &s }
func intPtr(v int) *int          { return &v }

// bestImageURL is the largest image of a set, served from i.scdn.co.
func bestImageURL(images []*metadatapb.Image) string {
	var best *metadatapb.Image
	var bestSize int64
	for _, img := range images {
		if len(img.GetFileId()) == 0 {
			continue
		}
		size := int64(img.GetWidth()) * int64(img.GetHeight())
		if size == 0 {
			// Older metadata carries only the named size (small..xlarge).
			size = int64(img.GetSize()) + 1
		}
		if best == nil || size > bestSize {
			best, bestSize = img, size
		}
	}
	if best == nil {
		return ""
	}
	return "https://i.scdn.co/image/" + hex.EncodeToString(best.GetFileId())
}

// albumItem describes one album. A nil album keeps the item listable by URI.
func albumItem(uri string, album *metadatapb.Album) Item {
	item := Item{URI: uri, Kind: ItemKindAlbum, Subtitle: ""}
	if album == nil {
		return item
	}
	item.Name = album.GetName()

	names := make([]string, 0, len(album.GetArtist()))
	for _, artist := range album.GetArtist() {
		if name := artist.GetName(); name != "" {
			names = append(names, name)
		}
	}
	item.Subtitle = strings.Join(names, ", ")

	images := album.GetCoverGroup().GetImage()
	if len(images) == 0 {
		images = album.GetCover()
	}
	if url := bestImageURL(images); url != "" {
		item.ImageURL = stringPtr(url)
	}
	return item
}

// artistItem describes one artist.
func artistItem(uri string, artist *metadatapb.Artist) Item {
	item := Item{URI: uri, Kind: ItemKindArtist}
	if artist == nil {
		return item
	}
	item.Name = artist.GetName()

	images := artist.GetPortraitGroup().GetImage()
	if len(images) == 0 {
		images = artist.GetPortrait()
	}
	if url := bestImageURL(images); url != "" {
		item.ImageURL = stringPtr(url)
	}
	return item
}

// playlistItem describes a playlist read from the user's rootlist.
func playlistItem(uri, name, owner string, image string, count int, revision *string) Item {
	item := Item{
		URI:        uri,
		Kind:       ItemKindPlaylist,
		Name:       name,
		Subtitle:   owner,
		TrackCount: intPtr(count),
		Revision:   revision,
	}
	if image != "" {
		item.ImageURL = stringPtr(image)
	}
	return item
}

// rootlistImage is the image a decorated rootlist entry names: its own picture,
// or the image_url format attribute that generated and editorial playlists use.
func rootlistImage(attrs *playlist4pb.ListAttributes) string {
	for _, attr := range attrs.GetFormatAttributes() {
		if key := attr.GetKey(); key == "image_url" || key == "header_image_url_desktop" {
			if value := attr.GetValue(); strings.HasPrefix(value, "https://") {
				return value
			}
		}
	}
	if picture := attrs.GetPicture(); len(picture) > 0 {
		return "https://u.scdn.co/images/pl/default/" + hex.EncodeToString(picture)
	}
	return ""
}

// splitRootlist turns a decorated rootlist into the "made for you" and "your
// playlists" lists. Folders (and anything that is not a playlist) are skipped;
// Spotify-owned playlists count as made for you.
func splitRootlist(rl *playlist4pb.SelectedListContent, username string) (madeFor, playlists []Item) {
	items := rl.GetContents().GetItems()
	metas := rl.GetContents().GetMetaItems()

	madeFor = []Item{}
	playlists = []Item{}
	for i, entry := range items {
		uri := entry.GetUri()
		if !strings.HasPrefix(uri, "spotify:playlist:") || i >= len(metas) {
			continue
		}
		meta := metas[i]
		attrs := meta.GetAttributes()
		owner := meta.GetOwnerUsername()

		item := playlistItem(uri, attrs.GetName(), owner, rootlistImage(attrs),
			int(meta.GetLength()), itemRevision(meta.GetRevision()))
		editable := playlistEditable(meta, username)
		item.Editable = &editable
		if strings.EqualFold(owner, "spotify") {
			madeFor = append(madeFor, item)
		} else {
			playlists = append(playlists, item)
		}
	}
	return madeFor, playlists
}

// playlistEditable tells whether the user can add tracks to a rootlist
// playlist: they own it, it is collaborative, or the playlist service's
// capabilities say they may edit its items.
func playlistEditable(meta *playlist4pb.MetaItem, username string) bool {
	if owner := meta.GetOwnerUsername(); owner != "" && username != "" && strings.EqualFold(owner, username) {
		return true
	}
	if meta.GetAttributes().GetCollaborative() {
		return true
	}
	return meta.GetCapabilities().GetCanEditItems()
}

// likedItem is the single Liked Songs entry of the library.
func likedItem(username string, uris []string) Item {
	return Item{
		URI:        "spotify:user:" + username + ":collection",
		Kind:       ItemKindCollection,
		Name:       "Liked Songs",
		Subtitle:   username,
		TrackCount: intPtr(len(uris)),
		Revision:   stringPtr(revisionHash(uris)),
	}
}

// fetchExtension reads one extended-metadata extension for a batch of entities.
// It mirrors fetchTracks: unique URIs, batches of metaBatchSize, a few requests
// in flight.
func fetchExtension(ctx context.Context, env *Env, uris []string, kind extmetadatapb.ExtensionKind) (map[string]*anypb.Any, error) {
	unique := make([]string, 0, len(uris))
	seen := make(map[string]bool, len(uris))
	for _, uri := range uris {
		if uri == "" || seen[uri] {
			continue
		}
		seen[uri] = true
		unique = append(unique, uri)
	}

	out := make(map[string]*anypb.Any, len(unique))
	if len(unique) == 0 {
		return out, nil
	}

	var (
		mu       sync.Mutex
		firstErr error
		wg       sync.WaitGroup
		sem      = make(chan struct{}, metaConcurrency)
	)
	for start := 0; start < len(unique); start += metaBatchSize {
		end := start + metaBatchSize
		if end > len(unique) {
			end = len(unique)
		}
		batch := unique[start:end]

		if ctx.Err() != nil {
			break
		}
		wg.Add(1)
		sem <- struct{}{}
		go func(batch []string) {
			defer wg.Done()
			defer func() { <-sem }()

			req := &extmetadatapb.BatchedEntityRequest{}
			for _, uri := range batch {
				req.EntityRequest = append(req.EntityRequest, &extmetadatapb.EntityRequest{
					EntityUri: uri,
					Query:     []*extmetadatapb.ExtensionQuery{{ExtensionKind: kind}},
				})
			}
			resp, err := env.Session.Spclient().ExtendedMetadata(ctx, req)

			mu.Lock()
			defer mu.Unlock()
			if err != nil {
				if firstErr == nil {
					firstErr = fmt.Errorf("fetching metadata: %w", err)
				}
				return
			}
			for _, arr := range resp.GetExtendedMetadata() {
				if arr.GetExtensionKind() != kind {
					continue
				}
				for _, data := range arr.GetExtensionData() {
					if data.GetHeader().GetStatusCode() != 200 || data.GetExtensionData() == nil {
						continue
					}
					out[data.GetEntityUri()] = data.GetExtensionData()
				}
			}
		}(batch)
	}
	wg.Wait()

	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return out, firstErr
}

// albumItems maps album URIs to items, using the item cache and fetching only
// the albums it does not know yet. A metadata failure leaves the affected
// albums listed by URI instead of failing the request.
func (m *Manager) albumItems(ctx context.Context, env *Env, uris []string) map[string]Item {
	out := make(map[string]Item, len(uris))
	missing := make([]string, 0, len(uris))
	seen := make(map[string]bool, len(uris))
	for _, uri := range uris {
		if seen[uri] {
			continue
		}
		seen[uri] = true
		if item, ok := m.items.get(uri); ok {
			out[uri] = item
			continue
		}
		missing = append(missing, uri)
	}

	if len(missing) > 0 {
		fetched, err := fetchExtension(ctx, env, missing, extmetadatapb.ExtensionKind_ALBUM_V4)
		if err != nil {
			m.log.WithError(err).Warnf("fetching %d album(s) of metadata", len(missing))
		}
		for _, uri := range missing {
			var album metadatapb.Album
			var ptr *metadatapb.Album
			if data := fetched[uri]; data != nil && data.UnmarshalTo(&album) == nil {
				ptr = &album
			}
			item := albumItem(uri, ptr)
			out[uri] = item
			if ptr != nil {
				m.items.put(uri, item)
			}
		}
	}
	return out
}

// artistItems maps artist URIs to items like albumItems does for albums.
func (m *Manager) artistItems(ctx context.Context, env *Env, uris []string) map[string]Item {
	out := make(map[string]Item, len(uris))
	missing := make([]string, 0, len(uris))
	seen := make(map[string]bool, len(uris))
	for _, uri := range uris {
		if seen[uri] {
			continue
		}
		seen[uri] = true
		if item, ok := m.items.get(uri); ok {
			out[uri] = item
			continue
		}
		missing = append(missing, uri)
	}

	if len(missing) > 0 {
		fetched, err := fetchExtension(ctx, env, missing, extmetadatapb.ExtensionKind_ARTIST_V4)
		if err != nil {
			m.log.WithError(err).Warnf("fetching %d artist(s) of metadata", len(missing))
		}
		// ARTIST_V4 is a whole page of artist data, so unmarshal it here rather
		// than in the generic fetch.
		byURI := make(map[string]*metadatapb.Artist, len(fetched))
		for uri, data := range fetched {
			var artist metadatapb.Artist
			if err := data.UnmarshalTo(&artist); err == nil {
				byURI[uri] = &artist
			}
		}
		for _, uri := range missing {
			item := artistItem(uri, byURI[uri])
			out[uri] = item
			if byURI[uri] != nil {
				m.items.put(uri, item)
			}
		}
	}
	return out
}

// trackURI turns a metadata GID of a track into its URI.
func trackURI(gid []byte) string {
	if len(gid) == 0 {
		return ""
	}
	return "spotify:track:" + librespot.GidToBase62(gid)
}

// albumURI turns a metadata GID of an album into its URI.
func albumURI(gid []byte) string {
	if len(gid) == 0 {
		return ""
	}
	return "spotify:album:" + librespot.GidToBase62(gid)
}

// artistURI turns a metadata GID of an artist into its URI.
func artistURI(gid []byte) string {
	if len(gid) == 0 {
		return ""
	}
	return "spotify:artist:" + librespot.GidToBase62(gid)
}
