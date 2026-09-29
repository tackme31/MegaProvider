# CLAUDE.md

## これは何か

MEGA のクラウドストレージを PowerShell の `mega:` ドライブとして見せる **PowerShell プロバイダ**
（`NavigationCmdletProvider`）。`cd mega:\photos`、`Get-ChildItem`、`Rename-Item`、`Move-Item`、
`Remove-Item` が標準コマンドのまま動き、パイプラインで一括処理が書けることが目的。

```powershell
Get-ChildItem mega:\photos -Recurse -Filter *.jpg | Rename-Item -NewName { $_.Name -replace '^IMG_', 'trip_' }
```

兄弟プロジェクト `../MegaExplorer`（Qt/C++ の MEGA クライアント、MIT、同じ作者）から派生した。
**あちらのコードや知見はコピーして持ってくる**方針で、ビルドやパスの上では依存しない。

## 状態: PoC

- 動くもの: 一覧は `docs/COMMANDS.md`。読み取り・変更系・転送（`Send-` / `Receive-MegaItem`）・
  ゴミ箱からの復元まで動く。
- 実アカウントでの確認は `mega:\MegaProviderTest` の下で行う（テスト用アカウントの砂場）。
- Windows と Linux（x64）で動く。macOS には対応しない。Linux 特有のこと（ソケット、セッションの守り方、`ls` が
  `Get-ChildItem` でないこと、ビルドと配布）は `docs/LINUX.md`。変更は両方の OS で確かめる（Linux の環境は `CLAUDE.local.md`）。
- **次の一手**: プロダクトとして出すための残り（`docs/COMMANDS.md` の「4.」にあるパイプ名の版）。そのあと「2. 実装しておくとよいもの」。
  既知の問題は同じファイルの「4.」。
- バックエンドは 2 つあり、`$env:MEGAPROVIDER_BACKEND` で選ぶ（`Backend/BackendHost.cs`）。
  - 既定: **SDK の常駐プロセス**（`native/`、`megaprovider-host.exe`）。MegaExplorer の `IMegaClient` を
    コピーして Qt を外したものに、名前付きパイプで JSON を返す口を付けた。設計とプロトコルは `docs/HOST.md`。
  - `fake`: メモリ上の木。アカウント不要。転送はできない。
- **認証の方針（決定済み）**:
  - `Connect-MegaAccount` でログインし、セッションを自前のファイルに保存する。その後は `mega:` を
    自由に使える。形式は MegaExplorer にならう（セッショントークンを DPAPI で暗号化）が、
    **ファイルは MegaExplorer と共有しない**（完全に別プロジェクト）。
  - Linux では DPAPI の代わりに、0700 のディレクトリの中の 0600 のファイルに保存する（MEGAcmd と同じ守り方。
    理由は `docs/LINUX.md`）。
  - 同時に扱えるアカウントは 1 つだけ。`Connect` し直したら差し替える。
  - 2FA（認証アプリのコード）は `Connect-MegaAccount -AuthCode`、または省略時にその場で尋ねる。
    2026-09-28 にユーザーが 2FA のアカウントで確認済み。

## 構成

