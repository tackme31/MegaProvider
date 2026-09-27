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

### 標準のコマンド

MEGAcmd 列は実アカウント、偽列は `FakeBackend`（`$env:MEGAPROVIDER_BACKEND = 'fake'`）での状態。

| コマンド | MEGAcmd | 偽 | 備考 |
|---|---|---|---|
| `Set-Location`（`cd`） | ✅ | ✅ | 大文字と小文字の違いは吸収する |
| `Get-ChildItem`（`ls`） | ✅ | ✅ | `-Recurse`、`-Filter`、`-Name` に対応 |
| `Get-Item` | ✅ | ✅ | |
| `Test-Path` | ✅ | ✅ | |
| タブ補完 | ✅ | ✅ | |
| `Resolve-Path` / `Split-Path` / `Join-Path` | ✅ | ✅ | PowerShell 側の汎用処理。ワイルドカードも効く |
| `Rename-Item` | ❌ 未実装 | ✅ | |
| `Move-Item` | ❌ 未実装 | ✅ | |
| `Remove-Item` | ❌ 未実装 | ✅ | ゴミ箱への移動。完全削除はしない方針 |
| `New-Item -ItemType Directory` | ❌ 未実装 | ✅ | |
| `-WhatIf` / `-Confirm` | — | ✅ | 変更系の操作と一緒に効く |

## 2. 実装しておくとよいもの

優先度は、パイプラインで一括処理を書くというこのプロジェクトの目的に照らしてつけた。

### 標準のコマンド（プロバイダの対応を足す）

| 優先 | コマンド | 内容・論点 |
|---|---|---|
| 高 | `Rename-Item` / `Move-Item` / `Remove-Item` / `New-Item`（MEGAcmd） | 偽ではもう動くので、MEGAcmd に繋ぐだけ。同名の兄弟ができるときの挙動を先に実測する。一括処理のためにリトライ（EAGAIN）と同時実行数の制限も要る |
| 高 | `Get-ChildItem -File` / `-Directory` | `GetChildItemsDynamicParameters` を実装する。`ls -File \| Rename-Item` のような使い方で必須 |
| 高 | 表示の書式（`.format.ps1xml`） | コマンドではないが、`ls` の見た目を決める。今は `MegaItem` のプロパティがそのまま表に並んでいる。`Mode` / `LastWriteTime` / `Length` / `Name` のように、ファイルシステムと同じ並びにしたい |
| 中 | `Copy-Item`（`mega:` の中で） | `CopyItem` を実装する。ファイルのコピーは版が積まれ、フォルダのコピーは同名の兄弟ができる（MEGA の仕様） |
| 中 | `Get-Content` | `IContentCmdletProvider` を実装する。テキストの小さなファイルを読む用途。MEGAcmd には `cat` がある（未実測） |
| 低 | `Set-Content` / `Add-Content` | MEGA のファイルは書き換えられず、アップロードすると新しい版になる。どう見せるかは未決 |
| 低 | `Get-ItemProperty` / `Set-ItemProperty` | ラベルやお気に入りなど MEGA の属性を扱う（`IPropertyCmdletProvider`） |

**PowerShell の `Copy-Item` は、プロバイダをまたいでコピーできない**（`C:\` ⇔ `mega:\`）。
実際に試すと「ソース パスと宛先パスが同じプロバイダーに解決されませんでした」になる。そのため、ローカルとの転送は下のモジュールのコマンドにする。

### モジュールのコマンド（名前は案）

| 優先 | コマンド | 内容・論点 |
|---|---|---|
| 高 | `Send-MegaItem -Path <ローカル> -Destination <mega:\...>` | アップロード。パイプラインで `ls C:\photos \| Send-MegaItem -Destination mega:\photos` のように使う |
| 高 | `Receive-MegaItem -Path <mega:\...> -Destination <ローカル>` | ダウンロード。`Save-MegaItem`（`Save-Module` と同じ言い回し）も候補 |
| 高 | `Get-MegaRubbishItem` / `Restore-MegaItem` | `Remove-Item` がゴミ箱への移動なので、元に戻す手段があると安全装置として完結する |
| 中 | `Publish-MegaItem` / `Unpublish-MegaItem` | 公開リンクを作る・消す。作ったリンクを返す |
| 中 | `Get-MegaItemVersion` | ファイルの版の一覧。コピーで版が積まれるので、それを確認する手段 |
| 中 | `Get-MegaAccount -Detailed` | 容量の使用状況などを足す（MEGAcmd の `whoami -l` / `df`）。新しいコマンドにはせず、スイッチで |
| 低 | `Import-MegaItem <公開リンク>` | 他の人の公開リンクを自分のアカウントへ取り込む（MEGAcmd の `import`） |
| 低 | `Get-MegaTransfer` | 大きな転送をバックグラウンドで行うようにしたときの進捗確認 |

### 作らないもの

| コマンド | 理由 |
|---|---|
| 完全削除（`Remove-Item -Permanent`、`Clear-MegaRubbish` など） | undo が無いので実装しない方針（CLAUDE.md） |
| 版の削除（`Remove-MegaItemVersion`） | 同じく完全削除にあたる |

## 3. コマンド以外で必要なもの

- **ハンドルでの指定**（`mega:\#h1a2b3c` のような書き方）: 同名の兄弟を確実に指すため。
- **2FA**: `Connect-MegaAccount` に `-AuthCode` を足す。
