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
| `Connect-MegaAccount [-Credential] <PSCredential> [-AuthCode <6桁>]` | ログインし、セッションを `%LOCALAPPDATA%\MegaProvider\session.dat` に保存する。前のアカウントがあれば差し替える。2FA のアカウントでは、`-AuthCode` が無ければその場でコードを尋ねる（非対話ではエラー）。2FA の無いアカウントではコードは無視される（実測） |
| `Get-MegaAccount` | 今つながっているアカウント（`Email`）を返す |
| `Disconnect-MegaAccount` | ログアウトし（サーバー側でもセッションを無効にする）、保存したセッションを消す |
| `Send-MegaItem [-Path] <ローカル> [-Destination] <mega:\フォルダ>` | アップロード（ファイル・フォルダ）。`ls C:\x \| Send-MegaItem -Destination mega:\y` で使える。同名のファイルがあれば版が積まれる |
| `Receive-MegaItem [-Path] <mega:\...> [[-Destination] <ローカルのフォルダ>]` | ダウンロード。`ls mega:\x -File \| Receive-MegaItem -Destination C:\y` で使える。既定の保存先は今のファイルシステムの場所 |
| `Get-MegaRubbishItem [[-Name] <ワイルドカード>]` | ゴミ箱の一番上の階層を、ハンドル付きで一覧する |
| `Restore-MegaItem [-Handle] <ハンドル> [-Destination <mega:\フォルダ>]` | ゴミ箱から、`Remove-Item` する前の場所（または `-Destination`）へ戻す。`Get-MegaRubbishItem x \| Restore-MegaItem` で使える |

### 標準のコマンド

すべて既定のバックエンド（SDK の常駐プロセス、`docs/HOST.md`）で動く。下の表の偽列は
`$env:MEGAPROVIDER_BACKEND = 'fake'` のときの状態。

| コマンド | 偽 | 備考 |
|---|---|---|
| `Set-Location`（`cd`） | ✅ | 大文字と小文字の違いは吸収する |
| `Get-ChildItem`（`ls`） | ✅ | `-Recurse`、`-Filter`、`-Name`、`-File`、`-Directory` に対応。`-Category <種類>`（Photo、Video、Document など。MEGA が拡張子から決める）と `-Favorite`（お気に入り）で絞れる。`-Recurse` と組み合わせると木をたどらず MEGA の索引を 1 回引く（ホストの `search`）。お気に入りのフォルダはそれ自体だけが出る。表示はファイルシステムと同じ並び（`.format.ps1xml`）。`Length`・`LastWriteTime`・`Mode` もファイルシステムと同じ名前で使える（`.types.ps1xml`。フォルダの `Length` は `$null`、`LastWriteTime` は作成日時） |
| `Get-Item` | ✅ | |
| `Test-Path` | ✅ | |
| タブ補完 | ✅ | |
| `Resolve-Path` / `Split-Path` / `Join-Path` | ✅ | PowerShell 側の汎用処理。ワイルドカードも効く |
| `Rename-Item` | ✅ | 兄弟に同じ名前があれば拒否する（一括の名前変更で 2 つが 1 つの名前に重なる事故を防ぐ） |
| `Move-Item` | ✅ | 移動先は既存のフォルダ。移動先に同名があれば拒否する（MEGA は上書きせず同名の兄弟を作るため） |
| `Copy-Item`（`mega:` の中で） | ✅ | 既存のフォルダを指せばその中へ、それ以外なら名前を変えてコピー。コピー先に同名があれば拒否する（MEGA はファイルなら版として積み、中身が同じなら黙って捨て、フォルダなら同名の兄弟を作るため）。フォルダは中身ごとしかコピーできないので `-Recurse` を必須にしている |
| `Remove-Item` | ✅ | ゴミ箱への移動。元の場所を記録し、`Restore-MegaItem` で戻せる。完全削除はしない方針 |
| `New-Item -ItemType Directory` | ✅ | 同名のフォルダがあると拒否される（サーバーの仕様） |
| `-WhatIf` / `-Confirm` | ✅ | 変更系の操作と、`Send-` / `Receive-` / `Restore-MegaItem` で効く |

