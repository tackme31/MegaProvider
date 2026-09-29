# 次のリリースに書くこと

`/release` がリリース文を書くときに読み、出したあとに中身を空（見出しだけ）に戻す。英語のリリース文へ訳して使う。

- 追加: 公開リンクのコマンド `Publish-MegaItem`（作る。`-ExpiresAt` / `-NoExpiry` で期限、`-Password` でパスワード付き）、
  `Unpublish-MegaItem`（消す）、`Get-MegaLink`（一覧。`-Recurse`）。期限とパスワードは MEGA の有料プランだけ
  （無料プランでは断る）。無料プランでは `UrlWithoutKey` と `Key` を別々に渡せる。パスワード付きリンクは
  MEGA に残らないので、その場で保存すること。
- 修正: 版を上げた直後に、裏に残っている古い版のホストへつながっていた（新しいコマンドが通らない、古い版の
  フォルダが消せない）。つないだときに別の版のホストなら止めて、自分のホストを起動し直す。別の版が転送中なら
  入れ替えずにエラーにする。
- 追加: `Get-Help` で各コマンドの説明・例・注意点が読める。`Get-Help about_MegaProvider` に、mega: ドライブで
  標準コマンド（`Remove-Item`、`Move-Item` など）がファイルシステムとどう違うかをまとめた。
- 追加: `Get-ChildItem` の `-Category`（Photo、Video、Document などの種類）と `-Favorite`（お気に入り）。
  `-Recurse` と一緒に使うと MEGA の索引を引くので、大きなドライブでも速い。
- 追加: `mega:` の項目に `CreationTime`（MEGA に置かれた日時）、`IsFavorite`、`HasLink`（公開リンクの有無）、
  `Label`（色ラベル）。`Where-Object` で絞れる。
