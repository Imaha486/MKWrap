# MKWrap

メディアカイト (Media Kite) から発売された、アーケード／ファミコン移植 PC ソフト (1999～2003 年頃) を
**Windows 10 / 11 で快適に遊ぶため**のツール一式です。

- **MKSetup.exe** — CD (または CD の中身のフォルダ) から、インストーラを実行せずにゲームのファイル一式を
  **展開だけ**します。16bit InstallShield のように 64bit Windows では動かないインストーラにも対応します。
  レジストリやスタートメニューは変更しません。
- **MKWrap (dinput.dll / ddraw.dll)** — ゲームフォルダに置くだけで働く汎用ラッパーです。
  拡大表示、擬似フルスクリーン、XInput コントローラ対応、速度の補正などを INI ファイルで設定できます。

> ゲーム本体のファイルはこのリポジトリに含まれていません。お手持ちの CD から展開してください。
> 本プロジェクトはメディアカイト社および各原作メーカーとは関係ありません。

## 機能

| 区分 | 内容 |
|---|---|
| 展開 | InstallShield 3 (`.Z`)、InstallShield 5/6 (`data1.hdr` / `data1.cab`、CD 上の非圧縮ファイル込み)、Windows Installer (非圧縮 `.msi`、圧縮 `.msi` は管理者用インストール) |
| 表示 | 任意倍率の拡大 (窓の枠をドラッグして自由に拡縮も可)、Alt+Enter で擬似フルスクリーン (縦横比維持・整数倍指定可)、Direct3D9 または GDI で描画 |
| DirectDraw | DirectDraw 1～7 をソフトウェアで肩代わり (8/16/32bpp、パレット、カラーキー、フリップ)。ゲームの解像度変更は実際には行わず擬似フルスクリーンで表示 |
| GDI 描画のゲーム | 窓の DC への描画を論理サイズのキャンバスに受け、拡大表示 |
| 入力 | XInput コントローラのボタン/スティックを任意のキー (同時押し可) に割り当て。DirectInput・GetAsyncKeyState・メニューのショートカット (F1～F12) に効く |
| 速度 | 時計の進みを補正して本来の周期 (60.606Hz など) に合わせる、1ms 精度のタイマー、timeSetEvent の置き換え、フレーム同期処理の置き換えパッチ |
| そのほか | 一時停止 (キー/パッド、タイトルバーに「 - Pause」、音も停止)、レジストリの INI への置き換え、仮想 CD-ROM ドライブ、高 DPI 対応 |

## 動作確認済みタイトル

| タイトル | 実行ファイル | インストーラ | 速度補正 |
|---|---|---|---|
| リブルラブル | LR1500.EXE | InstallShield 3 | フレーム同期を置き換え → 60.606Hz |
| ドラゴンバスター | DB1500.EXE | InstallShield 3 | 62.5Hz → 60.606Hz |
| 妖怪道中記 | YD1500.EXE | InstallShield 3 | 58.8Hz → 60.606Hz |
| 奇々怪界 | KKKAIKAI.exe | InstallShield 3 | 58.8Hz → 60Hz |
| ディグダグ | digdug.exe | InstallShield 6 | なし (元から 60.606Hz) |
| ニューラリーX | nrallyX.EXE | InstallShield 6 | なし |
| パックマン | PacMan.exe | InstallShield 6 | なし (元から 60.606Hz) |
| スプラッターハウス | splatter.exe | Windows Installer | 60Hz → 60.606Hz |
| レインボーアイランド | Rainbow.exe | InstallShield 6 | なし |
| 忍者じゃじゃ丸くん | Jajamaru.exe | InstallShield 6 | なし |
| 熱血高校ドッジボール部 | DodgeBall.exe | InstallShield 6 | なし |
| ぽっぷんぽっぷ | PopnPop.exe | InstallShield 5 (CD 上の非圧縮ファイル) | 62Hz → 58.97Hz、仮想 CD |
| バブルボブル | bublbobl.exe | InstallShield 3 | 62.5Hz → 59.185Hz |
| ガンスターヒーローズ (セガゲーム本舗) | GUNSTAR.exe | なし (CD から直接起動、フォルダをコピー) | 垂直同期 60Hz → 59.92Hz |

未登録のタイトルや、割り当てがゲームと合わない場合は、ゲーム中に **Ctrl+F12** (またはパッドの LB+RB+Back を 1.5 秒押す) で
「パッドのボタン → キーボードのキー」の順に押すだけで割り当てを覚えさせられます (mkwrap.ini に保存)。

各タイトルのボタン割り当てや注意点は [docs/titles.md](docs/titles.md) を参照してください。
動作確認は Linux 上の Wine で行っています (実機 Windows での報告を歓迎します)。

## 使い方

1. [Releases](../../releases) から `MKWrap-<version>.zip` を入手して展開します。
2. `MKSetup.exe` を起動し、「展開元」に CD ドライブ (ISO をマウントしたドライブでも可) を、「展開先」に保存先フォルダを指定して「実行」します。
3. 展開先のゲーム本体 (例 `LR1500.EXE`) を起動します。

- インストーラが見つからないフォルダを指定した場合は「展開済みのゲームフォルダ」とみなし、ラッパーと INI の配置だけを行います。
- コマンドライン: `MKSetup.exe <展開元> <展開先> [/nowrap]`、`MKSetup.exe /wrap <ゲームフォルダ>`
- やめるときは、ゲームフォルダの `dinput.dll` (または `ddraw.dll`) を削除するだけです。

設定項目は [docs/ini-reference.md](docs/ini-reference.md)、仕組みは [docs/how-it-works.md](docs/how-it-works.md) を参照してください。

## ビルド

MinGW-w64 (i686) でビルドします。

```sh
# Debian / Ubuntu
sudo apt install gcc-mingw-w64-i686 libz-mingw-w64-dev make zip
make            # build/dinput.dll, build/MKSetup.exe
make dist       # dist/MKWrap-<version>.zip
```

Windows では MSYS2 の MINGW32 環境で `pacman -S mingw-w64-i686-gcc mingw-w64-i686-zlib make zip` のあと `make CROSS=` を実行します。
GitHub Actions でもビルドされます (`.github/workflows/build.yml`)。

## 新しいタイトルへの対応

1. `MKSetup.exe` で展開すると、未登録タイトル用の汎用 INI が作られます。
2. ゲームの「操作の設定」に合わせて `[Input]` を書き換え、必要なら `[Timing]` で速度を補正します。
3. うまく動いた設定は `src/setup/profiles.h` に追加してください (Pull Request 歓迎)。
   調べ方は [docs/how-it-works.md](docs/how-it-works.md) の「調査用の機能」を参照してください。

## ライセンス

MKWrap 本体は [MIT License](LICENSE) です。同梱ライブラリのライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。

---

### English summary

Tools to run Media Kite's Windows ports of classic arcade/Famicom games (1999–2003) on modern Windows.
`MKSetup.exe` extracts game files from the original CDs without running their (often 16-bit) installers,
and `MKWrap` (a proxy `dinput.dll` / `ddraw.dll`) adds scaling, borderless fullscreen, XInput support,
speed correction, registry virtualization and CD-ROM emulation, all configured through `mkwrap.ini`.
No game data is included. Build with MinGW-w64 (`make`).
