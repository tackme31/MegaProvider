---
name: release
description: >-
  Cut a MegaProvider release: bump the minor version in MegaProvider.psd1,
  build and package the Windows zip, tag it, push main, and create the GitHub
  release with gh. `/release 0.3.0` overrides the version. Only when the user
  types /release.
disable-model-invocation: true
---

# /release — リリースを 1 本出す

`main` の今の状態を出す。**`/release` を打ったこと自体が `main` とタグの push、GitHub 公開の指示**
なので、途中で承認は取らない。何かが失敗したらそこで止まって報告する。

## 1. 前提確認（1 つでも欠けたら何もせずに理由を言って終わる）

```
git rev-parse --abbrev-ref HEAD                     # main
git status --porcelain                              # 空
git fetch origin --tags
git rev-list --count HEAD..origin/main              # 0（origin が先行していない）
gh auth status                                      # ログイン済み
```

`HEAD` が `origin/main` より先行しているのはよい（そのコミットごと出る）。

## 2. 版を決めて上げる

- 引数があればそれ（`v` 付きなら剥がす）。
- 無ければ最新タグ（`git describe --tags --abbrev=0`）の **MINOR を 1 上げ、PATCH を 0**
  にする（`v0.2.0` → `0.3.0`）。
- **タグが 1 つも無い（初回）なら、`MegaProvider.psd1` の版をそのまま出す**。

`git tag -l vX.Y.Z` が空、`gh release view vX.Y.Z` が not found であることを確かめる。

版の在処は `src/MegaProvider/MegaProvider.psd1` の `ModuleVersion` だけ（zip 名もここから出る。
native の `project(... VERSION)` は使っていないので触らない）。変えるときは書き換えてコミットする。

```
git add src/MegaProvider/MegaProvider.psd1
git commit           # Subject: Bump the version to X.Y.Z（trailer は他のコミットに合わせる）
```

## 3. テストとパッケージ

PowerShell から:

```
./scripts/test.ps1
./scripts/package.ps1
```

`package.ps1` は host と module の Release ビルド（`dev.ps1 -Native`。先に動いているホストを止める）
→ MSVC ランタイム・LICENSE・生成した THIRD-PARTY-NOTICES.txt と一緒に zip → 中身の検査 →
一時フォルダへ展開し、新しい pwsh で `Import-Module` して偽バックエンドで `mega:\` を一覧、
までをやる。**手で zip を作らない**（検査が抜ける）。出来上がりは
`artifacts/MegaProvider-X.Y.Z-win-x64.zip`。**zip 名の版が 2. の版と一致しているか見る。**

`-Live` は既定では流さない（1 分かかり、テスト用アカウントの状態に左右される）。前のタグから
`native/` か `src/MegaProvider/Backend/` に変更があれば流す。

## 4. タグ、push、公開

```
git tag -a vX.Y.Z -m "MegaProvider X.Y.Z"
git push origin main
git push origin vX.Y.Z
gh release create vX.Y.Z --verify-tag --title "vX.Y.Z" --notes-file <一時ファイル> artifacts/MegaProvider-X.Y.Z-win-x64.zip
```

リリース文は**英語**で、変化の 1〜2 行のあとに導入手順を毎回付ける（一時ファイルはスクラッチパッドに置く）。
変化は前のタグからの `git log --oneline` を眺めて、ユーザーに見える変化を一言で書く（初回なら
"First release." 程度）。内部の変更しかなければ "Minor fixes and internal changes."。
`--draft` / `--prerelease` は指示されたときだけ。

````
<変化の 1〜2 行>

Windows x64, PowerShell 7.4 or later.

```powershell
$dest = "$HOME\Documents\PowerShell\Modules\MegaProvider\X.Y.Z"
Expand-Archive MegaProvider-X.Y.Z-win-x64.zip $dest
Get-ChildItem $dest | Unblock-File
Import-Module MegaProvider
Connect-MegaAccount
```
````

## 5. 報告

リリース URL、zip 名とサイズ、含まれるコミット数を 2〜3 行で。

## 途中で落ちたとき

push より前ならローカルだけなので戻せるが、**戻す操作（`git tag -d`、`git reset --hard HEAD~1`）は
実行前に見せて確認を取る。** push した後に問題が見つかったら、タグや release を消さずに次の版を出すのが既定。
