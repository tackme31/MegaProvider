# megaprovider-host（SDK の常駐プロセス）

`native/` にある C++ の常駐プロセス。MEGA SDK の `MegaApi` を 1 つ持ち、ログインしたままノードの木を
メモリに置いて、PowerShell 側（`Backend/Host/`）の要求に名前付きパイプで答える。

## なぜ常駐させるか

大きなアカウントは `fetchNodes` に数分かかる（MegaExplorer で 640k ノード 385 秒）。コマンドのたびに
ログインする作りは成り立たない。常駐していれば 2 回目以降の PowerShell は待たずに済む。SDK の
ノードキャッシュ（`%LOCALAPPDATA%\MegaProvider\host\sdk`）があれば、再起動後の `fetchNodes` も短い。

## コードの出どころ

- `native/src/core/`、`native/src/mega/` は MegaExplorer（同じ作者、MIT）の `src/core`、`src/mega` から
  コピーしたもの。変えたのは Qt への依存だけ:
  - `QThreadPool` → `mega/SerialExecutor.h`（1 本のスレッドで順に実行）
  - `QMetaObject::invokeMethod` での GUI スレッドへの受け渡し → ワーカースレッドのまま `onDone` を呼ぶ
  - `qCWarning` などのログ → `app/Logging.h`
  - `MegaSdkClient::localLogout` を追加（サーバー側のセッションを無効にせずに手放す）
  - `IMegaClient::getLinkDetails`（`core/LinkDetails.h`）を追加（公開リンクの情報をまとめて読む）
- MegaExplorer 側で直したことは手で取り込む。コピーした時点は MegaExplorer の SDK v10.17.0 のころ。
- `native/third_party/sdk`（MEGA SDK v10.17.0）と `native/third_party/vcpkg` は submodule で、
  MegaExplorer と同じコミット。vcpkg のバイナリキャッシュ（`%LOCALAPPDATA%\vcpkg\archives`）が共有される。
- JSON は `native/third_party/json`（nlohmann/json 3.11.3、MIT）。

## ビルド

```powershell
./scripts/dev.ps1 -Native     # 起動中のホストを止め、native を Release でビルドし、dotnet build
```

- Visual Studio 2022 付属の CMake を使う（Git Bash の `cmake` は Strawberry のもの）。
- 初回の configure は vcpkg の依存を揃えるので数分かかる（キャッシュが効けば依存そのものは十数秒）。
- vcpkg の機能は `use-openssl;use-freeimage;use-pdfium;use-libuv`。**FFmpeg は外している**
  （動画のサムネイル生成にしか使わず、triplet で唯一 DLL になるので exe の横に配る手間が増える）。
- ビルドした `native/build/Release/megaprovider-host.exe` は、csproj がモジュールの出力へコピーする。
  依存する DLL は Windows 自身のものと VC++ ランタイム（`MSVCP140.dll` など）だけ。
- **ホストが動いていると exe がロックされてコピーに失敗する**。`dev.ps1` は最初に `shutdown` を送って止める。

## 起動と終了

- モジュールがパイプにつながらないときに起動する
  （`--pipe megaprovider-host-<ユーザー SID> --data %LOCALAPPDATA%\MegaProvider\host`）。
  ウィンドウは出さず、PowerShell が終わっても残る。
- **シェル経由（`UseShellExecute = true`）で起動する**。`UseShellExecute = false` だと pwsh のハンドルを
  継承し、リダイレクトされた標準出力を握り続けるので、`pwsh -File x.ps1 | sed` のようなパイプラインが
  ホストが終わるまで終わらなくなる（実際に固まった）。
- 同じパイプ名で 2 つ目が起動しても、パイプを作れずにすぐ終わる（`FILE_FLAG_FIRST_PIPE_INSTANCE`）。
  パイプを開くのはログや SDK より先なので、2 つ目は 1 つ目のログ（開くと空になる）にもノードキャッシュにも触れない。
- OS ごとに違うのは `native/src/host/Platform_win.cpp` / `Platform_unix.cpp`（待ち受け、1 接続の読み書き、起動元からの
  切り離し）だけ。Linux での置き換え方は `docs/LINUX.md`。接続先の名前はスクリプトからは `scripts/Get-HostPipeName.ps1` で得る。