**同名の兄弟**: `ls`・`cd`・`Get-Item`・`Test-Path` は最初に一致したものを見せる。変更系（`Rename-` / `Move-` /
`Copy-` / `Remove-` / `New-Item`）と `Send-` / `Receive-` / `Restore-MegaItem` は、パスのどこかに同名の兄弟があると
「ambiguous」のエラーで拒否する（別のものを変更・転送する事故を防ぐため）。自分で同名を作る操作も拒否するので、
同名ができるのは他のクライアントで作られた場合だけ。

### 公開リンク

| コマンド | 内容 |
|---|---|
| `Publish-MegaItem [-Path] <mega:\...> [-ExpiresAt <DateTime> \| -NoExpiry] [-Password <SecureString>]` | 公開リンクを作り、リンクの情報を返す。リンクがあれば同じリンクを返す（作り直さない）。期限は `-ExpiresAt` / `-NoExpiry` を付けたときだけ変え、付けなければ今の期限を保つ。`ls mega:\x -File \| Publish-MegaItem` で使える |
| `Unpublish-MegaItem [-Path] <mega:\...>` | 公開リンクを削除する。リンクの無い項目では何もしない。`Get-MegaLink \| Unpublish-MegaItem` で使える |
| `Get-MegaLink [[-Path] <mega:\...>] [-Recurse]` | パスを省くとクラウドドライブの全リンク。パスを付けるとその項目のリンク（無ければ何も返さない）、`-Recurse` でその下も |

返すものは `MegaProvider.MegaLinkInfo`: `Path`（`mega:\...`）、`Name`、`IsFolder`、`Url`、`Created`、
`ExpiresAt`（無期限は `$null`）、`IsExpired`、`IsTakenDown`、`Key`、`UrlWithoutKey`、`UnprotectedUrl`、`IsPasswordProtected`。

- **期限とパスワードは Pro（有料プラン）だけ。** MEGA がそう案内している機能なので、無料アカウントでは
  リンクを作る前に拒否する。期限はサーバーも `EACCESS` で断るが、パスワードは手元の計算だけで作れてしまうので、
  このモジュールの側で止める。
- **パスワード付きリンクはサーバーに何も残らない**（元のリンクを暗号化した別の文字列）。`-Password` を付けたときだけ
  `Url` が `#P!…` になり、後から `Get-MegaLink` で取り出す手段は無い。元のリンク（`UnprotectedUrl`）も有効なまま。
- 無料アカウントでも使える守り方は、鍵を分けて渡すこと（`UrlWithoutKey` と `Key`）。
- 偽バックエンドでは `MEGAPROVIDER_FAKE_PRO=1` で有料プランを模擬する。

作らないもの: リンクの閲覧数・ダウンロード数（SDK に API が無い）、書き込み可能なリンク、コンタクトとのフォルダ共有（別の機能）。

`Send-` / `Receive-MegaItem` は偽バックエンドでは使えない（転送は模擬していない）。既定のバックエンドでは
進捗バーが出て、Ctrl+C で転送を取り消せる。`Restore-MegaItem` は既定のバックエンドでは MEGA 本来の復元先を
使うので、他のアプリで削除したものも元の場所へ戻る。

## 2. 実装しておくとよいもの

優先度は、パイプラインで一括処理を書くというこのプロジェクトの目的に照らしてつけた。

### 標準のコマンド（プロバイダの対応を足す）

| 優先 | コマンド | 内容・論点 |
|---|---|---|
| 中 | `Get-Content` | `IContentCmdletProvider` を実装する。テキストの小さなファイルを読む用途 |
| 低 | `Set-Content` / `Add-Content` | MEGA のファイルは書き換えられず、アップロードすると新しい版になる。どう見せるかは未決 |
| 低 | `Get-ItemProperty` / `Set-ItemProperty` | ラベルやお気に入りなど MEGA の属性を扱う（`IPropertyCmdletProvider`） |

