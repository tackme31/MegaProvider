# End-to-end against the MEGA test account. Run via `scripts/test.ps1 -Live`, which starts one fresh
# pwsh per backend (MEGAPROVIDER_BACKEND is fixed per process).
#
# Safety: nothing runs unless the connected account is MEGAEXPLORER_TEST_ACCOUNT. Everything happens
# in a new folder under mega:\MegaProviderTest, which goes to the Rubbish Bin at the end.

BeforeDiscovery {
    $script:isHost = -not $env:MEGAPROVIDER_BACKEND
}

BeforeAll {
    Import-Module $env:MEGAPROVIDER_PSD1 -Force

    if (-not $env:MEGAEXPLORER_TEST_ACCOUNT) { throw 'MEGAEXPLORER_TEST_ACCOUNT is not set.' }
    $account = try { (Get-MegaAccount -ErrorAction Stop).Email } catch { $null }
    if ($account -ne $env:MEGAEXPLORER_TEST_ACCOUNT) {
        throw "Connected to '$account', not the test account. Run Connect-MegaAccount with the test account first."
    }

    $script:R = "mega:\MegaProviderTest\run-$(Get-Date -Format yyyyMMdd-HHmmss)-$(Get-Random -Maximum 10000)"
    if (-not (Test-Path mega:\MegaProviderTest)) { New-Item mega:\MegaProviderTest -ItemType Directory | Out-Null }
    New-Item $R -ItemType Directory | Out-Null

    $script:local = Join-Path ([IO.Path]::GetTempPath()) "mp-test-$(Get-Random)"
    $script:down = Join-Path $local 'down'
    New-Item "$local\up\sub", $down -ItemType Directory -Force | Out-Null
    'hello' | Set-Content "$local\up\a.txt"
    'jpg1' | Set-Content "$local\up\IMG_0001.jpg"
    'jpg2' | Set-Content "$local\up\IMG_0002.jpg"
    'nihongo' | Set-Content "$local\up\日本語 ファイル.txt"
    'x' | Set-Content "$local\up\sub\x.txt"
}

AfterAll {
    if ($script:R -and (Test-Path $R)) { Remove-Item $R -Recurse -Confirm:$false }
    if ($script:local) { Remove-Item $local -Recurse -Force -ErrorAction SilentlyContinue }
}

Describe 'Transfers and listing' {
    It 'uploads files and a folder through the pipeline' {
        $out = Get-ChildItem "$local\up" | Send-MegaItem -Destination $R
        $out.Name | Sort-Object | Should -Be @('a.txt', 'IMG_0001.jpg', 'IMG_0002.jpg', 'sub', '日本語 ファイル.txt')
        $out[0].PSPath | Should -BeLike 'MegaProvider\Mega::*'
    }

    It 'does not upload under -WhatIf' {
        'w' | Set-Content "$local\whatif.txt"
        Send-MegaItem "$local\whatif.txt" $R -WhatIf
        Test-Path "$R\whatif.txt" | Should -BeFalse
    }

    It 'lists with -File, -Directory and -Recurse' {
        (Get-ChildItem $R -Directory).Name | Should -Be 'sub'
        (Get-ChildItem $R -File).Name | Sort-Object | Should -Be @('a.txt', 'IMG_0001.jpg', 'IMG_0002.jpg', '日本語 ファイル.txt')
        Get-ChildItem $R -Recurse -File -Name | Should -Contain 'sub\x.txt'
    }

    It 'reports sizes and local-time dates' {
        $a = Get-Item "$R\a.txt"
        $a.Size | Should -Be (Get-Item "$local\up\a.txt").Length
        ([datetime]::Now - $a.Modified).Duration() | Should -BeLessThan ([timespan]::FromHours(1))
    }

    It 'downloads through the pipeline, and a folder' {
        $files = Get-ChildItem $R -File | Receive-MegaItem -Destination $down
        $files | Should -HaveCount 4
        Get-Content "$down\日本語 ファイル.txt" | Should -Be 'nihongo'
        (Receive-MegaItem "$R\sub" $down).FullName | Should -Be "$down\sub"
        Get-Content "$down\sub\x.txt" | Should -Be 'x'
    }

    It 'adds a version when the same file is uploaded again' {
        'hello, version 2' | Set-Content "$local\up\a.txt"
        Send-MegaItem "$local\up\a.txt" $R | Out-Null
        $a = @(Get-ChildItem $R -Filter a.txt)
        $a | Should -HaveCount 1
        $a[0].Size | Should -Be (Get-Item "$local\up\a.txt").Length
    }

    It 'reports errors for bad paths' {
        { Send-MegaItem "$local\up\a.txt" mega:\MegaProviderTest\nope -ErrorAction Stop } | Should -Throw
        { Receive-MegaItem "$R\nope.txt" $down -ErrorAction Stop } | Should -Throw
        { Receive-MegaItem "$R\a.txt" mega:\ -ErrorAction Stop } | Should -Throw '*not a file system path*'
    }
}

