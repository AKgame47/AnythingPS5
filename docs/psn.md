# PSN account connector (unofficial)

Status: **Implemented. Tested against synthetic fixtures, a scripted fake server and a local test server. `psn sync` against real Sony: Untested until you run it with your own token.**

Only `pnr-launcher psn sync` talks to the network, and since slice F1 it sits behind a warning gate (below). Everything else is local, and offline `psn import-json` is never gated.
It stores a token you paste locally, and turns either a saved "purchased
games" response (`import-json`) or a live fetch (`sync`) into owned-on-PSN
rows in the PNR library.

## Three different things (never mixed up in PNR)

| Thing | What it means | What PNR does about it |
|---|---|---|
| **Access** | Your PSN account owns a license (entitlement) for a title | Recorded as an "owned on PSN" row: names, ids, platform, artwork URL, Sony's flags |
| **Download permission** | Whether you may fetch the game's files | **PNR has no download feature.** Sony's `isDownloadable` flag is stored and shown as Sony's metadata, not as a PNR capability |
| **PC compatibility** | Whether a PC build of the game can run under PNR | Only for a PNR game that is actually installed. Compatibility states come from the existing engine (Untested, Launches Only, ...) and nothing claims playability |

An owned row is never installed, never runnable, never listed by
`pnr-launcher list`, and has no compatibility record of its own. Its state
reads "Owned on PSN (no PC build linked)". A person may **link** it to a
PNR game; then the row shows that game's compatibility state. Linking is
manual and changes nothing about whether the game runs.

## Credentials: what PNR does and does not do

- PNR never asks for your PlayStation password and has no login form.
- The only secret is your **NPSSO token**, which you copy yourself from your own signed-in browser.
- It is stored in one local file, `<library dir>/psn/npsso.token`, created with mode 0600 inside a 0700 directory (Linux). Override with `PNR_PSN_TOKEN_FILE`.
- On every read PNR checks the file is a regular file (not a symlink), owned by you, with no group/other permission bits. Otherwise it refuses to use it.
- The token is accepted from stdin or a file (`psn set-token`), never from a command-line argument, so it does not land in shell history or process listings.
- The token is never printed, logged, put in an error message or written to the library database. Text that comes from Sony (error messages) is scrubbed of any long token-like string before display. Tests check all of this.
- The repo `.gitignore` excludes `**/psn/npsso.token`, `npsso*`, `*.npsso` and `*.token`.
- `psn sync` sends the token **only to Sony's official hosts** (see "Live sync"), never to any PNR or third-party server. PNR has no servers.
- An NPSSO token is a session credential for your whole PSN account. Treat it like a password. PNR cannot revoke it at Sony; if you think it leaked, change your PSN password and review your account security settings.
- Windows: the token file is written, but the owner-only permission check and hardening are **not implemented** (planned: ACLs). `psn status` says so.

## How to get your NPSSO token

