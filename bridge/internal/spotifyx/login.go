package spotifyx

import (
	"context"
	"errors"
	"fmt"
	"io"

	librespot "github.com/devgianlu/go-librespot"
	devicespb "github.com/devgianlu/go-librespot/proto/spotify/connectstate/devices"
	"github.com/devgianlu/go-librespot/session"

	"foo_librespot/bridge/internal/creds"
	"foo_librespot/bridge/internal/logging"
)

// Login runs the OAuth 2.0 device authorization flow, prints the URL and code
// the user must approve to out, then persists the resulting stored credentials.
func Login(ctx context.Context, log *logging.Logger, out io.Writer) (*creds.Credentials, error) {
	deviceID, err := creds.NewDeviceID()
	if err != nil {
		return nil, err
	}

	client := librespot.NewHttpClient()
	sess, err := session.NewSessionFromOptions(ctx, &session.Options{
		Log:        log,
		DeviceType: devicespb.DeviceType_COMPUTER,
		DeviceId:   deviceID,
		Client:     client,
		Credentials: session.DeviceAuthCredentials{
			OnCode: func(code *session.DeviceAuthCode) {
				if code == nil {
					return
				}
				fmt.Fprintf(out, "\nOpen this URL in a browser: %s\n", code.VerificationUrl)
				fmt.Fprintf(out, "If prompted, enter code: %s\n", code.UserCode)
				if !code.ExpiresAt.IsZero() {
					fmt.Fprintf(out, "Code expires at: %s\n", code.ExpiresAt.Local().Format("15:04:05"))
				}
				fmt.Fprintln(out, "Waiting for approval...")
			},
		},
	})
	if err != nil {
		return nil, fmt.Errorf("login: %w", err)
	}

	c := &creds.Credentials{
		DeviceID:   deviceID,
		DeviceType: int32(devicespb.DeviceType_COMPUTER),
		Username:   sess.Username(),
		Data:       sess.StoredCredentials(),
	}
	sess.Close()

	if len(c.Data) == 0 {
		return nil, errors.New("login succeeded but the library returned no reusable credentials")
	}
	if err := c.Save(); err != nil {
		return nil, err
	}

	path, _ := creds.Path()
	fmt.Fprintf(out, "Logged in as %s; credentials saved to %s\n", c.Username, path)
	return c, nil
}