- 接続がない状態が 60 分続くと自分で終わる（`--idle-minutes`）。
- **別の版のホストは入れ替える。** モジュールは起動するときに `--build-id`（exe のパスと更新日時）を渡し、
  つないだときに `status` の `buildId` と比べる。違えば（0.3.0 までのホストは返さない）`shutdown` を送り、
  パイプが消えるのを待ってから自分のホストを起動する。パイプ名とデータのフォルダは版によらず同じなので、
  SDK のノードキャッシュはそのまま使える。版を上げた直後に古いホストへつながって新しい要求が通らない、
  古い版のフォルダが exe のロックで消せない、を防ぐ。
  - ホストは転送中とログイン中（`upload`、`download`、`login`、`resume`）は `shutdown` を断る（3）。
    モジュールは「別の版が使っている」のエラーで止まり、入れ替えない。`dev.ps1` も止められなかった旨で止まる。
  - 古い版と新しい版の pwsh を同時に使うと、互いにホストを入れ替え合う（そのたびに `resume` が走るだけで、壊れはしない）。
  - 古いモジュールが新しいホストにつながったときは何も確かめない。既存の要求は変えない限り、そのまま通る。
- ログは `%LOCALAPPDATA%\MegaProvider\host\host.log`（起動のたびに上書き）。
- パイプには起動したユーザーしかつながれない（DACL をそのユーザーの SID だけにしている。既定の
  DACL だと Everyone に読み取りが付く）。リモートからの接続も拒否する。

## セッション

- **保存するのは C# 側**（`%LOCALAPPDATA%\MegaProvider\session.dat`、DPAPI）。ホストはメモリにしか持たない。
- C# は操作の前に `status` でホストのセッションを確かめ、ログインしていないか別のセッションなら
  `resume` で保存したセッションを渡す（5 秒以内に確かめていれば省く）。
- **セッションが外で無効にされたとき**（別のクライアントでのログアウト、パスワード変更）、SDK はもう
  `dumpSession` できない。ホストは `status` でそれに気づくか、-15（ESID）を受けた時点で「ログインしていない」
  扱いに戻り、C# が保存したセッションで `resume` し直す。保存したセッションも無効なら
  `Connect-MegaAccount` を促すエラーになる。以前はホストが古いセッションを持っているつもりのまま、
  再起動するまで全部の操作が失敗していた（`Live.Tests.ps1` の回帰テストあり）。

## C# 側の振る舞い

- 前の呼び出しから持っていた接続が死んでいたら（アイドル終了、落ちた、ビルドのために止めた）、書き込みの
  時点で分かる。そのときは何も届いていないので、つなぎ直して（＝ホストを起動し直して）送り直す。
  **応答を待つ間に切れたときは送り直さない**（適用済みかもしれない）。
- EAGAIN（-3）は 0.5 秒から倍々で 4 回まで送り直す（`RateLimit.Retry`）。適用されていないことを意味するので安全。
  それでも通らなければ `MegaRateLimitedException` になり、プロバイダはパイプラインを止める。
- 一覧は 2 秒だけキャッシュし、変更系の要求を送ったら捨てる。PowerShell は出力した項目ごとにパスを
  解決し直すので、キャッシュが無いと祖先の一覧を何度も取り直して解析する（700 件のフォルダの
  `ls -Recurse` が 2.4 秒 → キャッシュありで 0.1 秒以下）。
- Ctrl+C（`StopProcessing`）ではパイプを閉じる。ホストは次の進捗の書き込みに失敗して転送を取り消す
  （512MB のアップロードで確認。ログに `Transfer (UPLOAD) finished with error: Incomplete`）。

## 実測（テスト用アカウント、2026-09-27）

- パイプ 1 往復: `list` 0.16ms、`status` 0.36ms。
- ホストが無い状態からの `Get-MegaAccount`（起動 + `resume` + `fetchNodes`）: 2.3 秒。
  `Connect-MegaAccount`（パスワードでのログイン + `fetchNodes`）: 14〜19 秒。
- `ls mega:\ -Recurse`（861 件）: 初回 0.11 秒、2 回目以降 0.01 秒以下。MEGAcmd では 5.6 秒。
- `Rename-Item` 1 件: 0.3〜0.8 秒（API の往復）。MEGAcmd では 0.8 秒以上。

## プロトコル

1 行に 1 つの JSON（UTF-8、`\n` 区切り）。1 つの接続では要求を 1 つずつ処理する。

```
→ {"id":1,"op":"list","args":{"handle":"xxxxxxxx"}}
← {"id":1,"ok":true,"result":[{"handle":"...","name":"docs","folder":true,"size":0,"mtime":1756022310}]}
← {"id":2,"ok":false,"error":{"code":-9,"message":"Not found"}}
← {"id":3,"progress":{"done":1048576,"total":5242880}}      （転送中。最後に ok の行が来る）
← {"id":4,"progress":{"stage":"download","done":1048576,"total":5242880}}   （login / resume の段階）
```

