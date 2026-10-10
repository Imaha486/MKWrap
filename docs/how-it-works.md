# 仕組み

## 構成

```
src/
  setup/        MKSetup.exe (展開ツール, Win32 GUI / コマンドライン)
    mksetup.c     画面・インストーラ検出・ラッパー配置・INI 生成
    is3.c         InstallShield 3 の .Z アーカイブ展開 (PKWARE DCL implode → zlib contrib/blast)
    is6.c         InstallShield 5/6 キャビネット展開 (unshield を利用) と CD 上の非圧縮ファイルの回収
    profiles.h    タイトル別の初期設定
  wrap/         MKWrap 本体 (dinput.dll / ddraw.dll として配置)
    main.c        初期化・INI 読み込み・本物の dinput.dll への転送
    hooks.c       IAT フック、GetProcAddress フック表 (圧縮実行ファイル対応)
    win.c         窓の仮想化、拡大表示、擬似フルスクリーン、解像度変更の代替
    present_d3d9.c Direct3D9 による拡大表示
    ddraw_emu.c   DirectDraw 1～7 のソフトウェア実装
    input.c       XInput → キー入力の変換 (DirectInput / Win32 API / メッセージ)
    timing.c      時計の補正、timeSetEvent の置き換え、画面転送のレート制限、一時停止
    patch.c       [Patch] の適用とフレーム待ち
    dsound_hk.c   一時停止中に DirectSound の再生を止める
    reg.c         レジストリの INI 化
    cd.c          仮想 CD-ROM ドライブ
third_party/
  unshield/     InstallShield キャビネット展開ライブラリ (MIT)
  blast/        PKWARE DCL implode 伸長 (zlib License)
tools/
  sendcmd.c     Wine での動作確認用 (窓へ WM_COMMAND やキーを送る)
```

## 工程1: 展開

展開元フォルダとその 2 階層下までを探し、最初に見つかった形式で展開します。

1. `data1.hdr` → InstallShield 5/6 (unshield)
2. `data1.cab` (先頭が `ISc(`) → InstallShield 5 (ヘッダ一体型)
3. `SETUP.INS` と同じ場所の `.Z` (先頭が `13 5D 65 8C`、`_` で始まる名前は除く) → InstallShield 3
4. `.msi` → 非圧縮ならフォルダをコピー、圧縮なら `msiexec /a` (管理者用インストール)

InstallShield 5/6 のファイルグループのうち `<Engine>` `<Support>` はインストーラ自身の部品なので除外します。
キャビネットに中身が無いファイル (CD 上に非圧縮で置かれている) は、名前とサイズが一致するファイルを
展開元から探してコピーします。ファイル名は Shift_JIS として解釈し、日本語名のまま書き出します。

インストーラが見つからなければ、指定フォルダを展開済みのゲームフォルダとして工程2へ進みます。

## 工程2: ラッパーの配置

ゲームフォルダの実行ファイルのうち、`dinput.dll` か `ddraw.dll` をインポートしているものをゲーム本体とみなします。
DirectInput を使うなら `dinput.dll`、使わず DirectDraw だけなら `ddraw.dll` という名前でラッパーを置きます
(同じ DLL が両方の関数をエクスポートしています)。Windows はアプリのフォルダの DLL を先に読み込むので、
ゲームの起動時にラッパーが読み込まれます。

## ラッパーの動作

### フック
起動時にゲーム本体の IAT (インポート表) を書き換えます。UPX などで圧縮された実行ファイルは、
展開処理が `GetProcAddress` で関数を解決するため、`GetProcAddress` 自体をフックし、
フック表に登録された関数を返します。

### 窓の仮想化
最初に作られるトップレベル窓を「ゲーム窓」として登録します (WM_CREATE より前に登録するため CBT フックを使用)。

- ゲームには元のサイズ (論理サイズ) に見せる: `GetClientRect` / `GetWindowRect` / `WM_SIZE` を論理サイズに置き換え
- 実際の窓は拡大し、ゲームが指定した位置と中心が一致するように置く
- `GetDC` / `BeginPaint` は論理サイズのキャンバス (32bpp DIB) を返し、`StretchDIBits` / `BitBlt` などの後で拡大表示
- 作成前に取得された窓の DC への描画も `WindowFromDC` で判定してキャンバスへ向け直す
- `ChangeDisplaySettings` / DirectDraw の `SetDisplayMode` は実際には行わず、擬似フルスクリーンにする

