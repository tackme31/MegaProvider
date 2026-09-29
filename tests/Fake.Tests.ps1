# Provider behaviour on the in-memory FakeBackend: no account, no network. Run via scripts/test.ps1,
# which starts a fresh pwsh with MEGAPROVIDER_BACKEND=fake (the backend is fixed per process).
#
# The fake tree lives for the whole process, so the mutating tests below leave it as they found it
# or work on items nothing else looks at.

BeforeAll {
    if ($env:MEGAPROVIDER_BACKEND -ne 'fake') { throw 'Run with MEGAPROVIDER_BACKEND=fake (scripts/test.ps1 does).' }
    Import-Module $env:MEGAPROVIDER_PSD1 -Force

    # Expected paths are written with '\'; outside Windows PowerShell hands them back with '/'.
    function P([string]$path) { $path.Replace('\', [IO.Path]::DirectorySeparatorChar) }
}

Describe 'Navigation' {
    It 'lists the root, keeping same-named siblings' {
        (Get-ChildItem mega:\).Name | Should -Be @('docs', 'photos', 'empty', 'dup.txt', 'dup.txt')
    }

    It 'filters with -File and -Directory' {
        (Get-ChildItem mega:\ -File).Name | Should -Be @('dup.txt', 'dup.txt')
        (Get-ChildItem mega:\ -Directory).Name | Should -Be @('docs', 'photos', 'empty')
    }

    It 'recurses with -Filter and -Name' {
        Get-ChildItem mega:\photos -Recurse -Filter *.jpg -Name | Should -HaveCount 5
        Get-ChildItem mega:\photos -Recurse -File -Name | Should -Contain (P '2024\IMG_0001.jpg')
    }

    It 'recurses from the drive root after cd (regression: GetChildName on "")' {
        Push-Location mega:\
        try { Get-ChildItem -Recurse | Should -HaveCount 13 } finally { Pop-Location }
    }

    It 'resolves names case-insensitively when there is no exact match' {
        Push-Location mega:\DOCS
        try { (Get-ChildItem).Name | Should -Contain 'readme.txt' } finally { Pop-Location }
    }

    It 'answers Test-Path, Get-Item and the path cmdlets' {
        Test-Path mega:\docs\readme.txt | Should -BeTrue
        Test-Path mega:\docs\nope.txt | Should -BeFalse
        (Get-Item mega:\docs).IsFolder | Should -BeTrue
        (Resolve-Path mega:\docs\*.txt).Path | Should -Be @((P 'mega:\docs\readme.txt'), (P 'mega:\docs\notes.txt'))
        Split-Path mega:\docs\readme.txt -Leaf | Should -Be 'readme.txt'
        Join-Path mega:\docs readme.txt | Should -Be (P 'mega:\docs\readme.txt')
    }

    It 'completes paths with Tab' {
        (TabExpansion2 'Get-ChildItem mega:\docs\re' 27).CompletionMatches.CompletionText | Should -Contain (P 'mega:\docs\readme.txt')
    }

    # Known limitation, PowerShell's side: completion for non-file-system providers
    # (CompletionCompleters.GetDefaultProviderResults) puts every child of the folder into a
    # dictionary keyed by name, so a folder holding same-named siblings throws and completes nothing.
    It 'completes paths with Tab in a folder with same-named siblings' -Skip {
        (TabExpansion2 'Get-ChildItem mega:\ph' 22).CompletionMatches.CompletionText | Should -Contain (P 'mega:\photos')
    }

    It 'answers to the file system''s property names' {
        # Without a real Length, $_.Length reads PowerShell's intrinsic 1 and filters pass silently.
        (Get-ChildItem mega:\docs -File | Where-Object Length -GT 1000).Name | Should -Be @('readme.txt')
        (Get-ChildItem mega:\docs -File | Measure-Object Length -Sum).Sum | Should -Be 1540
        (Get-ChildItem mega:\docs\readme.txt | Select-Object Length).Length | Should -Be 1200
        (Get-Item mega:\docs).Length | Should -BeNullOrEmpty
        (Get-Item mega:\docs).Mode | Should -Be 'd'
        (Get-Item mega:\docs\readme.txt).Mode | Should -Be '-'
        $item = Get-Item mega:\docs\readme.txt
        $item.LastWriteTime | Should -Be $item.Modified
    }

    It 'formats listings like the file system, grouped by folder' {
        $text = Get-ChildItem mega:\docs | Out-String -Width 120
        # PowerShell's own header format is localized: "Folder: x" in English, "Folder:x" in Japanese.
        $text | Should -Match ('Folder: ?' + [regex]::Escape((P 'mega:\docs')))
        $text | Should -Match 'Mode\s+LastWriteTime\s+Length\s+Name'
    }
}

Describe 'Changes' {
    It 'renames through the pipeline' {
        Get-ChildItem mega:\photos\2024 -Filter IMG_*.jpg |
            Rename-Item -NewName { $_.Name -replace '^IMG_', 'trip_' }
        (Get-ChildItem mega:\photos\2024).Name | Should -Be @('trip_0001.jpg', 'trip_0002.jpg', 'trip_0003.jpg', 'trip_0004.jpg', 'trip_0005.jpg')
        Get-ChildItem mega:\photos\2024 | Rename-Item -NewName { $_.Name -replace '^trip_', 'IMG_' }
    }

    It 'refuses to rename onto a name that already exists' {
        { Rename-Item mega:\docs\notes.txt -NewName readme.txt -ErrorAction Stop } | Should -Throw '*already exists*'
        Test-Path mega:\docs\notes.txt | Should -BeTrue
    }

    It 'does nothing under -WhatIf' {
        Remove-Item mega:\empty -WhatIf
        Rename-Item mega:\empty -NewName other -WhatIf
        Test-Path mega:\empty | Should -BeTrue
    }

    It 'creates a folder and moves an item into it' {
        New-Item mega:\docs\archive -ItemType Directory | Should -Not -BeNullOrEmpty
        Move-Item mega:\docs\notes.txt mega:\docs\archive
        Test-Path mega:\docs\archive\notes.txt | Should -BeTrue
        Move-Item mega:\docs\archive\notes.txt mega:\docs
    }

    It 'removes to the Rubbish Bin and restores to where it was' {
        $handle = (Get-Item mega:\docs\notes.txt).Handle
        Remove-Item mega:\docs\notes.txt
        Test-Path mega:\docs\notes.txt | Should -BeFalse
        $binned = Get-MegaRubbishItem | Where-Object Handle -EQ $handle
        $binned.Name | Should -Be 'notes.txt'
        $binned.Length | Should -Be 340   # the types file applies despite the inserted type name
        $binned | Restore-MegaItem | ForEach-Object PSPath | Should -BeLike (P '*::docs\notes.txt')
        Test-Path mega:\docs\notes.txt | Should -BeTrue
    }

    It 'restores to -Destination' {
        $handle = (Get-Item mega:\docs\archive).Handle
        Remove-Item mega:\docs\archive
        Restore-MegaItem -Handle $handle -Destination mega:\photos | Out-Null
        Test-Path mega:\photos\archive | Should -BeTrue
    }
}

Describe 'Copying' {
    BeforeAll { New-Item mega:\copies -ItemType Directory | Out-Null }
    AfterAll { Remove-Item mega:\copies -Recurse }

    It 'copies a file into a folder, keeping the original' {
        (Copy-Item mega:\docs\readme.txt mega:\copies -PassThru).PSPath | Should -BeLike (P '*::copies\readme.txt')
        (Get-Item mega:\copies\readme.txt).Handle | Should -Not -Be (Get-Item mega:\docs\readme.txt).Handle
        (Get-Item mega:\copies\readme.txt).Length | Should -Be 1200
    }

    It 'copies under a new name' {
        Copy-Item mega:\docs\notes.txt mega:\copies\notes-copy.txt
        (Get-Item mega:\copies\notes-copy.txt).Length | Should -Be 340
    }

    It 'copies through the pipeline' {
        New-Item mega:\copies\piped -ItemType Directory | Out-Null
        Get-ChildItem mega:\docs -File | Copy-Item -Destination mega:\copies\piped
        (Get-ChildItem mega:\copies\piped).Name | Should -Be @('readme.txt', 'notes.txt')
    }

    It 'refuses a name that already exists in the destination' {
        { Copy-Item mega:\docs\readme.txt mega:\copies -ErrorAction Stop } | Should -Throw '*already exists*'
        { Copy-Item mega:\docs\readme.txt mega:\docs -ErrorAction Stop } | Should -Throw '*already exists*'
    }

    It 'needs -Recurse for a folder, and then copies everything in it' {
        { Copy-Item mega:\photos\2024 mega:\copies -ErrorAction Stop } | Should -Throw '*-Recurse*'
        Test-Path mega:\copies\2024 | Should -BeFalse
        Copy-Item mega:\photos\2024 mega:\copies -Recurse
        (Get-ChildItem mega:\copies\2024).Count | Should -Be 5
    }

    It 'does nothing under -WhatIf' {
        Copy-Item mega:\docs\readme.txt mega:\copies\whatif.txt -WhatIf
        Test-Path mega:\copies\whatif.txt | Should -BeFalse
    }

    It 'refuses an ambiguous source' {
        { Copy-Item mega:\dup.txt mega:\copies -ErrorAction Stop } | Should -Throw '*ambiguous*'
    }
}

Describe 'Same-named siblings' {
    It 'lists and reads them, but refuses to change them by path' {
        (Get-Item mega:\dup.txt).Name | Should -Be 'dup.txt'
        { Rename-Item mega:\dup.txt -NewName one.txt -ErrorAction Stop } | Should -Throw '*ambiguous*'
        { Remove-Item mega:\dup.txt -ErrorAction Stop } | Should -Throw '*ambiguous*'
        { Move-Item mega:\dup.txt mega:\empty -ErrorAction Stop } | Should -Throw '*ambiguous*'
        (Get-ChildItem mega:\ -File).Name | Should -Be @('dup.txt', 'dup.txt')
    }

    It 'refuses each item piped from ls, and still handles the others' {
        Get-ChildItem mega:\ -File | Rename-Item -NewName { "x_$($_.Name)" } -ErrorVariable errs -ErrorAction SilentlyContinue
        $errs.Count | Should -Be 2
        (Get-ChildItem mega:\ -File).Name | Should -Be @('dup.txt', 'dup.txt')
    }

    It 'refuses a path whose ancestor is ambiguous' {
        # The fake lets a second same-named folder be created; the real server would refuse it.
        New-Item mega:\twins -ItemType Directory | Out-Null
        New-Item mega:\twins -ItemType Directory | Out-Null
        { New-Item mega:\twins\inner -ItemType Directory -ErrorAction Stop } | Should -Throw '*ambiguous*'
    }

    It 'refuses to move onto a name that already exists in the destination' {
        New-Item mega:\movetest -ItemType Directory | Out-Null
        New-Item mega:\movetest\docs -ItemType Directory | Out-Null
        { Move-Item mega:\docs mega:\movetest -ErrorAction Stop } | Should -Throw '*already exists*'
        Test-Path mega:\docs\readme.txt | Should -BeTrue
        Remove-Item mega:\movetest -Recurse
    }
}

Describe 'Public links' {
    AfterEach {
        Remove-Item env:MEGAPROVIDER_FAKE_PRO -ErrorAction SilentlyContinue
        Get-MegaLink | Unpublish-MegaItem
    }

    It 'creates a link, returns the same one again, and removes it' {
        $link = Publish-MegaItem mega:\docs\readme.txt
        $link.Path | Should -Be (P 'mega:\docs\readme.txt')
        $link.Url | Should -BeLike 'https://mega.nz/file/*#*'
        $link.UrlWithoutKey + '#' + $link.Key | Should -Be $link.Url
        $link.ExpiresAt | Should -BeNullOrEmpty
        $link.IsPasswordProtected | Should -BeFalse
        (Publish-MegaItem mega:\docs\readme.txt).Url | Should -Be $link.Url
        (Get-MegaLink mega:\docs\readme.txt).Url | Should -Be $link.Url
        Unpublish-MegaItem mega:\docs\readme.txt
        Get-MegaLink mega:\docs\readme.txt | Should -BeNullOrEmpty
        Unpublish-MegaItem mega:\docs\readme.txt   # no link: nothing to do, no error
    }

    It 'works through the pipeline both ways' {
        Get-ChildItem mega:\docs -File | Publish-MegaItem | Should -HaveCount 2
        (Get-MegaLink).Path | Should -Be @((P 'mega:\docs\readme.txt'), (P 'mega:\docs\notes.txt'))
        Get-MegaLink | Unpublish-MegaItem
        Get-MegaLink | Should -BeNullOrEmpty
    }

    It 'lists the links under a folder with -Recurse' {
        Publish-MegaItem mega:\photos, mega:\photos\2024\IMG_0001.jpg, mega:\docs\notes.txt | Out-Null
        Get-MegaLink mega:\photos | Should -HaveCount 1
        (Get-MegaLink mega:\photos -Recurse).Name | Should -Be @('photos', 'IMG_0001.jpg')
        (Get-MegaLink mega:\PHOTOS\2024 -Recurse).Name | Should -Be @('IMG_0001.jpg')
        Get-MegaLink mega:\ -Recurse | Should -HaveCount 3
    }

    It 'does nothing under -WhatIf' {
        Publish-MegaItem mega:\docs\readme.txt -WhatIf
        Get-MegaLink | Should -BeNullOrEmpty
        Publish-MegaItem mega:\docs\readme.txt | Out-Null
        Unpublish-MegaItem mega:\docs\readme.txt -WhatIf
        Get-MegaLink | Should -HaveCount 1
    }

    It 'refuses an ambiguous path' {
        { Publish-MegaItem mega:\dup.txt -ErrorAction Stop } | Should -Throw '*ambiguous*'
    }

    It 'refuses an expiry or a password on the free plan, before making a link' {
        { Publish-MegaItem mega:\docs\readme.txt -ExpiresAt (Get-Date).AddDays(1) -ErrorAction Stop } | Should -Throw '*Pro plan*'
        $password = ConvertTo-SecureString 'secret' -AsPlainText -Force
        { Publish-MegaItem mega:\docs\readme.txt -Password $password -ErrorAction Stop } | Should -Throw '*Pro plan*'
        Get-MegaLink | Should -BeNullOrEmpty
    }

    It 'sets, keeps and clears the expiry on a paid plan' {
        $env:MEGAPROVIDER_FAKE_PRO = '1'
        $at = (Get-Date).AddDays(7)
        (Publish-MegaItem mega:\docs\readme.txt -ExpiresAt $at).ExpiresAt | Should -Be $at
        (Publish-MegaItem mega:\docs\readme.txt).ExpiresAt | Should -Be $at
        (Get-MegaLink mega:\docs\readme.txt | Out-String) | Should -Match ([regex]::Escape($at.ToString('d')))
        (Publish-MegaItem mega:\docs\readme.txt -NoExpiry).ExpiresAt | Should -BeNullOrEmpty
    }

    It 'checks the expiry options' {
        $env:MEGAPROVIDER_FAKE_PRO = '1'
        { Publish-MegaItem mega:\docs\readme.txt -ExpiresAt (Get-Date).AddDays(-1) -ErrorAction Stop } | Should -Throw '*not in the future*'
        { Publish-MegaItem mega:\docs\readme.txt -ExpiresAt (Get-Date).AddDays(1) -NoExpiry -ErrorAction Stop } | Should -Throw '*either*'
    }

    It 'makes a password-protected link on a paid plan, keeping the plain one' {
        $env:MEGAPROVIDER_FAKE_PRO = '1'
        $password = ConvertTo-SecureString 'secret' -AsPlainText -Force
        $link = Publish-MegaItem mega:\docs\readme.txt -Password $password
        $link.Url | Should -BeLike 'https://mega.nz/#P!*'
        $link.IsPasswordProtected | Should -BeTrue
        $link.UnprotectedUrl | Should -BeLike 'https://mega.nz/file/*'
        # Nothing of the password is stored, so a later read has only the plain link.
        (Get-MegaLink mega:\docs\readme.txt).Url | Should -Be $link.UnprotectedUrl
    }

    It 'drops the link of an item in the Rubbish Bin from the listing' {
        New-Item mega:\binned -ItemType Directory | Out-Null
        Publish-MegaItem mega:\binned | Out-Null
        Remove-Item mega:\binned
        Get-MegaLink | Should -BeNullOrEmpty
    }
}

# MEGAPROVIDER_FAKE_EAGAIN makes the fake refuse changes with EAGAIN, through the same retry as the host
# (RateLimit.Retry: 4 retries). Nothing here reaches MEGA; provoking the real limit is not done on purpose.
Describe 'Rate limiting (EAGAIN)' {
    BeforeAll {
        New-Item mega:\ratelimit -ItemType Directory | Out-Null
        1..4 | ForEach-Object { Copy-Item mega:\docs\readme.txt "mega:\ratelimit\r$_.txt" }
    }
    AfterEach { Remove-Item env:MEGAPROVIDER_FAKE_EAGAIN -ErrorAction SilentlyContinue }
    AfterAll { Remove-Item mega:\ratelimit -Recurse }

    It 'retries a refused change until it goes through' {
        $env:MEGAPROVIDER_FAKE_EAGAIN = '4'
        Rename-Item mega:\ratelimit\r1.txt -NewName s1.txt
        Test-Path mega:\ratelimit\s1.txt | Should -BeTrue
        Rename-Item mega:\ratelimit\s1.txt -NewName r1.txt
    }

    It 'gives up after the retries and says where it stopped' {
        $env:MEGAPROVIDER_FAKE_EAGAIN = '5'
        $err = { Rename-Item mega:\ratelimit\r1.txt -NewName s1.txt } | Should -Throw -PassThru
        $err.Exception.GetType().Name | Should -Be 'MegaRateLimitedException'
        $err.Exception.Message | Should -BeLike '*Stopped at*r1.txt*'
        Test-Path mega:\ratelimit\r1.txt | Should -BeTrue
    }

    It 'stops the whole pipeline, leaving the items before it done' {
        $env:MEGAPROVIDER_FAKE_EAGAIN = '5@2'
        # -ErrorAction Continue would carry on after an ordinary per-item error; this one still stops.
        { Get-ChildItem mega:\ratelimit | Rename-Item -NewName { 's' + $_.Name.Substring(1) } -ErrorAction Continue } |
            Should -Throw '*EAGAIN*'
        (Get-ChildItem mega:\ratelimit).Name | Should -Be @('s1.txt', 's2.txt', 'r3.txt', 'r4.txt')
        Remove-Item env:MEGAPROVIDER_FAKE_EAGAIN
        Get-ChildItem mega:\ratelimit -Filter s*.txt | Rename-Item -NewName { 'r' + $_.Name.Substring(1) }
    }

    It 'stops the module cmdlets too' {
        $handle = (Get-Item mega:\ratelimit\r4.txt).Handle
        Remove-Item mega:\ratelimit\r4.txt
        $env:MEGAPROVIDER_FAKE_EAGAIN = '5'
        { Restore-MegaItem -Handle $handle -ErrorAction Continue } | Should -Throw '*EAGAIN*'
        Remove-Item env:MEGAPROVIDER_FAKE_EAGAIN
        Restore-MegaItem -Handle $handle | Out-Null
        Test-Path mega:\ratelimit\r4.txt | Should -BeTrue
    }
}

Describe 'What the fake backend cannot do' {
    It 'rejects transfers' {
        { Send-MegaItem $PSCommandPath mega:\docs -ErrorAction Stop } | Should -Throw '*does not simulate*'
    }

    It 'has no account' {
        { Get-MegaAccount -ErrorAction Stop } | Should -Throw '*fake backend has no account*'
    }
}