```
src/MegaProvider/
  MegaCloudProvider.cs     プロバイダ本体。PowerShell のパス ⇔ MEGA のパスの変換はここだけ
  AccountCommands.cs       Connect- / Get- / Disconnect-MegaAccount
  ItemCommands.cs          Send- / Receive-MegaItem、Get-MegaRubbishItem、Restore-MegaItem
  LinkCommands.cs          Publish- / Unpublish-MegaItem、Get-MegaLink（公開リンク）
  MegaProvider.format.ps1xml  ls の表示（ビルド出力へコピーされる）
  MegaProvider.types.ps1xml   FileInfo と同じ名前のプロパティ（Length、LastWriteTime、Mode）
  Backend/IMegaBackend.cs  バックエンドと認証の境界。パスは '/' 区切りでルート相対（"" がルート）
  Backend/BackendHost.cs   実装を選ぶ唯一の場所（差し替えるときはここだけ変える）
  Backend/FakeBackend.cs   メモリ上の偽物。同名の兄弟（dup.txt ×2）をわざと含む
  Backend/SessionStore.cs  セッションの保存（%LOCALAPPDATA%\MegaProvider\session.dat。Windows は DPAPI）
  Backend/PrivateFiles.cs  本人だけが開けるフォルダ・ファイル（Linux では 0700 / 0600）
  Backend/Host/            megaprovider-host.exe とパイプで話す実装（既定）
  MegaProvider.psd1        モジュールマニフェスト（ビルド出力へコピーされる）
native/                    megaprovider-host（C++、CMake + vcpkg）。src/core・src/mega は MegaExplorer からのコピー
  src/host/Platform_*.cpp  OS ごとの部分（Windows の名前付きパイプ / Unix ドメインソケット）
  third_party/sdk, vcpkg   submodule（MegaExplorer と同じコミット）
scripts/dev.ps1            ビルドして、モジュールを読み込んだ新しい pwsh を開く
scripts/test.ps1           ビルドして Pester を流す（スイートごとに新しい pwsh）
scripts/host-request.ps1   動いているホストへ要求を 1 つ送って応答を見る（プロトコルのデバッグ用）
scripts/Get-HostPipeName.ps1  ホストの接続先（HostClient.PipeName と同じもの）を返す
scripts/package.ps1        Release ビルド → artifacts/ に配布物（Windows は zip、Linux は tar.gz）→ 展開して読み込めるか確認。リリースは /release スキル
tests/Fake.Tests.ps1       偽バックエンドのテスト（アカウント不要）
tests/Live.Tests.ps1       テスト用アカウントでの端から端までのテスト
docs/APPROVED_VERBS.md     PowerShell の承認された動詞の一覧（命名の参照用）
docs/COMMANDS.md           実装済み・候補のコマンド一覧
docs/HOST.md               常駐プロセスの設計、ビルド、プロトコル
docs/LINUX.md              Linux 対応（OS ごとに違う箇所、置き換え方、実物で確かめたこと、配布）
docs/NEXT_RELEASE.md       次のリリース文に書くことの書き溜め（/release が読んで空に戻す）
```

MEGA を触るコードはすべて `IMegaBackend` の向こうに置く。プロバイダから直接ホストを呼ばない。

## ビルドと実行

```powershell
dotnet build                     # MegaProvider.sln
./scripts/dev.ps1                # ビルド → 新しい pwsh で Import-Module → Set-Location mega:
./scripts/dev.ps1 -Fake          # 偽バックエンドで開く（アカウント不要）
./scripts/dev.ps1 -Native        # native（ホスト）も Release でビルドし直す。初回は自動
./scripts/dev.ps1 -NoShell       # ビルドして .psd1 のパスを出すだけ（スクリプトからの確認用）
```

- 対象は `net8.0` で、`PowerShellStandard.Library` の参照アセンブリを使ってビルドする。
  実行時は pwsh（7.4 以上）の本物の `System.Management.Automation` が使われる。
- **読み込んだ DLL はアンロードできず、ロックもされる。** 修正を確かめるたびに新しい pwsh プロセスを
  立てること。`dev.ps1` のシェルを開いたままだと、次のビルドがコピーに失敗する。
- 動作確認は `pwsh -NoProfile -File <script>.ps1` で非対話に流す。一時スクリプトはスクラッチパッドに置く。
- 警告はエラーとして扱う（`TreatWarningsAsErrors`）。
- **ホストの exe も起動中はロックされる。** `dev.ps1` は最初にホストへ `shutdown` を送って止める。
  手で `cmake --build` するときも先に止めること。
