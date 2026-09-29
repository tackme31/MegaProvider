# Prose for Get-Help. scripts/Build-HelpFile.ps1 merges it with the cmdlets' own metadata (syntax,
# parameter sets, pipeline input) into en-US/MegaProvider.dll-Help.xml; every parameter needs a text here.
# Each string in Description / Notes / Remarks is one paragraph.
@{
    'Connect-MegaAccount'    = @{
        Synopsis    = 'Logs in to MEGA and remembers the session for the mega: drive.'
        Description = @(
            'Logs in with an e-mail address and password, and saves the session so that the mega: drive works in this and later PowerShell sessions without logging in again.'
            'Only one account is used at a time. Connecting again with another account replaces the previous one.'
            'For an account with two-factor authentication, give the code from the authenticator app with -AuthCode. Without it, the code is asked for at the prompt.'
        )
        Parameters  = @{
            Credential = 'The e-mail address of the MEGA account and its password.'
            AuthCode   = 'The 6-digit code from the authenticator app, for an account with two-factor authentication. Ignored for an account without it. Codes change every 30 seconds.'
        }
        Outputs     = @{ 'MegaProvider.MegaAccountInfo' = 'The connected account (Email).' }
        Notes       = @(
            'The session is saved in a file only you can read: on Windows %LOCALAPPDATA%\MegaProvider\session.dat, encrypted with DPAPI for the current user; on Linux ~/.local/share/MegaProvider/session.dat, with mode 0600. The password itself is not saved.'
            'This session is separate from those of MEGA''s own apps; connecting or disconnecting here does not affect them.'
            'Logging in to a large account can take minutes while the file list is loaded. A progress bar shows the steps.'
            'In a session without a prompt (pwsh -NonInteractive, a runspace), an account with two-factor authentication needs -AuthCode.'
        )
        Examples    = @(
            @{ Code = 'Connect-MegaAccount'; Remarks = 'Asks for the e-mail address and password, and for the authentication code if the account needs one.' }
            @{ Code = 'Connect-MegaAccount -Credential (Get-Credential you@example.com) -AuthCode 123456'; Remarks = 'Logs in to an account with two-factor authentication.' }
        )
        Links       = @('Get-MegaAccount', 'Disconnect-MegaAccount', 'about_MegaProvider')
    }

    'Get-MegaAccount'        = @{
        Synopsis    = 'Gets the MEGA account the mega: drive is connected to.'
        Description = @(
            'Returns the account saved by Connect-MegaAccount. Fails with an error that says to run Connect-MegaAccount when there is none, or when the saved session is no longer valid (for example after logging out everywhere or changing the password).'
        )
        Outputs     = @{ 'MegaProvider.MegaAccountInfo' = 'The connected account (Email).' }
        Notes       = @(
            'The first command after the background process has stopped (see about_MegaProvider) logs in again with the saved session, which can take a while on a large account.'
        )
        Examples    = @(
            @{ Code = 'Get-MegaAccount'; Remarks = 'Shows the connected account.' }
            @{ Code = 'try { (Get-MegaAccount -ErrorAction Stop).Email } catch { $null }'; Remarks = 'Gets the e-mail address, or $null when not connected. The error is terminating, so check it with try/catch rather than an if on the result.' }
        )
        Links       = @('Connect-MegaAccount', 'Disconnect-MegaAccount')
    }

    'Disconnect-MegaAccount' = @{
        Synopsis    = 'Logs out of MEGA and forgets the saved session.'
        Description = @(
            'Logs out, which also invalidates the session on MEGA''s side, and deletes the saved session. The mega: drive stays, but using it needs Connect-MegaAccount again.'
            'Does nothing when not connected.'
        )
        Examples    = @(
            @{ Code = 'Disconnect-MegaAccount'; Remarks = 'Logs out.' }
        )
        Links       = @('Connect-MegaAccount')
    }

    'Send-MegaItem'          = @{
        Synopsis    = 'Uploads local files and folders to a folder on the mega: drive.'
        Description = @(
            'Uploads each file, or each folder with everything in it, into an existing folder on the mega: drive, and outputs the uploaded item.'
            'Copy-Item cannot copy between the file system and the mega: drive (PowerShell does not copy across providers), so uploads go through this command.'
        )
        Parameters  = @{
            Path        = 'Local files or folders to upload. Wildcards are expanded.'
            LiteralPath = 'Local files or folders to upload, taken exactly as written (no wildcards). Items piped from Get-ChildItem bind here.'
            Destination = 'An existing folder on the mega: drive, such as mega:\photos.'
        }
        Inputs      = @{ 'System.String' = 'Local paths.'; 'System.IO.FileSystemInfo' = 'Items from Get-ChildItem or Get-Item (bound through PSPath).' }
        Outputs     = @{ 'MegaProvider.Backend.MegaItem' = 'The uploaded item on the mega: drive.' }
        Notes       = @(
            'A file with the same name as one already in the destination does not replace it and is not refused: MEGA adds the upload as a new version of that file. The item keeps one name, and older versions stay in MEGA''s version history.'
            'Press Ctrl+C to cancel a transfer in progress; the partly uploaded file is discarded.'
            'The destination is refused when a folder on its path has a same-named sibling, since it could not be told which one is meant (see about_MegaProvider).'
        )
        Examples    = @(
            @{ Code = 'Send-MegaItem C:\photos\trip.jpg mega:\photos'; Remarks = 'Uploads one file.' }
            @{ Code = 'Get-ChildItem C:\photos -Filter *.jpg | Send-MegaItem -Destination mega:\photos'; Remarks = 'Uploads every .jpg file in a local folder.' }
            @{ Code = 'Send-MegaItem C:\projects\report mega:\backup'; Remarks = 'Uploads a folder with everything in it, as mega:\backup\report.' }
        )
        Links       = @('Receive-MegaItem', 'about_MegaProvider')
    }

    'Receive-MegaItem'       = @{
        Synopsis    = 'Downloads files and folders from the mega: drive.'
        Description = @(
            'Downloads each file, or each folder with everything in it, into an existing local folder, and outputs the local file or folder.'
        )
        Parameters  = @{
            Path        = 'Items on the mega: drive to download. Wildcards are expanded.'
            LiteralPath = 'Items on the mega: drive to download, taken exactly as written (no wildcards). Items piped from Get-ChildItem mega:\... bind here.'
            Destination = 'An existing local folder. Defaults to the current file system location (the location of the FileSystem provider, even when the current location is on mega:).'
        }
        Inputs      = @{ 'System.String' = 'Paths on the mega: drive.'; 'MegaProvider.Backend.MegaItem' = 'Items from Get-ChildItem or Get-Item on the mega: drive.' }
        Outputs     = @{ 'System.IO.FileInfo, System.IO.DirectoryInfo' = 'The downloaded file or folder.' }
        Notes       = @(
            'An existing local file is not overwritten: the download is saved under a new name with " (1)" added, and the output shows which name was used.'
            'Press Ctrl+C to cancel a transfer in progress.'
            'An item whose path has a same-named sibling on the way is refused, since it could not be told which one is meant (see about_MegaProvider).'
        )
        Examples    = @(
            @{ Code = 'Receive-MegaItem mega:\docs\report.pdf C:\Downloads'; Remarks = 'Downloads one file.' }
            @{ Code = 'Get-ChildItem mega:\photos -Recurse -Filter *.jpg | Receive-MegaItem -Destination C:\photos'; Remarks = 'Downloads every .jpg file under mega:\photos into one local folder.' }
        )
        Links       = @('Send-MegaItem', 'about_MegaProvider')
    }

    'Get-MegaRubbishItem'    = @{
        Synopsis    = 'Lists the items in MEGA''s Rubbish Bin.'
        Description = @(
            'Lists the top level of the Rubbish Bin, where Remove-Item on the mega: drive puts things, with the Handle that Restore-MegaItem takes.'
            'Items removed by other MEGA apps are listed too.'
        )
        Parameters  = @{
            Name = 'Lists only the items whose name matches. Wildcards are allowed; case is ignored.'
        }
        Outputs     = @{ 'MegaProvider.Backend.MegaItem' = 'An item in the Rubbish Bin (Handle, Name, Length, LastWriteTime, ...).' }
        Notes       = @(
            'Only the top level is listed: the contents of a removed folder come back with it.'
            'Several items can share a name in the Rubbish Bin; the Handle tells them apart.'
        )
        Examples    = @(
            @{ Code = 'Get-MegaRubbishItem'; Remarks = 'Lists the Rubbish Bin.' }
            @{ Code = 'Get-MegaRubbishItem *.jpg | Restore-MegaItem'; Remarks = 'Restores every .jpg file to the folder it was removed from.' }
        )
        Links       = @('Restore-MegaItem', 'about_MegaProvider')
    }

    'Restore-MegaItem'       = @{
        Synopsis    = 'Moves items from MEGA''s Rubbish Bin back to where they were.'
        Description = @(
            'Moves a Rubbish Bin item, given by its Handle, back to the folder it was removed from, or into -Destination. Outputs the restored item.'
            'MEGA remembers where each item was removed from, so this also works for items removed by other MEGA apps.'
        )
        Parameters  = @{
            Handle      = 'The item''s handle, as listed by Get-MegaRubbishItem. Items piped from Get-MegaRubbishItem bind here.'
            Destination = 'An existing folder on the mega: drive to restore into, instead of the original folder.'
        }
        Inputs      = @{ 'MegaProvider.Backend.MegaItem' = 'Items from Get-MegaRubbishItem.' }
        Outputs     = @{ 'MegaProvider.Backend.MegaItem' = 'The restored item on the mega: drive.' }
        Notes       = @(
            'If the original folder no longer exists, the item is restored to the root of the drive.'
            'If the folder already has an item with the same name, the restored item is put next to it under the same name; it is not refused and nothing is replaced. Such same-named items can then only be read, not changed, by path (see about_MegaProvider). Rename one of them in another MEGA app, or restore with -Destination.'
        )
        Examples    = @(
            @{ Code = 'Get-MegaRubbishItem report.pdf | Restore-MegaItem'; Remarks = 'Restores report.pdf to the folder it was removed from.' }
            @{ Code = 'Restore-MegaItem -Handle h1a2b3c4 -Destination mega:\restored'; Remarks = 'Restores one item into another folder.' }
        )
        Links       = @('Get-MegaRubbishItem', 'about_MegaProvider')
    }

    'Publish-MegaItem'       = @{
        Synopsis    = 'Creates public links to items on the mega: drive.'
        Description = @(
            'Creates a public link to each file or folder and outputs it. An item that already has a link keeps it: the same link is returned, not a new one.'
            '-ExpiresAt and -NoExpiry set or remove the link''s expiry. Without them, the expiry the link already has is kept, including one set in another MEGA app.'
            '-Password also makes a password-protected link, returned as Url; the plain link is in UnprotectedUrl.'
        )
        Parameters  = @{
            Path        = 'Items on the mega: drive. Wildcards are expanded.'
            LiteralPath = 'Items on the mega: drive, taken exactly as written (no wildcards). Items piped from Get-ChildItem mega:\... or Get-MegaLink bind here.'
            ExpiresAt   = 'When the link stops working, in local time. Must be in the future. Needs a MEGA Pro plan.'
            NoExpiry    = 'Removes the link''s expiry, so it works until removed.'
            Password    = 'Makes a password-protected link from the plain one. Needs a MEGA Pro plan. Use Read-Host -AsSecureString to type it.'
        }
        Inputs      = @{ 'System.String' = 'Paths on the mega: drive.'; 'MegaProvider.Backend.MegaItem' = 'Items from Get-ChildItem or Get-Item on the mega: drive.' }
        Outputs     = @{ 'MegaProvider.MegaLinkInfo' = 'The link: Path, Url, ExpiresAt, UnprotectedUrl, UrlWithoutKey, Key, Created, IsExpired, IsTakenDown, IsPasswordProtected.' }
        Notes       = @(
            'Expiry dates and password-protected links are MEGA Pro features. On the free plan they are refused before any link is made. To keep a link private on the free plan, send UrlWithoutKey and Key separately (by different means); the link works only with both.'
            'Changing the expiry makes MEGA re-issue the link. Share the Url this command returns rather than one saved earlier.'
            'MEGA stores nothing of a password-protected link: it is the plain link encrypted with the password. Save the Url now; Get-MegaLink cannot show it later, and nothing can tell afterwards that one was made. The plain link (UnprotectedUrl) keeps working, so do not share that one. Unpublish-MegaItem stops both.'
            'Creating a link to a folder can take ten seconds or more.'
            'An item whose path has a same-named sibling on the way is refused (see about_MegaProvider).'
        )
        Examples    = @(
            @{ Code = '(Publish-MegaItem mega:\photos\2024).Url'; Remarks = 'Creates a link to a folder (or gets the one it has) and shows the URL.' }
            @{ Code = 'Get-ChildItem mega:\share -File | Publish-MegaItem -ExpiresAt (Get-Date).AddDays(7) | Select-Object Name, Url'; Remarks = 'Creates links that stop working in a week (Pro plan).' }
            @{ Code = "`$link = Publish-MegaItem mega:\report.pdf -Password (Read-Host -AsSecureString 'Password')`n`$link.Url | Set-Clipboard"; Remarks = 'Creates a password-protected link (Pro plan) and copies it. Save it: it cannot be shown again.' }
            @{ Code = "`$link = Publish-MegaItem mega:\report.pdf`n`$link.UrlWithoutKey`n`$link.Key"; Remarks = 'Gets the link and its key separately, to send by different means (any plan).' }
        )
        Links       = @('Unpublish-MegaItem', 'Get-MegaLink', 'about_MegaProvider')
    }

    'Unpublish-MegaItem'     = @{
        Synopsis    = 'Removes the public links of items on the mega: drive.'
        Description = @(
            'Removes the public link of each item. Anyone with the link, including a password-protected one made from it, can no longer open the item.'
            'Does nothing for an item that has no link.'
        )
        Parameters  = @{
            Path        = 'Items on the mega: drive. Wildcards are expanded.'
            LiteralPath = 'Items on the mega: drive, taken exactly as written (no wildcards). Items piped from Get-MegaLink or Get-ChildItem mega:\... bind here.'
        }
        Inputs      = @{ 'System.String' = 'Paths on the mega: drive.'; 'MegaProvider.MegaLinkInfo' = 'Links from Get-MegaLink or Publish-MegaItem.' }
        Notes       = @(
            'Remove-Item does not remove an item''s link, and Get-MegaLink does not list items in the Rubbish Bin. Remove the link first if it must stop working.'
            'An item whose path has a same-named sibling on the way is refused (see about_MegaProvider).'
        )
        Examples    = @(
            @{ Code = 'Unpublish-MegaItem mega:\photos\2024'; Remarks = 'Removes the link to a folder.' }
            @{ Code = 'Get-MegaLink | Where-Object IsExpired | Unpublish-MegaItem'; Remarks = 'Removes every link that has expired.' }
        )
        Links       = @('Publish-MegaItem', 'Get-MegaLink')
    }

    'Get-MegaLink'           = @{
        Synopsis    = 'Gets the public links of items on the mega: drive.'
        Description = @(
            'Without a path, gets every public link on the drive. With a path, gets the link of that item, and with -Recurse also those of everything under it.'
            'An item without a link outputs nothing (and no error), so this also answers whether an item has a link.'
        )
        Parameters  = @{
            Path        = 'Items on the mega: drive. Wildcards are expanded. Without it, all links are listed.'
            LiteralPath = 'Items on the mega: drive, taken exactly as written (no wildcards). Items piped from Get-ChildItem mega:\... bind here.'
            Recurse     = 'Also gets the links of everything under the given folders.'
        }
        Inputs      = @{ 'System.String' = 'Paths on the mega: drive.'; 'MegaProvider.Backend.MegaItem' = 'Items from Get-ChildItem or Get-Item on the mega: drive.' }
        Outputs     = @{ 'MegaProvider.MegaLinkInfo' = 'The link: Path, Url, ExpiresAt, UnprotectedUrl, UrlWithoutKey, Key, Created, IsExpired, IsTakenDown, IsPasswordProtected.' }
        Notes       = @(
            'Url is always the plain link. A password-protected link cannot be listed: MEGA stores nothing of it.'
            'Expired links are listed too (IsExpired). So are links MEGA has taken down (IsTakenDown).'
            'Items in the Rubbish Bin are not listed, even if they still have a link.'
        )
        Examples    = @(
            @{ Code = 'Get-MegaLink'; Remarks = 'Lists every public link.' }
            @{ Code = 'Get-MegaLink mega:\photos -Recurse | Select-Object Path, Url | Export-Csv links.csv'; Remarks = 'Saves the links under mega:\photos to a CSV file.' }
            @{ Code = 'if (Get-MegaLink mega:\docs\report.pdf) { ''shared'' }'; Remarks = 'Checks whether an item has a link.' }
        )
        Links       = @('Publish-MegaItem', 'Unpublish-MegaItem')
    }
}
