package spotifyx

import (
	"context"

	"github.com/devgianlu/go-librespot/spclient"
)

// Liked returns the URIs of the tracks in Liked Songs, most recently added
// first. It reads only the collection set (no track metadata), so it stays
// cheap for clients that only need to know which tracks are liked.
func (m *Manager) Liked(ctx context.Context) ([]string, error) {
	var uris []string
	err := m.Do(ctx, func(env *Env) error {
		entries, err := fetchCollection(ctx, env, spclient.CollectionSetLikedSongs)
		if err != nil {
			return err
		}
		uris = collectionURIs(entries, "spotify:track:")
		return nil
	})
	if err != nil {
		return nil, err
	}
	if uris == nil {
		uris = []string{}
	}
	return uris, nil
}