- native のビルドは Visual Studio 付属の CMake で。Git Bash から MSBuild に `/m` を渡すとパスに化けるので `-m` と書く。
- git clone したあとは `git submodule update --init` と `native/third_party/vcpkg/bootstrap-vcpkg.bat`（Linux では `.sh`）が要る。
- Linux では `dev.ps1` が `linux` プリセット（GCC + Ninja、出力は `native/build/megaprovider-host`）でビルドする。
  並列数は `CMAKE_BUILD_PARALLEL_LEVEL` で絞れる（詳細は `docs/LINUX.md`）。
- `native/` で clangd が出す「ヘッダが見つからない」類の診断は無視してよい（VS ジェネレータは
  compile_commands.json を作らない）。判断は MSVC のビルド結果で。

## テスト

```powershell
./scripts/test.ps1                                # 偽バックエンドだけ（アカウント不要、数秒）
./scripts/test.ps1 -Live                          # + テスト用アカウントで SDK のホスト（1 分ほど）
```

- Pester 5.5 以上（`Install-Module Pester -Scope CurrentUser`）。Windows 同梱の 3.4 では動かない。
- バックエンドはプロセスごとに固定なので、`test.ps1` はスイートごとに新しい pwsh を立てる。
- `-Live` は最初に `Get-MegaAccount` を `MEGAEXPLORER_TEST_ACCOUNT` と照合し、違えば何もせずに止まる。
  毎回 `mega:\MegaProviderTest\run-<日時>-<乱数>` を作り、最後にゴミ箱へ送る（ゴミ箱には溜まっていく）。
  最後の `Account` は一度 `Disconnect` してからパスワードで `Connect` し直す。
- 変更を入れたら、少なくとも `./scripts/test.ps1` は通す。バックエンドや native に触れたら `-Live` も。
- 既知の制限は `-Skip` で残し、理由をコメントに書く（例: 同名の兄弟がいるフォルダでのタブ補完）。
- 一時スクリプトでアカウントを照合するときは `try { (Get-MegaAccount -ErrorAction Stop).Email } catch { $null }` と書く。
  未接続のエラーは文単位の終了エラーなので、`if ((Get-MegaAccount).Email -ne ...) { throw }` は素通りして先へ進む。
- 非対話で進捗を確かめるには `[powershell]::Create()` で流して `.Streams.Progress` を見る（`4>&1` では取れない）。

## PowerShell プロバイダの落とし穴

- プロバイダのインスタンスは**呼び出しごとに作られる**。状態（バックエンド、セッション）は static に持つ。
- パスは `\` 区切りで、cmdlet によって先頭の `\` が付いたり付かなかったりする。正規化は `ToMegaPath` の 1 か所で行う。
- `-Filter` は `ProviderCapabilities.Filter` を宣言した時点でプロバイダの仕事になり、PowerShell は
  代わりにやってくれない（宣言しないと「フィルターをサポートしていない」エラーになる）。
- 変更を伴う操作は必ず `ShouldProcess` を通す。そうしないと `-WhatIf` / `-Confirm` が効かない。
- `ItemExists` / `IsItemContainer` から出た例外は、終了エラーでも「パスが存在しない」と表示される。理由を伝えたいときは
  先に `WriteError` する（未接続のときがそう）。`-ErrorAction SilentlyContinue` のときは、動的パラメーターの取得中の
  エラーとして終了エラー（`ParameterBindingException`）になる。
- 中身のあるフォルダを `-Recurse` なしで `Remove-Item` すると PowerShell が確認を求め、非対話では落ちる（テストで注意）。
- タブ補完（ファイルシステム以外のプロバイダ）は子の名前を辞書のキーにするので、同名の兄弟がいる
  フォルダでは例外になって何も補完されない。PowerShell 側の都合で、プロバイダからは直せない。

## コマンドの命名

- cmdlet や関数（`dev.ps1` 内のものも含む）は `動詞-名詞` とし、動詞は**承認された動詞だけ**を使う。
  一覧と使い分けは `docs/APPROVED_VERBS.md`（Microsoft Learn の表を写したもの）を見る。URL を開きに行かない。
- 承認されていない動詞は `Import-Module` で警告が出る。表の「回避するシノニム」列にある語
  （`Erase`、`Transfer`、`Put` など）を使いたくなったら、同じ行の動詞（`Remove`、`Move`、`Send`）に言い換える。

## MEGA の仕様（MegaExplorer で実測済みの事実）

詳細は `../MegaExplorer/docs/MEGATOOL.md` と
`../MegaExplorer/docs/investigations/SPEC_NAME_CONFLICT_COPY_MOVE.md`（参照のみ。ここへ書き写さない）。

- **同じフォルダに同名の子を置ける。** 名前は識別子ではなく属性にすぎない。パスで指定すると
  複数に一致しうる。読み取りは最初の一致を取り、変更・転送は拒否する（`MegaCloudProvider.EnsureUnambiguous`）。
  同名は稀なので、handle で直接指定する手段（`mega:\#h1a2b3c` など）は必要になるまで作らない。