**PowerShell の `Copy-Item` は、プロバイダをまたいでコピーできない**（`C:\` ⇔ `mega:\`）。
実際に試すと「ソース パスと宛先パスが同じプロバイダーに解決されませんでした」になる。そのため、
ローカルとの転送はモジュールのコマンド（`Send-` / `Receive-MegaItem`）にしている。

### モジュールのコマンド（名前は案）

| 優先 | コマンド | 内容・論点 |
|---|---|---|
| 中 | `Get-MegaItemVersion` | ファイルの版の一覧。アップロードやコピーで版が積まれるので、それを確認する手段 |
| 中 | `Get-MegaAccount -Detailed` | 容量の使用状況などを足す。新しいコマンドにはせず、スイッチで |
| 低 | `Import-MegaItem <公開リンク>` | 他の人の公開リンクを自分のアカウントへ取り込む |
| 低 | `Get-MegaTransfer` | 大きな転送をバックグラウンドで行うようにしたときの進捗確認 |
| 低 | `Restore-MegaItem` で同名を拒否 | 元の場所（または `-Destination`）に同名があると、いまは同名の兄弟ができる。他の変更系と同じく拒否すれば「このモジュールは同名を作らない」が揃う |

### 作らないもの

| コマンド | 理由 |
|---|---|
| 完全削除（`Remove-Item -Permanent`、`Clear-MegaRubbish` など） | undo が無いので実装しない方針（CLAUDE.md） |
| 版の削除（`Remove-MegaItemVersion`） | 同じく完全削除にあたる |

## 3. コマンド以外で必要なもの

- **ハンドルでの指定**（`mega:\#h1a2b3c` のような書き方）: 同名の兄弟を確実に指すため。いまは同名の兄弟を
  含むパスへの変更を拒否しているので、必要になるまで保留。
  「4.」の同名の兄弟の 2 つ（`ls -Recurse`、タブ補完）も、兄弟ごとに別のパスを与えられれば解ける。
- **大量処理とレート制限**: 規約上の配慮から、**EAGAIN をわざと引き出す試験（負荷試験）はしない**。
  代わりに次のように確かめた（2026-09-28）。
  - SDK の実装を読んだ: 要求のまとまり全体が -3 / -4 で断られたときは、SDK が自分で送り直す（上限約 10 分の
    指数バックオフ、`megaclient.cpp` の `btcs`）。アプリまで返ってくるのは、まとまりの中の 1 件だけが断られた場合。
  - 送り直しは `RateLimit.Retry`（0.5〜4 秒で 4 回）にまとめ、ホストと偽バックエンドで共有する。偽バックエンドは
    `MEGAPROVIDER_FAKE_EAGAIN=n[@k]` で EAGAIN を模擬でき、`tests/Fake.Tests.ps1` が送り直しと打ち切りを確かめる。
  - 送り直しでも通らなければ、その項目でパイプライン全体を止める（ほかのエラーは項目ごとで先へ進む）。
    止まった項目より前は済み、それ以降は未処理、とメッセージに出る。要求は 1 本ずつ直列に送っている（`HostClient` の lock）。
  - 残り（任意）: 普段の使い方の規模（50 件ほど）の名前変更をテスト用アカウントの砂場で 1 回だけ流し、所要時間を見る。
    件数を増やして繰り返したり、限界を探ったりはしない。

## 4. 既知の問題（未対応）

`/release` がリリース文の Known issues に載せる。利用者から見た症状が分かるように書く。

- 同名のフォルダがあると、`ls -Recurse` がどちらの中身も最初のフォルダのものとして 2 回出す
  （子のパスが名前で作られ、名前で引き直されるため）。ハンドルでの指定ができれば解ける。
- 同名の兄弟がいるフォルダでは、タブ補完が何も出さない。PowerShell の補完が子の名前を辞書のキーにしていて、
  重複で例外になる（PowerShell 側の制約）。`tests/Fake.Tests.ps1` に Skip で残してある。
- 未接続のまま `mega:\フォルダ` を指定すると、「Run Connect-MegaAccount first」のあとに PowerShell の
  「パスが存在しない」も出る。PowerShell が存在確認の例外を必ず「存在しない」にするので、消せない。
- ログイン中の進捗のうち、受信バイト数の表示（`Downloading the file list (x / y)`）は、テスト用アカウントが
  小さく一瞬で届くため、まだ実際に出たのを見ていない。大きなアカウントで `Connect-MegaAccount` すると出るはず。
- 要求のまとまり全体がレート制限で断られている間は、SDK が内部で送り直し続けるので、コマンドが止まったように
  見える（進捗もタイムアウトも出ない）。SDK の一時エラーの通知（`onRequestTemporaryError`）を拾えば知らせられる。
