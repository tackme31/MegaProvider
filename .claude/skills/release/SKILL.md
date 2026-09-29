---
name: release
description: >-
  Cut a MegaProvider release: bump the minor version in MegaProvider.psd1,
  build and package the Windows zip and the Linux tarball, tag it, push main,
  and create the GitHub release with gh. `/release 0.3.0` overrides the version.
  Only when the user types /release.
disable-model-invocation: true
---

# /release — リリースを 1 本出す

`main` の今の状態を出す。**`/release` を打ったこと自体が `main` とタグの push、GitHub 公開の指示**
なので、途中で承認は取らない。何かが失敗したらそこで止まって報告する。

配布物は 2 つ。Windows の zip は Windows で、Linux の tar.gz は Linux で作る（ネイティブのホストは
その OS でしかビルドできない）。Linux の環境への入り方と、Windows 側のリポジトリの変更の取り込み方は
`CLAUDE.local.md`（マシン固有なのでここには書かない）。

## 1. 前提確認（1 つでも欠けたら何もせずに理由を言って終わる）

Windows で:

```
git rev-parse --abbrev-ref HEAD                     # main
git status --porcelain                              # 空
git fetch origin --tags
git rev-list --count HEAD..origin/main              # 0（origin が先行していない）
gh auth status                                      # ログイン済み
```

`HEAD` が `origin/main` より先行しているのはよい（そのコミットごと出る）。

Linux の環境に入れて、その clone が作業ツリーの変更なしで `HEAD` まで早送りできること
（`git status --porcelain` が空）。

## 2. 版を決めて上げる

- 引数があればそれ（`v` 付きなら剥がす）。
- 無ければ最新タグ（`git describe --tags --abbrev=0`）の **MINOR を 1 上げ、PATCH を 0**
  にする（`v0.2.0` → `0.3.0`）。
- **タグが 1 つも無い（初回）なら、`MegaProvider.psd1` の版をそのまま出す**。

`git tag -l vX.Y.Z` が空、`gh release view vX.Y.Z` が not found であることを確かめる。

版の在処は `src/MegaProvider/MegaProvider.psd1` の `ModuleVersion` だけ（配布物の名前もここから出る。
native の `project(... VERSION)` は使っていないので触らない）。変えるときは書き換えてコミットする。

```
git add src/MegaProvider/MegaProvider.psd1
git commit           # Subject: Bump the version to X.Y.Z（trailer は他のコミットに合わせる）
```

## 3. テストとパッケージ

**Windows** の PowerShell から:

```
./scripts/test.ps1
./scripts/package.ps1
```

**Linux** では、clone を 2. のコミットまで早送りして（`git log -1` が Windows の `HEAD` と同じか見る）から、
pwsh で同じ 2 つを流す。WSL ではホストのビルドの並列数を絞る（`CMAKE_BUILD_PARALLEL_LEVEL=8`。
絞らないと Windows 側のメモリが尽きる）。出来た tar.gz を Windows の `artifacts/` へコピーする。

`package.ps1` は host と module の Release ビルド（`dev.ps1 -Native`。先に動いているホストを止める）
→ LICENSE・生成した THIRD-PARTY-NOTICES.txt（Windows では MSVC ランタイムも）と一緒に固める →
中身の検査（Linux ではホストの実行ビットと、要求する glibc が README の約束（2.35）を超えないこと）→
一時フォルダへ展開し、新しい pwsh で `Import-Module` して偽バックエンドで `mega:\` を一覧、までをやる。
**手で固めない**（検査が抜ける）。出来上がりは次の 2 つで、**名前の版が 2. の版と一致しているか見る。**

- `artifacts/MegaProvider-X.Y.Z-win-x64.zip`
- `artifacts/MegaProvider-X.Y.Z-linux-x64.tar.gz`

`-Live` は既定では流さない（1 分かかり、テスト用アカウントの状態に左右される）。前のタグから
`native/` か `src/MegaProvider/Backend/` に変更があれば、両方の OS で流す。

## 4. タグ、push、公開

```
git tag -a vX.Y.Z -m "MegaProvider X.Y.Z"
git push origin main
git push origin vX.Y.Z
gh release create vX.Y.Z --verify-tag --title "vX.Y.Z" --notes-file <一時ファイル> \
    artifacts/MegaProvider-X.Y.Z-win-x64.zip artifacts/MegaProvider-X.Y.Z-linux-x64.tar.gz
```

リリース文は**英語**で、変化の 1〜2 行のあとに導入手順を毎回付ける（一時ファイルはスクラッチパッドに置く）。
まず `docs/NEXT_RELEASE.md` に書き溜めたことを入れる（出したあとは中身を空に戻してコミットする）。
残りの変化は前のタグからの `git log --oneline` を眺めて、ユーザーに見える変化を一言で書く（初回なら
"First release." 程度）。内部の変更しかなければ "Minor fixes and internal changes."。
`--draft` / `--prerelease` は指示されたときだけ。

**破壊的変更があれば `### Breaking changes` を立てて 1 つずつ列挙する。** 1.0 までは互換性を
保たない（README の Note）ので、その代わりにここで必ず知らせる。前のタグからの差分
（`git diff vA.B.C..HEAD -- src/ README.md`）を見て、今までのスクリプトが動かなくなる・結果が
変わるものを拾う: cmdlet・パラメーター・プロパティの改名や削除、出力の型や値の変化（`$null` と 0 など）、
既定の動作の変化、セッションなど保存しているファイルの形式、要求環境（PowerShell や glibc の版）。
各項目は「何が変わったか」と「どう書き換えればよいか」を 1 行で。無ければ節ごと省く。

**既知の問題は `### Known issues` に列挙する。** 元は `docs/COMMANDS.md` の「4. 既知の問題」。そこから
利用者が踏みうるもの（誤動作、誤解を招く表示、止まったように見えるなど）を選び、「どんなときに・何が起きるか」と、
回避策があればそれを 1 行で書く。内部の事情（原因のコードや直し方の案）は書かない。PowerShell 側の制約で
直せないものは "(PowerShell limitation)" と添える。この版で直したものは Known issues から外し、変化の行に
"Fixed: ..." として書く。「4.」が空なら節ごと省く。

````
<変化の 1〜2 行>

### Breaking changes

- `<何が変わったか>` — <どう書き換えればよいか>

### Known issues

- <どんなときに・何が起きるか>. <回避策があれば>

PowerShell 7.4 or later on Windows x64, or Linux x64 with glibc 2.35 or later.

**Windows**

```powershell
$dest = "$HOME\Documents\PowerShell\Modules\MegaProvider\X.Y.Z"
Expand-Archive MegaProvider-X.Y.Z-win-x64.zip $dest
Get-ChildItem $dest | Unblock-File
Import-Module MegaProvider
Connect-MegaAccount
```

**Linux**

```powershell
$dest = "$HOME/.local/share/powershell/Modules/MegaProvider/X.Y.Z"
New-Item -ItemType Directory -Force $dest | Out-Null
tar -xzf MegaProvider-X.Y.Z-linux-x64.tar.gz -C $dest
Import-Module MegaProvider
Connect-MegaAccount
```
````

## 5. 報告

リリース URL、配布物 2 つの名前とサイズ、含まれるコミット数を 2〜3 行で。

## 途中で落ちたとき

push より前ならローカルだけなので戻せるが、**戻す操作（`git tag -d`、`git reset --hard HEAD~1`）は
実行前に見せて確認を取る。** push した後に問題が見つかったら、タグや release を消さずに次の版を出すのが既定。
