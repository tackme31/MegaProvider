# Provider behaviour on the in-memory FakeBackend: no account, no network. Run via scripts/test.ps1,
# which starts a fresh pwsh with MEGAPROVIDER_BACKEND=fake (the backend is fixed per process).
#
# The fake tree lives for the whole process, so the mutating tests below leave it as they found it
# or work on items nothing else looks at.

BeforeAll {
    if ($env:MEGAPROVIDER_BACKEND -ne 'fake') { throw 'Run with MEGAPROVIDER_BACKEND=fake (scripts/test.ps1 does).' }
    Import-Module $env:MEGAPROVIDER_PSD1 -Force
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
        Get-ChildItem mega:\photos -Recurse -File -Name | Should -Contain '2024\IMG_0001.jpg'
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
        (Resolve-Path mega:\docs\*.txt).Path | Should -Be @('mega:\docs\readme.txt', 'mega:\docs\notes.txt')
        Split-Path mega:\docs\readme.txt -Leaf | Should -Be 'readme.txt'
        Join-Path mega:\docs readme.txt | Should -Be 'mega:\docs\readme.txt'
    }

    It 'completes paths with Tab' {
        (TabExpansion2 'Get-ChildItem mega:\docs\re' 27).CompletionMatches.CompletionText | Should -Contain 'mega:\docs\readme.txt'
    }

    # Known limitation, PowerShell's side: completion for non-file-system providers
    # (CompletionCompleters.GetDefaultProviderResults) puts every child of the folder into a
    # dictionary keyed by name, so a folder holding same-named siblings throws and completes nothing.
    It 'completes paths with Tab in a folder with same-named siblings' -Skip {
        (TabExpansion2 'Get-ChildItem mega:\ph' 22).CompletionMatches.CompletionText | Should -Contain 'mega:\photos'
    }

    It 'formats listings like the file system, grouped by folder' {
        $text = Get-ChildItem mega:\docs | Out-String -Width 120
        $text | Should -Match 'Folder: mega:\\docs'
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
        $binned | Restore-MegaItem | ForEach-Object PSPath | Should -BeLike '*::docs\notes.txt'
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
        (Copy-Item mega:\docs\readme.txt mega:\copies -PassThru).PSPath | Should -BeLike '*::copies\readme.txt'
        (Get-Item mega:\copies\readme.txt).Handle | Should -Not -Be (Get-Item mega:\docs\readme.txt).Handle
        (Get-Item mega:\copies\readme.txt).Size | Should -Be 1200
    }

    It 'copies under a new name' {
        Copy-Item mega:\docs\notes.txt mega:\copies\notes-copy.txt
        (Get-Item mega:\copies\notes-copy.txt).Size | Should -Be 340
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

Describe 'What the fake backend cannot do' {
    It 'rejects transfers' {
        { Send-MegaItem $PSCommandPath mega:\docs -ErrorAction Stop } | Should -Throw '*does not simulate*'
    }

    It 'has no account' {
        { Get-MegaAccount -ErrorAction Stop } | Should -Throw '*fake backend has no account*'
    }
}
