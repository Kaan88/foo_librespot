# Security policy

## Supported versions

Only the latest release gets fixes. Update through **Preferences > Components**
in foobar2000 with the newest `foo_librespot.fb2k-component` from the
[releases page](https://github.com/Kaan88/foo_librespot/releases).

## Reporting a vulnerability

Please report security problems privately through
[GitHub's private vulnerability reporting](https://github.com/Kaan88/foo_librespot/security/advisories/new),
not in a public issue.

Useful to include: the version, what an attacker can do, and steps to
reproduce. Never include your Spotify credentials or the contents of
`%APPDATA%\foo_librespot\credentials.json`.

## Scope

- The component (`foo_librespot.dll`) and the bridge helper
  (`librespot-bridge.exe`), including the named pipe between them.
- Handling of the saved Spotify login.

Bugs in Spotify itself, foobar2000 or other components are out of scope;
report those to their owners.
