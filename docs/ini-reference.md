# mkwrap.ini リファレンス

`mkwrap.ini` はラッパー DLL と同じフォルダに置きます。MKSetup は UTF-16 (BOM 付き) で書き出します
(Windows の INI API がそのまま日本語を扱えるため)。メモ帳で編集できます。

## [Display]

| キー | 既定 | 内容 |
|---|---|---|
| Scale | 0 | 窓表示の倍率 (小数可)。0 = 作業領域に収まる最大の整数倍 |
| Fullscreen | 0 | 1 = 起動時から擬似フルスクリーン |
| FullscreenKey | ENTER | Alt + このキーで擬似フルスクリーン切り替え |
| Filter | 0 | 0 = ニアレストネイバー、1 = なめらか (線形補間) |
| KeepAspect | 1 | 縦横比を保って黒帯を付ける |
| IntegerScale | 0 | 1 = フルスクリーン時も整数倍のみ |
| Aspect | 0 | 表示の横縦比 (例 1.3333)。0 = ゲームの画素比のまま |
| HideMenuInFullscreen | 1 | フルスクリーン時にメニューバーを隠す |
| HideCursorInFullscreen | 1 | フルスクリーン時にマウスカーソルを隠す |
| IgnoreModeChange | 1 | ゲームの解像度変更を行わず擬似フルスクリーンで代替 |
| ModeChangeFullscreen | Fullscreen と同じ | ゲームが全画面に切り替えたとき、1 = 擬似フルスクリーン、0 = 窓のまま表示 |
| Resizable | 1 | 窓の大きさをマウスで変えられるようにする (最大化も可)。KeepAspect=1 なら縦横比を保つ |
| FramePopupWindow | 1 | 枠のない窓 (全画面専用ゲームの窓など) を窓表示するとき、タイトルバーと枠を付ける |
| DesktopBpp | 32 | DirectDraw に報告する画面の色数 (16 / 32) |
| Renderer | auto | auto = Direct3D9 (失敗時 GDI)、gdi = GDI のみ |
| VSync | 0 | 1 = Direct3D9 の表示を垂直同期に合わせる |
| DpiAware | 1 | 高 DPI 対応として動かす (ぼやけ防止) |
| VirtualizeWindow | 1 | 0 = 窓の仮想化を行わない (表示補正なし) |
| DialogDpiUnaware | 1 | ゲームのダイアログを DPI 非対応として作る (高 DPI 環境では Windows が拡大表示)。0 = ゲーム窓と同じ |

## [Timing]

| キー | 既定 | 内容 |
|---|---|---|
| GameHz / TargetHz | - | ゲーム本来の周期と合わせたい周期。両方指定すると時計の進みを TargetHz/GameHz 倍にする |
| TimeScale | 1 | 時計の倍率を直接指定 (GameHz/TargetHz が優先) |
| PreciseTimer | 1 | GetTickCount / timeGetTime を QueryPerformanceCounter ベースの 1ms 精度にする |
| RefreshRate | 0 | 画面転送の上限 (Hz)。0 = 制限しない。DirectDraw の垂直同期待ちの周期にも使う (0 なら 60) |
| TimerHz | 0 | timeSetEvent の周期タイマーをこの周期の高精度スレッドに置き換える |
| FrameWaitHz | TargetHz または 60.606 | `[Patch]` の framewait の周期 |

時計の補正は GetTickCount / timeGetTime / QueryPerformanceCounter に掛かります。
ゲームが音声バッファの再生位置で速度を決めている場合は効きません。

## [Input]

| キー | 既定 | 内容 |
|---|---|---|
| Enable | 1 | XInput コントローラを使う |
| Pad | 0 | コントローラ番号 (0～3) |
| Deadzone | 35 | アナログスティックの遊び (%) |
| StickDirections | 8 | スティックを 8 方向 / 4 方向に変換 |
| KeyboardExclusive | 0 | 0 = ゲームが DirectInput のキーボードを排他モードで使おうとしても非排他にする (Alt+Enter などを効かせるため) |
| HideJoysticks | 1 | ゲームから DirectInput / winmm のジョイスティックを隠す (二重入力防止) |
| PostKeyMessages | 2 | キー入力メッセージも送る。0 = 送らない、1 = すべて、2 = F1～F12 のみ |
| BackgroundInput | 0 | 1 = 窓が非アクティブでも入力を渡す |
| PauseKey | PAUSE | 一時停止のキー |
| PauseButton | (空) | 一時停止のパッドボタン (例 `Back+Start`、`Guide`) |
| ボタン名 | | `A=X` のように、パッドのボタンにキーを割り当てる。`X+Z` で同時押し |

