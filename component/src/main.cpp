#include "stdafx.h"
#include "version.h"

// Component identity. v2 requires exactly one DECLARE_COMPONENT_VERSION per DLL
// and the file name check keeps the troubleshooter reliable.
DECLARE_COMPONENT_VERSION(
	"foo_librespot",
	FLP_VERSION,
	"Spotify playback for foobar2000 through the librespot-bridge helper process.\n"
	"Paths: spotify:track:<id>\n"
	"Spotify tab: Add New UI Element... > Spotify\n"
	"Synced playlists: Library > Sync Spotify playlists\n"
	"File > Add Spotify link... adds tracks, albums and playlists.\n"
	"Bridge settings: Preferences > Tools > foo_librespot\n"
	"\n"
	"Requires Spotify Premium. Not affiliated with or endorsed by Spotify.\n"
	"Audio through go-librespot (https://github.com/devgianlu/go-librespot), GPL-3.0.\n"
	"\n"
	"Licensed under the GNU General Public License v3.0.\n"
	"https://github.com/Kaan88/foo_librespot")

VALIDATE_COMPONENT_FILENAME("foo_librespot.dll");