Describe 'Renaming' {
    It 'renames in bulk through the pipeline' {
        Get-ChildItem $R -Filter IMG_*.jpg | Rename-Item -NewName { $_.Name -replace '^IMG_', 'trip_' }
        (Get-ChildItem $R -Filter *.jpg).Name | Sort-Object | Should -Be @('trip_0001.jpg', 'trip_0002.jpg')
    }

    It 'refuses a name that a sibling already has, and leaves the item alone' {
        { Rename-Item "$R\a.txt" -NewName sub -ErrorAction Stop } | Should -Throw '*already exists*'
        Test-Path "$R\a.txt" | Should -BeTrue
        (Get-ChildItem "$R\sub").Name | Should -Be 'x.txt'
    }

    It 'changes only the case of a name' {
        (Rename-Item "$R\a.txt" -NewName A.txt -PassThru).Name | Should -BeExactly 'A.txt'
    }

    It 'renames to and from a name with wildcard characters' {
        (Rename-Item "$R\A.txt" -NewName 'star*q?.txt' -PassThru).Name | Should -Be 'star*q?.txt'
        (Rename-Item -LiteralPath "$R\star*q?.txt" -NewName A.txt -PassThru).Name | Should -Be 'A.txt'
    }

    # MEGAcmd reads '*' and '?' in a folder path as wildcards, so that backend refuses this.
    It 'works inside a folder whose name has wildcard characters' -Skip:(-not $isHost) {
        New-Item "$R\wild*?" -ItemType Directory | Out-Null
        New-Item "$R\wild*?\inner" -ItemType Directory | Out-Null
        (Rename-Item -LiteralPath "$R\wild*?\inner" -NewName 'inner*2' -PassThru).Name | Should -Be 'inner*2'
    }
}

Describe 'Folders, moving and the Rubbish Bin' {
    It 'creates a folder and refuses a second one of the same name' {
        (New-Item "$R\moved" -ItemType Directory).Name | Should -Be 'moved'
        { New-Item "$R\moved" -ItemType Directory -ErrorAction Stop } | Should -Throw '*already exists*'
    }

    It 'moves into a folder' {
        (Move-Item "$R\trip_0001.jpg" "$R\moved" -PassThru).PSPath | Should -BeLike '*\moved\trip_0001.jpg'
        (Get-ChildItem "$R\moved").Name | Should -Be 'trip_0001.jpg'
    }

    It 'refuses to move onto an existing name (MEGA would add a same-named sibling)' {
        Send-MegaItem "$local\up\IMG_0001.jpg" "$R\moved" | Rename-Item -NewName trip_0002.jpg
        { Move-Item "$R\trip_0002.jpg" "$R\moved" -ErrorAction Stop } | Should -Throw '*already exists*'
        Test-Path "$R\trip_0002.jpg" | Should -BeTrue
        @(Get-ChildItem "$R\moved" -Filter trip_0002.jpg) | Should -HaveCount 1
    }

    It 'removes to the Rubbish Bin and restores to the original folder' {
        $handle = (Get-Item "$R\A.txt").Handle
        Remove-Item "$R\A.txt"
        Test-Path "$R\A.txt" | Should -BeFalse
        $binned = Get-MegaRubbishItem | Where-Object Handle -EQ $handle
        $binned.Name | Should -Be 'A.txt'
        ($binned | Restore-MegaItem).PSPath | Should -BeLike '*\A.txt'
        Test-Path "$R\A.txt" | Should -BeTrue
    }

    It 'removes a folder with -Recurse and restores it elsewhere' {
        $handle = (Get-Item "$R\sub").Handle
        Remove-Item "$R\sub" -Recurse
        Restore-MegaItem -Handle $handle -Destination "$R\moved" | Out-Null
        Get-ChildItem "$R\moved" -Recurse -Name | Should -Contain 'sub\x.txt'
    }
}