Source: the MIT-licensed [psn-api docs](https://psn-api.achievements.app/authentication/authenticating-manually) and [psn-mcp](https://github.com/tmrp/psn-mcp).

1. In a browser, sign in to your account at playstation.com.
2. In the same browser, open `https://ca.account.sony.com/api/v1/ssocookie`.
3. You see JSON like `{"npsso":"<64 characters>"}`. If you see an error, try another browser.
4. Save that text to a file and run `pnr-launcher psn set-token --file thatfile`, or pipe it: `pnr-launcher psn set-token < thatfile`. Delete the file afterwards.

The token expires after about two months (per psn-api and psn-mcp); you then repeat the steps.

This is the same unofficial route used by trophy-tracker tools. It does not need Sony developer approval. Sony can change or block it at any time, and using it may be against Sony's terms: that is your decision to make.

## Commands

```
pnr-launcher psn status [--json]
pnr-launcher psn set-token [--file PATH]          (stdin by default)
pnr-launcher psn import-json <file> [--complete] [--json]
pnr-launcher psn sync [--complete] [--json] [--live-ok]   (LIVE, contacts Sony, warning gate)
pnr-launcher psn owned [--platform PS4|PS5] [--json]
pnr-launcher psn link <entitlement-id> <game-id>
pnr-launcher psn unlink <entitlement-id>
pnr-launcher psn disconnect [--forget-owned] [--yes]
```

- `import-json` reads a saved purchased-games response (the shape in `examples/psn/synthetic_purchased_games.json`). Without `--complete` it only adds and updates. With `--complete` (the file is the whole library) rows missing from it are **marked** "not returned by the last complete sync", never deleted.
- A response that is not the expected shape imports **nothing** and says why.
- `disconnect` zeroes and deletes the token file and keeps owned rows. `--forget-owned` also deletes the owned rows and needs `--yes`. Installed games are never touched.
- Exit codes: 0 ok, 1 error, 2 unknown id, 3 needs confirmation, 64 usage.

## What data exists (and what does not)

The parser follows the published `PurchasedGamesResponse` type of the MIT
psn-api library: `data.purchasedTitlesRetrieve.games[]` with `entitlementId`,
`titleId`, `productId`, `conceptId`, `name`, `platform`, `membership`,
`isActive`, `isDownloadable`, `isPreOrder` and `image.url`. PNR stores those
and nothing else. This endpoint shape has **not** been checked against live
Sony output; it may differ or change.

**Not available from this endpoint, so not provided:** DLC and edition
details, prices, purchase dates, package sizes, download URLs, license
expiry. PNR does not guess them. Other Sony endpoints might provide some of
that; that is unresearched and Untested.

## Errors and API changes

| Kind | Meaning | What to do |
|---|---|---|
| `no-token` | nothing stored | `psn set-token` |
| `token-format` | not a 64-char alphanumeric value | copy it again |
| `token-permissions` | file readable by others, or a symlink | fix permissions or `disconnect` then `set-token` |
| `token-rejected` | HTTP 401/403: expired or revoked | get a fresh NPSSO |
| `rate-limited` | HTTP 429 | wait |
| `server-error` | HTTP 5xx | retry later |
| `bad-json`, `schema-change` | response unreadable or not the expected shape (API change?) | nothing is imported; report it |
| `sony-errors` | a GraphQL `errors` array and no data | nothing is imported |
| `library-write` | the local database could not be written | check disk and permissions |

Also: `transport` (could not connect, TLS verification failed, timeout, or libcurl missing) and `blocked-host` (a request to a host outside the allow-list was refused before sending).
The HTTP mapping is implemented and tested against a fake and a local server, not against Sony.

## Direct downloads of purchased games: findings

**Result: no authorized mechanism was found that lets PNR download PS4/PS5
game files to a PC, so none is implemented, and no download feature or
placeholder exists.**

What this is based on (limits stated plainly):

- The psn-api and psn-mcp documentation I read describes library **metadata** (ids, names, flags, artwork). Neither documents any download-URL or package endpoint.
- I did not review AnyPS5's source for this slice, and absence from these docs does not prove no such endpoint exists. It means none was found.
- PS4/PS5 content is distributed under Sony licensing, bound to an account and a Sony console. Fetching or converting it for another device would mean working around that licensing and encryption. **PNR will not bypass DRM, encryption or licensing.** The master rules forbid it.
- `isDownloadable` is Sony's own flag about the entitlement. It does not mean PNR, a PC or a third-party tool may download it.
- Officially supported ways to use a purchase are Sony's. PNR makes no claim about them.

## Platform support

| Part | Linux | Windows |
|---|---|---|
| JSON reader, parser, database, command logic | Built and tested (3 builds, ASan clean) | Portable code, **not compiled for Windows in this slice** |
| Owner-only token file | Implemented and tested | Not implemented (planned: ACLs) |
| Live sync (libcurl via dlopen) | Built and tested (needs libcurl.so.4 installed) | **Not built or tested.** The loader uses `dlopen`; a Windows loader for libcurl.dll is not written |

## Warning gate for live sync (slice F1)

The owner chose to keep the live connection, with a gate. Before `psn sync`
sends anything, PNR prints this warning:

- the route is UNOFFICIAL, derived from community projects, not an approved Sony integration;
- it has NEVER been tested against real Sony servers, it may fail, and it may be against Sony's terms;
- the token is as sensitive as a password (sent only to Sony's official hosts, never printed or logged);
- use at your own risk, and `psn import-json` is the offline alternative.

Then, as implemented and unit tested with a fake client:

| Situation | Result |
|---|---|
| Terminal (stdin and stderr are a TTY), no flag | Warning shown, you must type `yes`. Anything else cancels and nothing is sent. |
| Not a terminal (script, pipe, CI), no flag | **Refuses**, exit 1, names `--live-ok`, nothing is sent. |
| `--live-ok` | Skips the prompt (the warning text is still printed in text mode). You accept the risk in advance. |
| No token stored | Fails first with the usual no-token message. |

`--live-ok` is a consent flag, not a safety feature: it does not make the route
official or tested. Only `psn sync` talks to Sony, so it is the only gated
path. The test suite never passes `--live-ok` to the real binary, so no test
contacts Sony. The gate is a speed bump for a person, not a security control
against local malware that can already read your token file.

## Live sync (slice 2)

`pnr-launcher psn sync [--complete] --live-ok` (after the gate above) does this, following the MIT psn-api sources:

1. `GET https://ca.account.sony.com/api/authz/v3/oauth/authorize?...` with your NPSSO as a `Cookie` header, redirects **not followed**. The authorization code is read from the `Location` header. No code means Sony did not accept the token (`token-rejected`).
2. `POST .../oauth/token` to exchange the code for an access token and refresh token.
3. `GET https://web.np.playstation.com/api/graphql/v1/op` with the persisted query `getPurchasedGameList`, 50 per page, until a short page. A page cap of 200 applies.
4. On a 401/403 it refreshes once with the refresh token and retries once; a second rejection stops with `token-rejected`.

Safety rules, each with a test:

- Exactly two hosts are allowed: `ca.account.sony.com` and `web.np.playstation.com`. https only, port 443 only, exact host match (no user-info tricks, no sub-domains, no trailing dot). Anything else is refused before it is sent and counted.
- TLS verification (certificate and host name) is always on. A test proves a self-signed server is refused. There is no option to turn it off.
- Redirects are never followed, so a redirect cannot carry a token to another host.
- Access and refresh tokens live in memory for the one run and are wiped after. The NPSSO is only ever sent as the cookie on step 1. Nothing logs request or response headers or bodies.
- A failed or partial fetch changes nothing in the library.
- Default is **merge**. `--complete` marks rows Sony did not return, but the request asks for active licences only, so inactive ones can be mislabelled; it is ignored if paging hit its cap.

Things to know:

- The sign-in uses the public client ID and secret of Sony's own PlayStation mobile app, as published in the MIT psn-api project. They are not secret and not PNR's; they identify the official app to Sony. This is the same unofficial route trophy tools use and may be against Sony's terms. Your call.
- libcurl is loaded at run time (`libcurl.so.4`, then `libcurl-gnutls.so.4`). If it is missing, `psn sync` says so and nothing else is affected. There is no build-time curl dependency.
- libcurl option numbers are declared by hand (no headers were available when this was written). The tests exercise them against a local server: URL, POST body, headers, status, Location capture, no redirect following, TLS failure and the size cap. One real finding: libcurl older than 7.85 ignores the newer protocol-restriction option, so PNR also sets the legacy option and refuses non-https URLs itself.
- **Not verified:** the whole exchange against real Sony. Sony can change any of it. If the response shape changes you get `schema-change` and no import.

## Credits

Response shape, sign-in flow, public client ID and persisted-query hash from [psn-api](https://github.com/achievements-app/psn-api) (MIT) and [psn-mcp](https://github.com/tmrp/psn-mcp) (MIT). The values are used as data; the code is PNR's own. GPL-2.0.