### 表示するスレッド
窓への表示 (Direct3D9 / GDI) は、その窓を作ったスレッドだけで行います。描画用スレッドを持つゲームでは、
描画スレッドはキャンバスを書き換えて表示を依頼するメッセージを送るだけです。別スレッドから窓へ描画すると、
ダイアログを開いた時などに窓のスレッドとの間でロック待ちが起き、固まる原因になるためです。

### 枠なし窓の扱い
DirectDraw の全画面用に作られた枠なし (WS_POPUP) の窓は、窓表示に戻すときにタイトルバーと枠を付けます
(`FramePopupWindow`)。ゲームが `GetWindowLong` で読むスタイルは元のままに見せます。

### 表示の依頼の間隔
描画スレッドからの表示依頼 (投稿メッセージ) が途切れなく届くと、窓のスレッドはメッセージを取り出すたびに
投稿メッセージを優先するため、ダイアログの描画 (WM_PAINT) や入力が後回しにされ続けます。
そこで、表示にかかった時間以上 (最低 4ms、モーダルダイアログ表示中は 1/30 秒) 間隔を空けてから次の依頼を送ります。
間引いた分は 15ms のタイマーで表示します。

### 応答なし・異常終了の記録
監視スレッドが 1 秒ごとにゲームの窓へ `WM_NULL` を送り、応答しない状態が続くと記録します。
例外で落ちるときは、例外フィルタ (ゲームが設定したものより先に呼ばれる) とベクタ例外ハンドラ (スタック溢れ用) が記録します。
どちらも、`rundll32.exe` で MKWrap の `MkwDump` を実行し、ゲームのプロセスの外から
全スレッドの実行位置 (EIP)・EBP の連鎖・スタック上の戻り番地 (直前が call 命令のもの) をテキストで、
あわせてミニダンプ (`MiniDumpWriteDump`) を書き出します。

### ダイアログと高 DPI
ラッパーはぼやけを避けるためプロセスを Per-Monitor V2 の DPI 対応にしますが、ゲームのダイアログ
(`CreateDialog*` / `DialogBox*` / `PropertySheet`) は作成の間だけスレッドを DPI 非対応 (GDI スケーリング) に切り替えて作ります。
1998 年当時の作りのダイアログを、当時と同じ条件で動かすためです。

### DirectDraw
すべてのサーフェスを DIB セクションで持ち、プライマリへの転送をキャンバスへ流します。
窓モードのプライマリは画面座標で転送されるので、窓の原点を引いてキャンバス座標に変換します。
排他モード (フルスクリーン) ではプライマリが専用バッファを持ち、Flip でキャンバスへ変換します。

### 入力
XInput の状態を 4ms ごとに読み、割り当てたキーを
DirectInput のキーボード (`GetDeviceState` / `GetDeviceData`)、`GetAsyncKeyState` / `GetKeyState` / `GetKeyboardState`、
(設定により) `WM_KEYDOWN` / `WM_KEYUP` に合成します。メニューのショートカット (F2 = クレジットなど) は
メッセージでないと効かないため、既定で F1～F12 はメッセージも送ります。

### 速度
多くのタイトルは「GetTickCount で一定 ms ごとにフレームを進める」作りで、本来の周期 (60.606Hz など) からずれています。
`GameHz` と `TargetHz` を指定すると、ゲームが読む時計の進みを `TargetHz / GameHz` 倍にして補正します。
時計は QueryPerformanceCounter から作るので、Windows 標準の約 16ms 刻みの粗さもなくなります。

## 調査用の機能

新しいタイトルの調査には `[Debug]` を使います。

- `Log=1`: 毎 5 秒、画面転送回数、内容が変化したフレーム数、GetTickCount/timeGetTime の呼び出し回数と呼び出し元アドレスを記録します。
  呼び出し元アドレスを逆アセンブルすると、フレーム同期処理の作りと周期がわかります。
- `MeasureIRQ=1`: Z80EMU.dll / DLL6809.dll を使うタイトルで、CPU 割り込み要求の回数 (= 実際のフレーム数) を記録します。
- `Log=2`: GetProcAddress で解決された関数、メニュー構成とコマンド ID、ファイルアクセス、メッセージボックスの文面を記録します。
- `TestInput=`: 決まった時刻にキーを押したことにできるので、パッドが無い環境でも操作を確認できます。

Wine で確認する場合は `WINEDLLOVERRIDES="dinput=n,b"` (または `ddraw=n,b`) を指定してください。
