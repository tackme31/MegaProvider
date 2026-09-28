# 次のリリースに書くこと

`/release` がリリース文を書くときに読み、出したあとに中身を空（見出しだけ）に戻す。英語のリリース文へ訳して使う。

- 修正: `mega:` の項目に `Length`・`LastWriteTime`・`Mode` を足した（`ls` の列名と同じ）。今までは
  列名だけで、`Select-Object Length` は空、`$_.Length` は PowerShell 組み込みの 1 になり、
  `Where-Object { $_.Length -gt 1MB }` のような絞り込みが黙って外れていた。フォルダの `Length` は `$null`。
  `Size`・`Modified` はそのまま残っている。
- README に「1.0 までは互換性なしに変わりうる」旨の Note を足した。以後、破壊的変更は
  リリース文の Breaking changes に列挙する。
