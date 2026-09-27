# MegaProvider

A PowerShell provider that exposes MEGA cloud storage as a `mega:` drive, so the standard cmdlets
(`Get-ChildItem`, `Rename-Item`, `Move-Item`, `Remove-Item`, ...) and pipelines work on it.

```powershell
Get-ChildItem mega:\photos -Recurse -Filter *.jpg | Rename-Item -NewName { $_.Name -replace '^IMG_', 'trip_' }
```

> This README is a stub. Installation and usage will follow.

## Cautions

### Bulk operations

MegaProvider makes it easy to change hundreds of items with one pipeline. MEGA limits how fast an
account may send requests, and there is no undo for renames and moves.

- **Preview first.** Run the pipeline with `-WhatIf` and check what it would do.
- **Keep runs to a sensible size.** Split very large jobs instead of changing thousands of items at once.
  Do not use MegaProvider to stress-test or benchmark MEGA's servers.
- Requests are sent one at a time. When MEGA refuses one with EAGAIN (rate limiting), MegaProvider
  waits and retries up to 4 times (0.5 to 4 seconds apart).
- If it is still refused, **the whole pipeline stops** at that item, with an error naming it: the items
  before it were done, it and the rest were not. Wait a few minutes, then run the pipeline again for the
  rest.
- While MEGA is refusing a whole batch of requests, the MEGA SDK keeps retrying on its own, so a command
  can seem to hang for a while without any message.

### Other

- `Remove-Item` moves items to the Rubbish Bin; it never deletes permanently. `Restore-MegaItem` puts them back.
- Windows only for now.
- This is an unofficial tool, not affiliated with or endorsed by MEGA.
