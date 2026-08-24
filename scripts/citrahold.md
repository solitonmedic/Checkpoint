# Citrahold sync

`citrahold` synchronizes Checkpoint save backups and extdata backups with a
Citrahold-compatible server. It runs on 3DS and stores its settings and token
in an encrypted, console-bound Checkpoint vault.

## Install

Copy `citrahold.c` to:

```text
/3ds/Checkpoint/scripts/universal/citrahold.c
```

Start Checkpoint, press SELECT, choose **citrahold**, then choose **Official**
or **Custom** server mode. Enter the account token for the selected server when
prompted. The script reuses the selected mode and token on later runs; use
**Configuration** to change either one.

The script stores its encrypted settings, token, and Game ID mappings at:

```text
/3ds/Checkpoint/config/citrahold.vault
```

While an upload is running, its temporary request body is stored under
`/3ds/Checkpoint/config/` and removed afterwards.

## Use

Create a Game ID mapping before uploading or downloading a save or extdata
backup. The script has separate **Upload** and **Download** pages. On a
download, it asks for a Checkpoint backup name and creates that final backup
folder only after every file arrives successfully. If the transfer is cancelled
or fails, its incomplete `*.citrahold-partial` folder is removed.

Use **Manage Game IDs** to add or edit mappings. The script queries and stores
remote Game IDs so that known IDs can be identified later.

Uploads are streamed from a temporary file. Downloads enumerate server files
and receive them one file at a time with HTTP range requests, so large extdata
backups can be transferred without loading an entire backup into script memory.
Progress is shown for uploads and downloads. Hold B to cancel a transfer.

## Server compatibility

Official mode uses `https://api.citrahold.com`. Custom mode requires a
Citrahold-compatible server. The reference contract is below so custom-server
authors can support the script directly.

### API contract

Requests use HTTP `POST`. Requests carrying fields use JSON with
`Content-Type: application/json`; a token is supplied as the `token` property.

| Purpose | Route |
| --- | --- |
| Health check | `/areyouawake` |
| Version and message of the day | `/softwareVersion` |
| Exchange a shorthand token | `/getToken` |
| Verify a token | `/getUserID` |
| List remote save Game IDs | `/getSaves` |
| List remote extdata Game IDs | `/getExtdata` |
| Save/extdata update time | `/getSavesLastUpdated`, `/getExtdataLastUpdated` |
| Upload saves/extdata | `/uploadMultiSaves`, `/uploadMultiExtdata` |
| List or download save files | `/downloadSaves` |
| List or download extdata files | `/downloadExtdata` |

`/getToken` exchanges a shorthand token for the full token, which the script
then verifies with `/getUserID`. The listing routes return the Game IDs that
belong to the authenticated account.

### Multi-file uploads

An upload contains the token, a user-defined Game ID, and a `multi` array. Each
file entry contains its remote relative path and Base64-encoded content. Empty
directories are represented with a `citraholdDirectoryDummy` marker:

```json
{
  "token": "...",
  "game": "user-defined-game-id",
  "multi": [
    ["user-defined-game-id/path/file.bin", "BASE64_DATA"],
    ["user-defined-game-id/subdirectory/citraholdDirectoryDummy", "citraholdDirectoryDummy"]
  ]
}
```

A successful multi-upload replaces the existing remote directory for that Game
ID. The script therefore checks the remote state and asks for confirmation
before replacing a present backup.

### File-list and ranged downloads

The initial request to `/downloadSaves` or `/downloadExtdata` includes `token`
and `game`. A successful response lists relative paths:

```json
{
  "files": [
    "path/file.bin",
    "subdirectory/file.bin"
  ]
}
```

For each path, the script sends `token`, `game`, and `file` to the same route,
with a `Range: bytes=<start>-<end>` header. The server must return the requested
binary bytes with HTTP `206 Partial Content` and a `Content-Range` header. This
allows the 3DS script to download large backups a file and range at a time.

## Notes

Checkpoint may need to be restarted after a download before its current backup
list displays the newly created folder. This does not affect the backup files
on the SD card.
