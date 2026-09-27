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
  - DATE はファイルなら更新日時、フォルダなら作成日時で、UTC。
  - 名前は行末まで（空白を含みうる）なので、列は左から固定個数で切り、残りを名前とする。
  - ハンドルは `H:XXXXXXXX` で、パスの代わりに指定できる。
- ハンドルで指定したフォルダを `ls` すると、見出しの前に `/Documents/Notes: ` のようなフルパスの行が付く。
- 出力は UTF-8、行末は CRLF（日本語の名前で確認）。
- 1 回の呼び出しは 150ms 前後。フォルダを 1 つずつ `ls` して 52 フォルダ・842 件を辿ると 5.6 秒。
- パスにはワイルドカード（`*`、`?`）が効く。名前に `*` や `?` を含むノードをパスで指すときは要注意。
