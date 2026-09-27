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

- 動くもの:
  - MEGAcmd バックエンド（実アカウント）: `Connect-` / `Get-` / `Disconnect-MegaAccount`、
    `cd` / `ls` / `-Filter` / `-Recurse` / `Get-Item` / `Test-Path` / タブ補完。**読み取りのみ**。
  - 偽バックエンド（`FakeBackend`、`$env:MEGAPROVIDER_BACKEND = 'fake'`）: 上に加えて `Rename-Item` /
    `New-Item -ItemType Directory` / `Move-Item` / `Remove-Item` / `-WhatIf`。
- **次の一手: MEGAcmd バックエンドの変更系**（`mkdir` / `mv` / `rm`）。同名の兄弟ができる挙動を
  MEGAcmd で実測してから書く。
- バックエンドは 2 段階で考えている。
  1. **MEGAcmd を裏で呼ぶ**（`MEGAclient.exe ls -l ...` 等の出力を解析）。C++ を書かずに実アカウントで
     操作感を確かめられる。インストール済みで、実測した出力形式や挙動は `docs/MEGACMD.md`。
     そこに無いことは実物で確認すること — 記憶で解析器を書かない。
  2. 解析の遅さや脆さが気になったら、**自前のデーモン**に差し替える。候補は MegaExplorer の
     `megatool`（`IMegaClient` の上の CLI）に、名前付きパイプで JSON を返す `serve` を足したもの。
     大きなアカウントは `fetchNodes` に数分かかるので、コマンドのたびにログインする作りは成り立たず、
     常駐するプロセスが必要になる。
- **認証の方針（決定済み）**:
  - `Connect-MegaAccount` でログインし、セッションを自前のファイルに保存する。その後は `mega:` を
    自由に使える。形式は MegaExplorer にならう（セッショントークンを DPAPI で暗号化）が、
    **ファイルは MegaExplorer と共有しない**（完全に別プロジェクト）。
  - 同時に扱えるアカウントは 1 つだけ。`Connect` し直したら差し替える。
  - 2FA は後で対応する（MEGAcmd は `--auth-code` で受けられる）。
- 未対応: `-File` / `-Directory`（`GetChildItemsDynamicParameters` が要る）、`Copy-Item`、
  アップロードとダウンロード（`Get-Content` や、ローカルとの `Copy-Item` をどう表すかは未決）、
  書式ファイル（`.format.ps1xml`）、テスト（Pester を想定）。

## 構成

```
src/MegaProvider/
  MegaCloudProvider.cs     プロバイダ本体。PowerShell のパス ⇔ MEGA のパスの変換はここだけ
  AccountCommands.cs       Connect- / Get- / Disconnect-MegaAccount
  Backend/IMegaBackend.cs  バックエンドと認証の境界。パスは '/' 区切りでルート相対（"" がルート）
  Backend/BackendHost.cs   実装を選ぶ唯一の場所（差し替えるときはここだけ変える）
  Backend/FakeBackend.cs   メモリ上の偽物。同名の兄弟（dup.txt ×2）をわざと含む
  Backend/MegaCmd/         MEGAclient.exe を呼ぶ実装。セッションは %LOCALAPPDATA%\MegaProvider\session.dat
  MegaProvider.psd1        モジュールマニフェスト（ビルド出力へコピーされる）
scripts/dev.ps1            ビルドして、モジュールを読み込んだ新しい pwsh を開く
docs/APPROVED_VERBS.md     PowerShell の承認された動詞の一覧（命名の参照用）
docs/MEGACMD.md            MEGAcmd の実測メモ（認証、出力形式、終了コード）
docs/COMMANDS.md           実装済み・候補のコマンド一覧
```

MEGA を触るコードはすべて `IMegaBackend` の向こうに置く。プロバイダから直接 MEGAcmd を呼ばない。

## ビルドと実行

```powershell
dotnet build                     # MegaProvider.sln
./scripts/dev.ps1                # ビルド → 新しい pwsh で Import-Module → Set-Location mega:
./scripts/dev.ps1 -Fake          # 偽バックエンドで開く（アカウント不要）
./scripts/dev.ps1 -NoShell       # ビルドして .psd1 のパスを出すだけ（スクリプトからの確認用）
```

- 対象は `net8.0` で、`PowerShellStandard.Library` の参照アセンブリを使ってビルドする。
  実行時は pwsh 7.6（.NET 10）の本物の `System.Management.Automation` が使われる。
  入っている SDK は 9 までなので、`net10.0` は指定できない。
- **読み込んだ DLL はアンロードできず、ロックもされる。** 修正を確かめるたびに新しい pwsh プロセスを
  立てること。`dev.ps1` のシェルを開いたままだと、次のビルドがコピーに失敗する。
