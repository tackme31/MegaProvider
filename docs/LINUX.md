# Linux 対応の調査（2026-09-28）

> **状態: 調査のみ（未着手）。** コードはまだ Windows 専用。macOS には対応しない。

コードを読んで Windows に依存している箇所を洗い出し、置き換え方と進める順序を決めた記録。
実機（Linux）ではまだ何も確かめていない。「要確認」と書いたものは、手を付けるときに実物で確かめる。

## 結論

**移植はできる。作業は追加が中心で、書き換えにはならない。**

- C# のプロバイダ本体（`MegaCloudProvider.cs`、コマンド群）と `FakeBackend` は OS に依存しない。
- native の `src/core`・`src/mega`・`src/host/Service.cpp` も OS に依存しない（MegaExplorer の調査
  `../MegaExplorer/docs/investigations/STUDY_CROSS_PLATFORM_BUILD.md` と同じ結論）。
- MEGA SDK はもともと Linux を想定しており、vcpkg の triplet `x64-linux-mega` も SDK に同梱されている。
- Windows 依存は **ホストとの通信（名前付きパイプ）**、**セッションの保存（DPAPI）**、**ビルドとスクリプト**
  の 3 か所にまとまっている。

## Windows に依存している箇所

| 箇所 | Windows 依存 | Linux での置き換え |
|---|---|---|
| `Backend/BackendHost.cs` | Windows 以外では起動を拒否する | fake と host を通す |
| `Backend/SessionStore.cs` | DPAPI（`ProtectedData`） | 0700 のディレクトリの中の 0600 のファイル（下の「セッションの保存」） |
| `Backend/Host/HostClient.cs` | パイプ名にユーザーの SID（`WindowsIdentity`）、`megaprovider-host.exe` 決め打ち、`UseShellExecute = true` での起動 | ソケットのパス、拡張子なし、`UseShellExecute = false`（下の「ホストの起動」） |
| `HostAuth.cs`・`HostBackend.cs`・`HostClient.cs`・`SessionStore.cs` | `[SupportedOSPlatform("windows")]` | 外す、または OS ごとに分ける |
| `native/src/host/main.cpp` | `wmain`・`_wtoi`、`CreateNamedPipeW` と SDDL の DACL、`"\\"` を足した SDK のパス、`SetErrorMode`、`ExitProcess` | `main`、Unix ドメインソケット、`/`、`_exit` |
| `native/CMakeLists.txt` | `/W4 /external:W0 /utf-8`、`NOMINMAX` などの定義、`advapi32` | `if(WIN32)` で分ける。GCC では SDK にならって `-Wall -Wextra` |
| `native/CMakePresets.json` | VS 2022 ジェネレータ、`x64-windows-mega` | Linux 用のプリセットを足す（Ninja、`x64-linux-mega`） |
| `scripts/dev.ps1` | vswhere で探す VS 付属の CMake、SID からパイプ名 | OS で分ける |
| `scripts/host-request.ps1` | SID からパイプ名 | ソケットのパス |
| `scripts/package.ps1` | vswhere、VC++ ランタイムのコピー、`win-x64.zip` | `linux-x64.tar.gz`（下の「配布」） |
| `tests/Live.Tests.ps1` | 2 つ目のホストを `.exe` で起動し、SID からパイプ名を作る | OS で分ける |
| README / `docs/HOST.md` | Windows 前提の記述 | 追記 |

## ホストとの通信

Windows の名前付きパイプを Unix ドメインソケットに替える。プロトコル（1 行 1 JSON）は変えない。

- **置き場所**: `$XDG_RUNTIME_DIR/megaprovider/host.sock`。`XDG_RUNTIME_DIR` が無ければ
  `~/.local/share/MegaProvider/host/`。ディレクトリは 0700、ソケットは 0600 にする。起動したユーザーだけが
  開けるという、いまの DACL と同じ性質になる。
