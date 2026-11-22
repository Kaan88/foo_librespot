package spotifyx

import (
	"errors"
	"testing"

	librespot "github.com/devgianlu/go-librespot"
	"github.com/devgianlu/go-librespot/spclient"
)

func TestIsPermanent(t *testing.T) {
	cases := []struct {
		name string
		err  error
		want bool
	}{
		{"nil", nil, false},
		{"restricted", librespot.ErrMediaRestricted, true},
		{"no formats", librespot.ErrNoSupportedFormats, true},
		{"wrapped restricted", errors.Join(errors.New("relink"), librespot.ErrMediaRestricted), true},
		{"not logged in", ErrNotLoggedIn, true},
		{"access denied", &spclient.ContextResolveError{StatusCode: 403}, true},
		{"not found", &spclient.ContextResolveError{StatusCode: 404}, true},
		{"server error", &spclient.ContextResolveError{StatusCode: 500}, false},
		{"generic", errors.New("connection reset"), false},
	}

	for _, tc := range cases {
		if got := IsPermanent(tc.err); got != tc.want {
			t.Errorf("%s: IsPermanent = %v, want %v", tc.name, got, tc.want)
		}
	}
}