- 動作確認は `pwsh -NoProfile -File <script>.ps1` で非対話に流す。一時スクリプトはスクラッチパッドに置く。
- 警告はエラーとして扱う（`TreatWarningsAsErrors`）。

## PowerShell プロバイダの落とし穴

- プロバイダのインスタンスは**呼び出しごとに作られる**。状態（バックエンド、セッション）は static に持つ。
- パスは `\` 区切りで、cmdlet によって先頭の `\` が付いたり付かなかったりする。正規化は `ToMegaPath` の 1 か所で行う。
- `-Filter` は `ProviderCapabilities.Filter` を宣言した時点でプロバイダの仕事になり、PowerShell は
  代わりにやってくれない（宣言しないと「フィルターをサポートしていない」エラーになる）。
- 変更を伴う操作は必ず `ShouldProcess` を通す。そうしないと `-WhatIf` / `-Confirm` が効かない。

## コマンドの命名

- cmdlet や関数（`dev.ps1` 内のものも含む）は `動詞-名詞` とし、動詞は**承認された動詞だけ**を使う。
  一覧と使い分けは `docs/APPROVED_VERBS.md`（Microsoft Learn の表を写したもの）を見る。URL を開きに行かない。
- 承認されていない動詞は `Import-Module` で警告が出る。表の「回避するシノニム」列にある語
  （`Erase`、`Transfer`、`Put` など）を使いたくなったら、同じ行の動詞（`Remove`、`Move`、`Send`）に言い換える。

## MEGA の仕様（MegaExplorer で実測済みの事実）

詳細は `../MegaExplorer/docs/MEGATOOL.md` と
`../MegaExplorer/docs/investigations/SPEC_NAME_CONFLICT_COPY_MOVE.md`（参照のみ。ここへ書き写さない）。

- **同じフォルダに同名の子を置ける。** 名前は識別子ではなく属性にすぎない。パスで指定すると
  複数に一致しうるので、いまは最初の一致を取っている。handle で直接指定する手段（たとえば
  `mega:\#h1a2b3c` のような書き方）は、どこかで必要になる。
- **名前は大文字と小文字を区別する。** `FakeBackend` は完全一致を優先し、見つからなければ
  大文字小文字を無視して探す（`cd Docs` を Windows らしく通すため）。本物のバックエンドでも同じ方針にするかは未決。
- 同名のまま発行したときの結果は API が決めている。**ファイルのコピーは版が積まれ、それ以外
  （フォルダのコピー、移動）は同名の兄弟ができる**。上書きという操作は API に存在しない。
- **`Remove-Item` はゴミ箱への移動にする。完全削除は実装しない。** undo が無いので、スクリプトが
  暴発したときの最後の砦になる。
- 大量の変更を短時間に投げると EAGAIN（レート制限）が返る。一括処理にはリトライと同時実行数の制限が要る。
- Git Bash は引数の裸の `/` を `C:/Program Files/Git/` に書き換える。MEGA のルートを `/` で渡すコマンドを
  Git Bash から叩くときは注意（megatool はルートを `.` にして避けた）。

## テスト用アカウント

- 実アカウントに対する操作は、**必ずテスト用アカウントで行う**。本番アカウントに対して破壊的な確認をしない。
- アカウント名は `MEGAEXPLORER_TEST_ACCOUNT`（gitignore 済みの `.claude/settings.local.json` の `env`）。
  パスワードは `MEGAEXPLORER_TEST_PASSWORD`（Windows のユーザー環境変数。ファイルには絶対に書かない）。
  変数名は MegaExplorer と共有している。
- テスト用アカウントへのログインは Claude が上の環境変数を使って行ってよい。パスワードは必ず
  変数参照（`"$MEGAEXPLORER_TEST_PASSWORD"`）で渡し、値を表示・ログ出力・ファイル化しない。
  `session` が出すトークンも秘密として扱い、表示しない。
- MEGAcmd のセッションは MegaExplorer とは独立している。操作の前に `whoami` で
  テスト用アカウントと照合する。

## ライセンス

MIT。MEGA SDK は BSD-2-Clause。`meganz/MEGAsync` のソースは制限的なライセンスなので、**コードを
コピーしない**（SDK の使い方の参考にするだけ）。MEGAcmd のコードを取り込むときは、その前にライセンスを確認する。

## 進め方

- ユーザーとのやり取りは日本語。
- コミットは区切りのよいところで自由にしてよい。ブランチは切らず main に直接コミットする（push は頼まれたときだけ）。
- コメントは、コードから読み取れないことだけを 1〜2 行で書く（外部仕様の罠、もっともらしい「修正」への予防線、別ファイルに原因がある制約）。