- **C# 側は `NamedPipeClientStream` のまま使える見込み。** .NET は Unix でパイプ名に絶対パスを渡すと、
  それをそのままソケットのパスとして使う（相対の名前だと `/tmp/CoreFxPipe_<名前>`）。**要確認。**
- **二重起動の判定**: Windows では `FILE_FLAG_FIRST_PIPE_INSTANCE` で判定している。Unix では bind が
  `EADDRINUSE` なら connect を試し、応答があれば別のホストが動いているので終わる。応答が無ければ前のホストが
  残した古いソケットなので、消して bind し直す。
- **`SIGPIPE`**: 切れたソケットへ書くとプロセスごと落ちる。無視するか `send(..., MSG_NOSIGNAL)` にする。
  Ctrl+C での転送の取り消しは「進捗の書き込みに失敗したら取り消す」作りなので、ここを忘れると取り消しの
  たびにホストが死ぬ。
- `main.cpp` の通信部分（待ち受け、1 接続の読み書き、切断）を `Transport_win.cpp` / `Transport_unix.cpp` に
  分ければ、残りは共通にできる。

## ホストの起動

- **`UseShellExecute = true` は使えない。** Linux の .NET ではこれが xdg-open 経由の起動になる。
- Windows でシェル経由にした理由（`docs/HOST.md`）は、pwsh のリダイレクトされた標準出力をホストが握り続け、
  `pwsh -File x.ps1 | sed` が終わらなくなることだった。Linux では `UseShellExecute = false` で起動し、
  **ホストが起動直後に `setsid()` して 0/1/2 を `/dev/null` へ付け替える**ことで防ぐ。`setsid()` には、
  端末を閉じたときの SIGHUP でホストが道連れにならない効果もある。
- .NET が自分で開くファイル記述子には CLOEXEC が付くので、0/1/2 以外は継承されない見込み（**要確認**）。

## セッションの保存（決定: 0600 のファイル）

Windows は DPAPI で暗号化したファイル。Linux では次の 3 つを比べ、1 に決めた（2026-09-28）。

1. **0600 のファイル**（`~/.local/share/MegaProvider/session.dat`）。依存が無く、WSL・SSH 先・ヘッドレスの
   サーバーでも動く。守りはファイルのパーミッションだけ。
2. **libsecret**（Secret Service、GNOME Keyring など）。DPAPI に近い守り方になる。ただし D-Bus とキーリングの
   デーモンが要り、ヘッドレスの環境や WSL では使えないことが多い。C# からは P/Invoke になり、実行時に
   `libsecret-1.so.0` が要る。
3. 1 で始めて、あとで 2 を足す。

**MEGA の公式クライアントは、Linux ではどちらも OS の保護を使っていない**（2026-09-28、各 master で確認。
MEGAsync のコードは制限的なライセンスなので、読んで仕組みを知っただけ）。

- **MEGAsync**: 設定ファイル（`MEGAsync.cfg`）の値を、キーのハッシュで XOR してから OS の暗号化に渡す作り。
  Windows では OS の暗号化が DPAPI（`CryptProtectData`）で、このプロジェクトの Windows 版はこれにならった。
  Linux の実装は暗号化を上書きしておらず、既定の「そのまま返す」が使われる。鍵の元になる値も固定値
  なので、実質は難読化だけ。ファイルのパーミッションを明示的に設定しているコードは見当たらない。
- **MEGAcmd**: 設定ディレクトリの `session` にトークンを平文で書く。ディレクトリは作るときに 0700 にしている。

→ **1（0700 のディレクトリの中の 0600 のファイル）にする**。MEGAcmd と同じ守り方で、MEGAsync の
固定鍵の XOR は守りにならないので真似しない。どれにしても `SessionStore` はインターフェースにして
OS ごとに実装し、`BackendHost` で選ぶ。

## ビルド

- SDK の CMake は、`v142` のツールセット指定を Windows のときだけ行う（`sdklib_variables.cmake`）。Linux では
  Ninja も Make も使える。