- **名前は大文字と小文字を区別する。** `FakeBackend` は完全一致を優先し、見つからなければ
  大文字小文字を無視して探す（`cd Docs` を Windows らしく通すため）。本物のバックエンドでも同じ方針にするかは未決。
- 同名のまま発行したときの結果は API が決めている。**ファイルのコピーは版が積まれ、それ以外
  （フォルダのコピー、移動）は同名の兄弟ができる**。上書きという操作は API に存在しない。
- **`Remove-Item` はゴミ箱への移動にする。完全削除は実装しない。** undo が無いので、スクリプトが
  暴発したときの最後の砦になる。
- 大量の変更を短時間に投げると EAGAIN（レート制限）が返る。送り直しと、諦めたときにパイプラインを止める処理がある
  （`Backend/RateLimit.cs`）。**EAGAIN をわざと引き出す試験はしない**（負荷試験になる）。確かめるときは
  偽バックエンドの `MEGAPROVIDER_FAKE_EAGAIN` で模擬する。
- Git Bash は引数の裸の `/` を `C:/Program Files/Git/` に書き換える。MEGA のルートを `/` で渡すコマンドを
  Git Bash から叩くときは注意（megatool はルートを `.` にして避けた）。

## テスト用アカウント

- 実アカウントに対する操作は、**必ずテスト用アカウントで行う**。本番アカウントに対して破壊的な確認をしない。
- アカウント名は環境変数 `MEGAEXPLORER_TEST_ACCOUNT`、パスワードは `MEGAEXPLORER_TEST_PASSWORD`。
  パスワードはリポジトリ内のファイルには絶対に書かない。
- テスト用アカウントへのログインは Claude が上の環境変数を使って行ってよい。パスワードは必ず
  変数参照（`"$MEGAEXPLORER_TEST_PASSWORD"`）で渡し、値を表示・ログ出力・ファイル化しない。
  ホストの `status` が返すセッショントークンも秘密として扱い、表示しない。
- このモジュールのセッションは MegaExplorer とは独立している。操作の前に `Get-MegaAccount`
  でテスト用アカウントと照合する。

## ライセンス

MIT。MEGA SDK は BSD-2-Clause、nlohmann/json は MIT。`meganz/MEGAsync` のソースは制限的なライセンスなので、**コードを
コピーしない**（SDK の使い方の参考にするだけ）。MEGAcmd のコードを取り込むときは、その前にライセンスを確認する。

## 進め方

- ユーザーとのやり取りは日本語。コミットメッセージは英語で書く。
- このリポジトリは公開している。このマシンに固有のこと（入っているツールの版、テスト用アカウントの事情など）は
  gitignore 済みの `CLAUDE.local.md` に書き、ここや `docs/` には書かない。
- コミットは区切りのよいところで自由にしてよい。ブランチは切らず main に直接コミットする（push は頼まれたときだけ）。
- コメントは、コードから読み取れないことだけを 1〜2 行で書く（外部仕様の罠、もっともらしい「修正」への予防線、別ファイルに原因がある制約）。
