# コマンド一覧（実装済み・候補）

2026-09-27 時点。コマンド名は `docs/APPROVED_VERBS.md` の承認された動詞だけを使う。

コマンドは 2 種類ある。

- **標準のコマンド**（`Get-ChildItem` など）: PowerShell 本体のもの。プロバイダが対応するメソッドを
  実装すると `mega:` で動く。新しいコマンドは増えない。
- **モジュールのコマンド**（`Connect-MegaAccount` など）: このモジュールが足すもの。標準のコマンドで
  表せないこと（認証、ローカルとの転送、ゴミ箱からの復元など）に使う。

## 1. 実装済み

### モジュールのコマンド

| コマンド | 内容 |
|---|---|
| `Connect-MegaAccount [-Credential] <PSCredential>` | ログインし、セッションを `%LOCALAPPDATA%\MegaProvider\session.dat` に保存する。前のアカウントがあれば差し替える |
| `Get-MegaAccount` | 今つながっているアカウント（`Email`）を返す |
| `Disconnect-MegaAccount` | ログアウトし（サーバー側でもセッションを無効にする）、保存したセッションを消す |
| `Send-MegaItem [-Path] <ローカル> [-Destination] <mega:\フォルダ>` | アップロード（ファイル・フォルダ）。`ls C:\x \| Send-MegaItem -Destination mega:\y` で使える。同名のファイルがあれば版が積まれる |
| `Receive-MegaItem [-Path] <mega:\...> [[-Destination] <ローカルのフォルダ>]` | ダウンロード。`ls mega:\x -File \| Receive-MegaItem -Destination C:\y` で使える。既定の保存先は今のファイルシステムの場所 |
| `Get-MegaRubbishItem [[-Name] <ワイルドカード>]` | ゴミ箱の一番上の階層を、ハンドル付きで一覧する |
| `Restore-MegaItem [-Handle] <ハンドル> [-Destination <mega:\フォルダ>]` | ゴミ箱から、`Remove-Item` する前の場所（または `-Destination`）へ戻す。`Get-MegaRubbishItem x \| Restore-MegaItem` で使える |

### 標準のコマンド

すべて既定のバックエンド（SDK の常駐プロセス、`docs/HOST.md`）で動く。下の表の MEGAcmd 列は
`$env:MEGAPROVIDER_BACKEND = 'megacmd'`、偽列は `'fake'` のときの状態。

| コマンド | MEGAcmd | 偽 | 備考 |
|---|---|---|---|
| `Set-Location`（`cd`） | ✅ | ✅ | 大文字と小文字の違いは吸収する |
| `Get-ChildItem`（`ls`） | ✅ | ✅ | `-Recurse`、`-Filter`、`-Name`、`-File`、`-Directory` に対応。表示はファイルシステムと同じ並び（`.format.ps1xml`） |
| `Get-Item` | ✅ | ✅ | |
| `Test-Path` | ✅ | ✅ | |
| タブ補完 | ✅ | ✅ | |
| `Resolve-Path` / `Split-Path` / `Join-Path` | ✅ | ✅ | PowerShell 側の汎用処理。ワイルドカードも効く |
| `Rename-Item` | ✅ | ✅ | 兄弟に同じ名前があれば拒否する（一括の名前変更で 2 つが 1 つの名前に重なる事故を防ぐ）。MEGAcmd では大文字小文字も無視して比べる |
| `Move-Item` | ✅ | ✅ | 移動先は既存のフォルダ。移動先に同名があれば拒否する（MEGA は上書きせず同名の兄弟を作るため） |
| `Remove-Item` | ✅ | ✅ | ゴミ箱への移動。元の場所を記録し、`Restore-MegaItem` で戻せる。完全削除はしない方針 |
| `New-Item -ItemType Directory` | ✅ | ✅ | 同名のフォルダがあると拒否される（サーバーの仕様） |
| `-WhatIf` / `-Confirm` | ✅ | ✅ | 変更系の操作と、`Send-` / `Receive-` / `Restore-MegaItem` で効く |

