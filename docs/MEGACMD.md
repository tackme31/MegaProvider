# MEGAcmd の実測メモ

MEGAcmd 2.6.0（公式インストーラ `https://mega.nz/MEGAcmdSetup64.exe`、署名 Mega Limited）を
テスト用アカウントで実際に叩いて確かめたこと（2026-09-27）。winget には無い。

## 配置と起動

- インストール先は `%LOCALAPPDATA%\MEGAcmd`（ユーザー単位、`/S` で無人インストールできる）。
  使うのは `MEGAclient.exe <command> ...`。`mega-*.bat` はその薄いラッパー。
- `MEGAcmdServer.exe` が常駐し、クライアントはそこへ依頼を投げるだけ。ログイン状態はサーバーが
  1 つだけ持つ（同時に 1 エンティティ）。データは `%LOCALAPPDATA%\MEGAcmd\.megaCmd`。
- ヘルプは `MEGAclient.exe <command> --help`。**`MEGAclient.exe help <command>` は、出力が
  コンソールでないと返ってこない**（固まる）。
- Git Bash から `/` 始まりのパスを渡すときは `MSYS_NO_PATHCONV=1` を付ける。

## 認証

- **非対話モードでは `login email password` の形しか通らない**。パスワードを標準入力から渡すと
  `Extra args required in non-interactive mode.`（exit 51）。つまりパスワードは引数に載る。
  ログイン後に `.megaCmd` 以下を検索してパスワードが書かれていないことは確認した。
- `session` → `Your (secret) session is: <80 文字のトークン>`。
- `login <session>` で再開できる。キャッシュが効いて 4 秒ほど（パスワードでの初回は fetchNodes が走る）。
- `logout --keep-session` はセッションを無効にせずに閉じる。素の `logout` はサーバー側でも無効にする。
- MEGAcmd 自身もセッションを保存していて、次にサーバーが起動したときに勝手に復元する。
- 2FA は `--auth-code=XXXXXX`。

## 出力と終了コード

- エラーは `[<時刻> cmd ERR  <メッセージ>]` の形で出る。終了コードの実例:
  未ログイン 57、引数不足 51、パスが見つからない 53（`Couldn't find /nope`）。
- `whoami` → `Account e-mail: <メールアドレス>`。
- `ls -l --show-handles --time-format=ISO6081_WITH_TIME <path>`:

```
FLAGS VERS      SIZE             DATE          HANDLE NAME
d---    -            - 2026-08-24T16:58:30 H:fSIDyBZC Notes
```

  オプションなしの `ls -l /` ではファイルはこう出る（日付は `11Sep2026 17:06:53` 形式）:

```
FLAGS VERS      SIZE            DATE       NAME
----    1         7270 11Sep2026 17:06:53 archive_pass_test.zip
-ep-    1      5965972 07Jun2020 19:18:08 In_app_viewer_data.mp3
```

  - FLAGS の 1 文字目が種別（`d` フォルダ、`-` ファイル、`r` ルート、`i` 受信箱、`b` ゴミ箱）、
    2〜4 文字目が公開リンク・共有の状態。フォルダの VERS と SIZE は `-`。
  - DATE はファイルなら更新日時、フォルダなら作成日時。**`--help` には UTC とあるが、実際は
    ローカル時刻**（JST 21:04 に作ったフォルダが `21:04`、MEGAcmd 自身のログは 12:04）。
    `--time-format=ISO6081_WITH_TIME` でも同じ。
  - 名前は行末まで（空白を含みうる）なので、列は左から固定個数で切り、残りを名前とする。
  - ハンドルは `H:XXXXXXXX` で、パスの代わりに指定できる。
- ハンドルで指定したフォルダを `ls` すると、見出しの前に `/Documents/Notes: ` のようなフルパスの行が付く。
- 出力は UTF-8、行末は CRLF（日本語の名前で確認）。
- 1 回の呼び出しは 150ms 前後。フォルダを 1 つずつ `ls` して 52 フォルダ・842 件を辿ると 5.6 秒。
- パスにはワイルドカード（`*`、`?`）が効く。名前に `*` や `?` を含むノードをパスで指すときは要注意。
- ゴミ箱は `//bin`（`ls //bin`、`mv <node> //bin`）。

## 変更系（2026-09-27、`/MegaProviderTest` で実測）

- **`mv` には名前変更専用の形が無い**。`mv src dst` は、dst が既存のフォルダならその中へ移動、
  無ければその名前に変更する。
  - `mv a/g.txt a/sub`（`sub` が兄弟のフォルダ）→ 名前変更ではなく `sub` の中へ移動した。
  - 移動先に同名のファイルがあっても上書きせず、**同名の兄弟ができる**。
  - 新しい名前は**フルパスで**書く必要がある。`mv H:x "H:親/新しい名前"` は **exit 0 で何もしない**。
  - 新しい名前の中の `*` `?` はワイルドカードにならない（まだ存在しないので）。`star*q?.txt` にも変えられた。
  - `mv H:x H:フォルダ` と、ハンドル同士の移動はできる。
- `mkdir` も新しい名前なのでフルパス。`mkdir H:親/名前` は `Use -p ...`（exit 51）。
  同名のフォルダがあると `Folder already exists: <名前>`（exit 54）で、同名の兄弟は作らない。
- `rm` は完全削除（`--help` の文言から判断、未実行）。ゴミ箱へは `mv` で送る。
- **`attr` は独自属性しか扱わない**。
  - `attr x -s n newname` は名前を変えず、`n` という独自属性ができるだけ。
  - 独自属性の名前は短くないと `Invalid argument`（`mporigin` は不可、`origin` / `mp_rr` は可）。
  - MEGA 本来の復元先属性（`rr`）は見えない。`-s rr` は独自属性 `rr` を作るだけ。
  - 同名が 2 つあるパスで `attr` すると、どちらに付くかはこちらで選べない（今回は `ls` で先に出たほう）。
    **同名があり得る所はハンドルで指す**。
- `put <ローカル> H:フォルダ` でアップロードできる（フォルダも可）。同名のファイルがあると
  **版が積まれ**（VERS が 2 になる）、ハンドルは新しい版のものに変わる。
  出力は `Upload finished: <MEGA 側のパス>`。
- `get H:x <ローカルのフォルダ>` はその中に保存し、`Download finished: <ローカルのパス>` を出す。
  中身が同じファイルがあれば何もしない。異なれば ` (N)` を付けた別名にする（`--help` より、未実測）。
- 版の一覧は `ls --versions`。
- レート制限（EAGAIN）がどう見えるかは、まだ引き当てていない。