- このプロジェクトは SDK を `add_subdirectory` で取り込んでいるので、SDK の triplet 自動選択は走らない。
  Windows と同じく、プリセットで triplet を指定する。
- **readline**: Linux では SDK の `USE_READLINE` が既定で ON になり、`pkg_check_modules(readline REQUIRED)` で
  失敗する（`sdklib_options.cmake`）。ホストはコンソールを使わないので `-DUSE_READLINE=OFF` にする。
- **vcpkg の機能**: いまは `use-openssl;use-freeimage;use-pdfium;use-libuv`。freeimage と pdfium は、アップロード時に
  SDK がサムネイルとプレビューを作るのに使う。Linux で最初のビルドが重すぎれば外すことを検討する。外すと、
  Linux からアップロードした画像や PDF にサムネイルが付かなくなる（Windows と差が出る）。
- 最初の vcpkg のビルドには、`build-essential cmake ninja-build pkg-config autoconf autoconf-archive libtool curl zip`
  などが要る（MegaExplorer の調査より。実際に足りないものはビルドして確かめる）。
- WSL でビルドするときは、リポジトリを WSL 側のファイルシステム（`~/` の下）に置く。`/mnt/<ドライブ>`
  越しでは vcpkg のビルドが極端に遅くなる。
- clone 後の準備は `git submodule update --init` と `native/third_party/vcpkg/bootstrap-vcpkg.sh`。

## プロバイダのパス（要確認）

- `MegaCloudProvider.ToProviderPath` は `\` 区切りでパスを返し、`ItemCommands.cs` は `"mega:\\" + ...` を組み立てる。
  Linux の pwsh は `\` と `/` の両方を区切りとして受け付けるはずだが、`PSPath` の見え方、`Split-Path` /
  `Join-Path`、タブ補完の結果がどうなるかは、偽バックエンドのテストを Linux で流して確かめる。
- MEGA の名前に `\` や `/` が入っていると、いまもパスでは指せない（`ToMegaPath` がどちらも区切りにする）。
  Linux ではローカルのファイル名に `\` を使えるので、`Send-MegaItem` でそういう名前のものができうる。
  稀なのでアップロード時に拒否すれば足りる。

## 配布

- 形式は `MegaProvider-<version>-linux-x64.tar.gz`。zip だとホストの実行ビットが失われる。
- 依存ライブラリは vcpkg が静的にリンクする。VC++ ランタイムに当たるものは同梱しない。
- 動くかどうかは glibc の版で決まる。**ビルドする環境の glibc より古いディストリでは動かない**ので、
  古めの LTS（Ubuntu 22.04 など）でビルドする。
- サードパーティの告知（`vcpkg_installed` から組み立てる部分）は同じ作りで使える。
- 対象は x64 だけから始める（arm64 は要望があれば。triplet `arm64-linux-mega` はある）。

## 進める順序

1. **Windows のまま進める下準備。** Windows のテスト（`-Live` も）で退行がないことを確かめながら進める。
   - `main.cpp` の通信部分を分け、`CMakeLists.txt` に `if(WIN32)` を入れる。
   - `HostClient` の名前・パス・起動方法、`SessionStore`、`BackendHost` を OS で切り替える。
2. **Linux の開発環境**（WSL2 の Ubuntu など）: .NET SDK 8、pwsh 7.4 以上、Pester 5.5 以上、ビルドの依存を揃える。
3. **偽バックエンドのテストを Linux で流す。** ネイティブは不要。パスの問題が出るならここ。
4. **Linux のプリセットでホストをビルドする。** `host-request.ps1` で 1 要求ずつ確かめ、テスト用アカウントで
   `test.ps1 -Live` を流す。
5. **スクリプトの OS 分岐と配布**（`dev.ps1`、`test.ps1`、`host-request.ps1`、`package.ps1`）。
6. **文書**: README（動作環境、インストール）、`docs/HOST.md`（ソケット）、`CLAUDE.md`。
7. （任意）GitHub Actions の ubuntu で、偽バックエンドのテストとネイティブのビルドを回す。