ボタン名: `A B X Y LB RB LT RT Back Start LThumb RThumb Guide DPadUp DPadDown DPadLeft DPadRight
LStickUp LStickDown LStickLeft LStickRight RStickUp RStickDown RStickLeft RStickRight`

キー名: `A`～`Z` `0`～`9` `F1`～`F24` `UP DOWN LEFT RIGHT SPACE ENTER ESC TAB BACKSPACE SHIFT LSHIFT RSHIFT
CTRL LCTRL RCTRL ALT LALT RALT INSERT DELETE HOME END PGUP PGDN PAUSE NUM0`～`NUM9`
`MULTIPLY ADD SUBTRACT DECIMAL DIVIDE MINUS COMMA PERIOD SLASH SEMICOLON COLON AT LBRACKET RBRACKET CARET YEN BACKSLASH`、
または `0x41` のような仮想キーコード。

## [Registry]

| キー | 既定 | 内容 |
|---|---|---|
| Virtualize | 1 | `HKCU\Software\*` と `HKLM\Software\*` (Microsoft などを除く) を `mkwrap_reg.ini` に置き換える |
| `キー\値名=データ` | | 初期値。`%GAMEDIR%` はゲームフォルダ。`dword:10` で DWORD |

## [CD]

| キー | 既定 | 内容 |
|---|---|---|
| Emulate | 0 | 1 = 空いているドライブ文字を CD-ROM ドライブとして見せる |
| Path | (空) | CD の中身とみなすフォルダ (空欄 = ゲームフォルダ、相対パス可) |
| Letter | (空) | ドライブ文字 (空欄 = Z から逆順に空いている文字) |
| Label | (空) | ボリュームラベル |

## [Patch]

タイトル別の実行ファイル修正です。`名前=アドレス,種類,引数[,expect=元のバイト列]`。
`expect=` が一致しない場合は適用しません (別バージョンの実行ファイルを壊さないため)。

- `bytes,XX XX ...` … アドレスにバイト列を書き込む
- `framewait,再開アドレス` … アドレスに「MKWrap のフレーム待ち (FrameWaitHz) の呼び出し + 再開アドレスへのジャンプ」を書き込む

全タイトル共通の修正:

| キー | 既定 | 内容 |
|---|---|---|
| FixNullDefWindowProc | 1 | ゲームが窓ハンドル NULL で `DefWindowProcA` を呼んだとき、Windows 11 で落ちないように補正する (WM_CTLCOLOR* は対象のコントロールで既定処理、それ以外は 0 を返す) |

## [Debug]

| キー | 内容 |
|---|---|
| Log | 1 = `mkwrap.log` に記録。2 = さらに詳しく (GetProcAddress で解決した関数、メニュー構成、ファイルアクセスなど) |
| TestInput | 自動テスト用の入力。`F2@4-4.3,F3@6-6.3,LEFT@8-10` (キー@開始秒-終了秒) |
| MeasureIRQ | 1 = CPU エミュレータ DLL (Z80EMU / DLL6809) の割り込み要求回数を記録 (速度の調査用) |
| HangReport | 既定 1。ゲームの窓が数秒応答しなくなったら、全スレッドの実行位置と呼び出し履歴を `mkwrap_hang.log` に、ミニダンプを `mkwrap_hang.dmp` に書き出す (Log の設定と無関係) |
| CrashReport | 既定 1。例外で異常終了するとき、例外の種類と場所・全スレッドの呼び出し履歴を `mkwrap_crash.log` に、ミニダンプを `mkwrap_crash.dmp` に書き出す |

記録は `rundll32.exe` でこの DLL の `MkwDump` を実行し、ゲームのプロセスの外から行います
(固まりかけ・落ちかけのプロセスの中で全スレッドを止めて書き出すと、記録処理自体が固まることがあるため)。
