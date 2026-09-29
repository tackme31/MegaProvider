# 次のリリースに書くこと

`/release` がリリース文を書くときに読み、出したあとに中身を空（見出しだけ）に戻す。英語のリリース文へ訳して使う。

- 追加: 公開リンクのコマンド `Publish-MegaItem`（作る。`-ExpiresAt` / `-NoExpiry` で期限、`-Password` でパスワード付き）、
  `Unpublish-MegaItem`（消す）、`Get-MegaLink`（一覧。`-Recurse`）。期限とパスワードは MEGA の有料プランだけ
  （無料プランでは断る）。無料プランでは `UrlWithoutKey` と `Key` を別々に渡せる。パスワード付きリンクは
  MEGA に残らないので、その場で保存すること。