**同名の兄弟**: `ls`・`cd`・`Get-Item`・`Test-Path` は最初に一致したものを見せる。変更系（`Rename-` / `Move-` /
`Remove-` / `New-Item`）と `Send-` / `Receive-` / `Restore-MegaItem` は、パスのどこかに同名の兄弟があると
「ambiguous」のエラーで拒否する（別のものを変更・転送する事故を防ぐため）。自分で同名を作る操作も拒否するので、
同名ができるのは他のクライアントで作られた場合だけ。

`Send-` / `Receive-MegaItem` は偽バックエンドでは使えない（転送は模擬していない）。既定のバックエンドでは
進捗バーが出て、Ctrl+C で転送を取り消せる。`Restore-MegaItem` は既定のバックエンドでは MEGA 本来の復元先を
使うので、他のアプリで削除したものも元の場所へ戻る（MEGAcmd では独自に記録したものだけ）。

## 2. 実装しておくとよいもの

優先度は、パイプラインで一括処理を書くというこのプロジェクトの目的に照らしてつけた。

### 標準のコマンド（プロバイダの対応を足す）

| 優先 | コマンド | 内容・論点 |
|---|---|---|
| 中 | `Copy-Item`（`mega:` の中で） | `CopyItem` を実装する。ファイルのコピーは版が積まれ、フォルダのコピーは同名の兄弟ができる（MEGA の仕様） |
| 中 | `Get-Content` | `IContentCmdletProvider` を実装する。テキストの小さなファイルを読む用途。MEGAcmd には `cat` がある（未実測） |
| 低 | `Set-Content` / `Add-Content` | MEGA のファイルは書き換えられず、アップロードすると新しい版になる。どう見せるかは未決 |
| 低 | `Get-ItemProperty` / `Set-ItemProperty` | ラベルやお気に入りなど MEGA の属性を扱う（`IPropertyCmdletProvider`） |

**PowerShell の `Copy-Item` は、プロバイダをまたいでコピーできない**（`C:\` ⇔ `mega:\`）。
実際に試すと「ソース パスと宛先パスが同じプロバイダーに解決されませんでした」になる。そのため、
ローカルとの転送はモジュールのコマンド（`Send-` / `Receive-MegaItem`）にしている。

### モジュールのコマンド（名前は案）

| 優先 | コマンド | 内容・論点 |
|---|---|---|
| 中 | `Publish-MegaItem` / `Unpublish-MegaItem` | 公開リンクを作る・消す。作ったリンクを返す |
| 中 | `Get-MegaItemVersion` | ファイルの版の一覧（MEGAcmd の `ls --versions`）。アップロードやコピーで版が積まれるので、それを確認する手段 |
| 中 | `Get-MegaAccount -Detailed` | 容量の使用状況などを足す（MEGAcmd の `whoami -l` / `df`）。新しいコマンドにはせず、スイッチで |
| 低 | `Import-MegaItem <公開リンク>` | 他の人の公開リンクを自分のアカウントへ取り込む（MEGAcmd の `import`） |
| 低 | `Get-MegaTransfer` | 大きな転送をバックグラウンドで行うようにしたときの進捗確認 |

### 作らないもの

| コマンド | 理由 |
|---|---|
| 完全削除（`Remove-Item -Permanent`、`Clear-MegaRubbish` など） | undo が無いので実装しない方針（CLAUDE.md） |
| 版の削除（`Remove-MegaItemVersion`） | 同じく完全削除にあたる |

## 3. コマンド以外で必要なもの

- **ハンドルでの指定**（`mega:\#h1a2b3c` のような書き方）: 同名の兄弟を確実に指すため。いまは同名の兄弟を
  含むパスへの変更を拒否しているので、必要になるまで保留。
  同名の兄弟がいるフォルダでタブ補完が効かない問題（PowerShell の補完が子の名前を辞書のキーにしていて、
  重複で例外になる）も、兄弟ごとに別のパスを与えられれば解ける。`tests/Fake.Tests.ps1` に Skip で残してある。
- **2FA**: `Connect-MegaAccount` に `-AuthCode` を足す。
- **EAGAIN の実地確認**: 既定のバックエンドは 4 回まで送り直すが、実際に EAGAIN を引き当てて確かめてはいない。