- `login` / `resume` の進捗は `stage` 付き: `login`（認証）→ `load`（`fetchNodes` 開始）→ `download`
  （ノード一覧の受信バイト数。SDK の状態キャッシュが効くと出ない）→ `build`（復号と木の組み立て。進捗なし）。
  `build` は受信が総量に達したとき、または 8 秒途絶えたときに出る（最後の通知が 100% に届くとは限らない）。

- ハンドルは MEGA の 8 文字の base64（Web 版や MEGAcmd の `H:xxxxxxxx` と同じ）。フォルダの引数で
  `null` や省略はクラウドドライブのルート。
- `mtime` は Unix 時間（秒）。
- エラーコード: 負の値は SDK の `MegaError`（-3 EAGAIN、-9 ENOENT、-11 EACCESS、-12 EEXIST、-13 EINCOMPLETE＝転送の中断、
  -26 2FA が必要）。正の値はホスト独自（1 要求の形が不正、2 ログインしていない、3 転送中・ログイン中なので `shutdown` しない）。
- **転送の中断は、クライアントが接続を切ることで伝える**。ホストは進捗を書けなくなった時点で転送を取り消す。

| op | args | result |
|---|---|---|
| `status` | — | `{loggedIn, email?, session?, buildId}`（`buildId` は起動時の `--build-id`。無ければ空） |
| `login` | `{email, password, authCode?}` | 段階の進捗行のあと `{email, session}`（`fetchNodes` まで終えてから返る） |
| `resume` | `{session}` | 段階の進捗行のあと `{email}`。同じセッションを持っていれば何もしない（進捗行も出ない） |
| `logout` | — | サーバー側でもセッションを無効にする |
| `list` | `{handle?}` | 子の一覧 |
| `search` | `{handle?, recursive, category?, favourite}` | MEGA の索引で絞った子（`recursive` ならフォルダの下すべて）。`category` は `photo`、`audio`、`video`、`document`、`pdf`、`presentation`、`spreadsheet`、`archive`、`program`、`other`（種類を付けるとファイルだけ）。`recursive` のときは各項目に `names`（`path` と同じ） |
| `rubbish` | — | ゴミ箱の一番上の階層 |
| `path` | `{handle}` | `{root: "cloud"\|"rubbish"\|"other", names: [...]}`（ルートを除く祖先と自分） |
| `restoreTarget` | `{handle}` | `{parent, fellBackToRoot}`。MEGA 本来の復元先。元のフォルダが無ければルート |
| `mkdir` | `{parent?, name}` | 同名のフォルダがあると -12 |
| `rename` | `{handle, name}` | |
| `move` | `{handle, parent?, name?}` | 同名があっても兄弟ができるだけ（上書きしない） |
| `copy` | `{handle, parent?, name?}` | フォルダは中身ごと。同名のファイルがあると兄弟ではなく版として積まれ、中身が同じなら何も起きない（どちらも成功が返る）。新しい handle は返らない。フォルダを自分の中へコピーするとエラーにならず、その時点の中身が 1 段だけ複製される（実測） |
| `trash` | `{handle}` | ゴミ箱へ移す |
| `upload` | `{local, parent?}` | 進捗の行のあと `{handle}`。同名のファイルがあれば版が積まれる |
| `download` | `{handle, local}` | 進捗の行のあと `{local}`（名前が埋まっていれば ` (1)` が付いた実際のパス） |
| `link` | `{handle}` | 公開リンク `{url, created, expires, expired, takenDown}`、無ければ `null`。`url` は鍵付き、`expires` の 0 は無期限。手元の読み取りだけ |
| `links` | — | クラウドドライブの公開リンクすべて。`list` の項目に `names`（`path` と同じ）と `link` を足したもの。ゴミ箱の中のものは除く |
| `export` | `{handle, expires?}` | リンクを作る（あれば同じものを返す）。`expires` を省くか `null` なら今の期限を保ち、0 は無期限。無料プランで期限を付けると -11（EACCESS） |
| `unexport` | `{handle}` | リンクを消す。リンクが無くても成功 |
| `protectLink` | `{url, password}` | `{url}`（`#P!` の形）。手元の計算だけで、MEGA には何も残らない。モジュールは有料プランでしか使わない |
| `plan` | — | `{proLevel}`（0 が無料プラン）。サーバーへの問い合わせ |
| `shutdown` | — | 応答を返してから終了する。転送中・ログイン中は 3 で断る |