# The MEGAcmd backend has no Copy yet (its cp is unmeasured).
Describe 'Copying' -Skip:(-not $isHost) {
    BeforeAll { New-Item "$R\copies" -ItemType Directory | Out-Null }

    It 'copies a file into a folder under its own name, keeping the original' {
        $copy = Copy-Item "$R\A.txt" "$R\copies" -PassThru
        $copy.PSPath | Should -BeLike '*\copies\A.txt'
        $copy.Handle | Should -Not -Be (Get-Item "$R\A.txt").Handle
        Receive-MegaItem "$R\copies\A.txt" $down | Get-Content | Should -Be 'hello, version 2'
    }

    It 'copies under a new name, and through the pipeline' {
        (Copy-Item "$R\A.txt" "$R\copies\renamed.txt" -PassThru).Name | Should -Be 'renamed.txt'
        Get-ChildItem "$R\moved" -File | Copy-Item -Destination "$R\copies"
        (Get-ChildItem "$R\copies" -File).Name | Sort-Object |
            Should -Be @('A.txt', 'renamed.txt', 'trip_0001.jpg', 'trip_0002.jpg')
    }

    It 'refuses a name that exists in the destination, adding no version' {
        { Copy-Item "$R\trip_0002.jpg" "$R\copies" -ErrorAction Stop } | Should -Throw '*already exists*'
        @(Get-ChildItem "$R\copies" -Filter trip_0002.jpg) | Should -HaveCount 1
        (Get-Item "$R\copies\trip_0002.jpg").Size | Should -Be (Get-Item "$R\moved\trip_0002.jpg").Size
    }

    It 'needs -Recurse for a folder, and then copies everything in it' {
        { Copy-Item "$R\moved" "$R\copies" -ErrorAction Stop } | Should -Throw '*-Recurse*'
        Copy-Item "$R\moved" "$R\copies" -Recurse
        Get-ChildItem "$R\copies\moved" -Recurse -Name | Sort-Object |
            Should -Be @('sub', 'sub\x.txt', 'trip_0001.jpg', 'trip_0002.jpg')
    }
}

Describe 'The SDK host' -Skip:(-not $isHost) {
    It 'comes back by itself after being killed' {
        Get-Process megaprovider-host | Stop-Process -Force
        Start-Sleep -Seconds 3   # past the 2 s listing cache, so the next call reaches the host
        (Get-ChildItem $R).Name | Should -Contain 'moved'
        Get-Process megaprovider-host | Should -Not -BeNullOrEmpty
    }

    # Regression: the host kept claiming the dead session and every call failed until it restarted.
    It 'recovers when its session is revoked elsewhere' -Skip:(-not (Test-Path "$env:LOCALAPPDATA\MEGAcmd\MEGAclient.exe")) {
        (Get-ChildItem $R).Name | Should -Contain 'moved'   # the host holds the current session
        # Through the MEGAcmd backend: logout (kills the session on the server), then a fresh login.
        pwsh -NoProfile -Command {
            param($psd1)
            $env:MEGAPROVIDER_BACKEND = 'megacmd'
            Import-Module $psd1
            Disconnect-MegaAccount
            $password = ConvertTo-SecureString $env:MEGAEXPLORER_TEST_PASSWORD -AsPlainText -Force
            Connect-MegaAccount -Credential ([pscredential]::new($env:MEGAEXPLORER_TEST_ACCOUNT, $password)) | Out-Null
        } -args $env:MEGAPROVIDER_PSD1
        Start-Sleep -Seconds 6   # past HostAuth's 5 s verification window
        (Get-ChildItem $R).Name | Should -Contain 'moved'
        (Get-MegaAccount).Email | Should -Be $env:MEGAEXPLORER_TEST_ACCOUNT
    }

    It 'cancels an upload when the pipeline is stopped (Ctrl+C)' {
        $big = Join-Path $local 'big.bin'
        $f = [IO.File]::OpenWrite($big); $b = [byte[]]::new(1MB); $rnd = [Random]::new(1)
        foreach ($i in 1..256) { $rnd.NextBytes($b); $f.Write($b, 0, $b.Length) }
        $f.Close()
        $ps = [powershell]::Create().AddScript({
                param($psd1, $file, $dest) Import-Module $psd1; Send-MegaItem $file $dest
            }).AddArgument($env:MEGAPROVIDER_PSD1).AddArgument($big).AddArgument($R)
        $async = $ps.BeginInvoke()
        while ($ps.Streams.Progress.Count -lt 2 -and -not $async.IsCompleted) { Start-Sleep -Milliseconds 50 }
        $ps.Stop()
        $ps.InvocationStateInfo.State | Should -Be 'Stopped'
        Start-Sleep -Seconds 3
        Test-Path "$R\big.bin" | Should -BeFalse
    }
}

Describe 'Account' {
    # Last on purpose: Disconnect invalidates the session everything above used.
    It 'disconnects, then connects again with the test credentials' {
        Disconnect-MegaAccount
        { Get-MegaAccount -ErrorAction Stop } | Should -Throw '*Connect-MegaAccount*'
        # The drive root needs no account, so dev.ps1's `Set-Location mega:` works before logging in.
        Push-Location mega:\
        try { { Get-ChildItem -ErrorAction Stop } | Should -Throw '*Connect-MegaAccount*' }
        finally { Pop-Location }
        $password = ConvertTo-SecureString $env:MEGAEXPLORER_TEST_PASSWORD -AsPlainText -Force
        $cred = [pscredential]::new($env:MEGAEXPLORER_TEST_ACCOUNT, $password)
        (Connect-MegaAccount -Credential $cred).Email | Should -Be $env:MEGAEXPLORER_TEST_ACCOUNT
        (Get-MegaAccount).Email | Should -Be $env:MEGAEXPLORER_TEST_ACCOUNT
    }
}
