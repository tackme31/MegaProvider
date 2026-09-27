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
- 接続がない状態が 60 分続くと自分で終わる（`--idle-minutes`）。
- ログは `%LOCALAPPDATA%\MegaProvider\host\host.log`（起動のたびに上書き）。
- パイプには起動したユーザーしかつながれない（DACL をそのユーザーの SID だけにしている。既定の
  DACL だと Everyone に読み取りが付く）。リモートからの接続も拒否する。

## セッション

- **保存するのは C# 側**（`%LOCALAPPDATA%\MegaProvider\session.dat`、DPAPI）。ホストはメモリにしか持たない。
- C# は操作の前に `status` でホストのセッションを確かめ、ログインしていないか別のセッションなら
  `resume` で保存したセッションを渡す（5 秒以内に確かめていれば省く）。
- MEGAcmd の `session` が出したトークンも、そのまま `resume` できた（どちらも SDK の `dumpSession`）。

## C# 側の振る舞い

- 前の呼び出しから持っていた接続が死んでいたら（アイドル終了、落ちた、ビルドのために止めた）、書き込みの
  時点で分かる。そのときは何も届いていないので、つなぎ直して（＝ホストを起動し直して）送り直す。
  **応答を待つ間に切れたときは送り直さない**（適用済みかもしれない）。
- EAGAIN（-3）は 0.5 秒から倍々で 4 回まで送り直す。適用されていないことを意味するので安全。
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
→ {"id":1,"op":"list","args":{"handle":"fSIDyBZC"}}
← {"id":1,"ok":true,"result":[{"handle":"...","name":"Notes","folder":true,"size":0,"mtime":1756022310}]}
← {"id":2,"ok":false,"error":{"code":-9,"message":"Not found"}}
← {"id":3,"progress":{"done":1048576,"total":5242880}}      （転送中。最後に ok の行が来る）
```

- ハンドルは MEGA の 8 文字の base64（Web 版や MEGAcmd の `H:xxxxxxxx` と同じ）。フォルダの引数で
  `null` や省略はクラウドドライブのルート。
- `mtime` は Unix 時間（秒）。
- エラーコード: 負の値は SDK の `MegaError`（-3 EAGAIN、-9 ENOENT、-12 EEXIST、-13 EINCOMPLETE＝転送の中断、
  -26 2FA が必要）。正の値はホスト独自（1 要求の形が不正、2 ログインしていない）。
- **転送の中断は、クライアントが接続を切ることで伝える**。ホストは進捗を書けなくなった時点で転送を取り消す。

| op | args | result |
|---|---|---|
| `status` | — | `{loggedIn, email?, session?}` |
| `login` | `{email, password, authCode?}` | `{email, session}`（`fetchNodes` まで終えてから返る） |
| `resume` | `{session}` | `{email}`。同じセッションを持っていれば何もしない |
| `logout` | — | サーバー側でもセッションを無効にする |
| `list` | `{handle?}` | 子の一覧 |
| `rubbish` | — | ゴミ箱の一番上の階層 |
| `path` | `{handle}` | `{root: "cloud"\|"rubbish"\|"other", names: [...]}`（ルートを除く祖先と自分） |
| `restoreTarget` | `{handle}` | `{parent, fellBackToRoot}`。MEGA 本来の復元先。元のフォルダが無ければルート |
| `mkdir` | `{parent?, name}` | 同名のフォルダがあると -12 |
| `rename` | `{handle, name}` | |
| `move` | `{handle, parent?, name?}` | 同名があっても兄弟ができるだけ（上書きしない） |
| `trash` | `{handle}` | ゴミ箱へ移す |
| `upload` | `{local, parent?}` | 進捗の行のあと `{handle}`。同名のファイルがあれば版が積まれる |
| `download` | `{handle, local}` | 進捗の行のあと `{local}`（名前が埋まっていれば ` (1)` が付いた実際のパス） |
| `shutdown` | — | 応答を返してから終了する |
