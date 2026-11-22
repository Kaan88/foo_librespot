#include "stdafx.h"

// Component identity. v2 requires exactly one DECLARE_COMPONENT_VERSION per DLL
// and the file name check keeps the troubleshooter reliable.
DECLARE_COMPONENT_VERSION(
	"foo_librespot",
	"0.1.0",
	"Spotify playback for foobar2000 through the librespot-bridge helper process.\n"
	"Paths: spotify:track:<id>\n"
	"File > Add Spotify link... adds tracks, albums and playlists.\n"
	"Bridge settings: Preferences > Tools > foo_librespot")

VALIDATE_COMPONENT_FILENAME("foo_librespot.dll");
