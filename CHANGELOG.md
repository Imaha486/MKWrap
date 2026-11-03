# 変更履歴

## 1.7.3
- マウスの速さの補正 (`[Input] MouseSpeed` / `MouseSpeedX` / `MouseSpeedY` / `MouseGameDivX` / `MouseGameDivY`)。
  ゲームがカーソルを画面中央に戻して移動量で照準を動かす作り (バーチャコップなど) のとき、移動量を倍率どおりにしてゲームに渡す。
  ゲームが移動量を割って切り捨てる値 (`MouseGameDiv`) を指定すると、今のマウスの細かい移動 (1～2 画素ずつ) が切り捨てで消えないようにし、端数も次回へ持ち越す
- バーチャコップ: 初期設定を `MouseSpeed=1` `MouseGameDivX=3` `MouseGameDivY=2.25` にして、照準が窓の上のマウスの動きとほぼ同じだけ動くようにした
  (元は横 1/3・縦 1/2.25 で、画面の端から端まで照準を動かすのにマウスを画面幅の 3 倍動かす必要があった)
- カーソルを中央に戻す作りのゲームでは、ゲームが前面に無いときのマウスの移動をゲームに渡さない (照準が飛ぶのを防ぐ)

## 1.7.2
- 起動時の作業フォルダがゲームのフォルダでない場合 (ショートカットやランチャー、ファイル管理ソフトから起動した場合など)、ゲームのフォルダに変更する。
  バーチャコップが `MENUS.TXT` を開けず「File Error. / Data Load Error.」で止まっていた。`[Debug] KeepWorkDir=1` で従来どおり
- 相対パスのファイルが見つからないとき、フォルダ無しのファイル名 (`MENUS.TXT` など) もゲームのフォルダ直下を探す

## 1.7.1
- MKSetup: セガ PC のインストーラ (SSP.INI) で、`DocPath` の説明書 (バーチャコップは `NATIVE\DOC` の `VCOP.HLP` / `VCOP.CNT`) もコピーする

## 1.7.0
- バーチャコップ (Windows 版、PPJDD.EXE) に対応。
- MKSetup: セガ PC のインストーラ (CD の `SSP.INI`) に対応。`SourcePath1` / `SourcePath2` のフォルダ (`NATIVE\VCOP` と `COMMON`) を
  1 つのフォルダにまとめてコピーし、CD の `INDEO` フォルダの動画コーデック (`IR41_32.DLL`) も一緒にコピーする
- MKSetup: DirectPlay (`DPLAY.dll`) を読み込むゲームには、代わりの `DPLAY.dll` をゲームのフォルダに置く。
  Windows 10/11 は DirectPlay が既定で無効で、通信対戦用の部品 (DPCTRL.DLL) が DPLAY.dll を読み込むだけでゲームが起動できなかった。
  代わりの DPLAY.dll は、システムに本物があればそれに処理を渡し、無ければ「DirectPlay は使えない」と返す (通信対戦以外は普通に遊べる)
- CD-DA (CD の音楽トラック) のエミュレーション。仮想 CD ([CD] Emulate=1) のとき、ゲームが `mciSendCommand` で開く `cdaudio` を MKWrap が受け持つ。
  取り込んだ音楽ファイル (`CDDA\Track02.wav` / `.mp3` / `.wma` など) があれば再生し、無ければ曲の長さだけ「再生中」を演じて終了を通知する。
  `[CD] Tracks` `TrackLengths` `AudioPath` `Volume`
- 仮想 CD: `_lopen` も CD の中として扱う。CD 上のパス (`Y:\NATIVE\VCOP\PPJDD.EXE` など) のファイルがフォルダに無い場合は、
  フォルダ直下の同じ名前のファイルを開く (インストール時にフォルダ構造を平らにした場合)。MCI で開く動画のパスも置き換える
- 動画: `mciSendCommand` で開く AVI にも対応 (これまでは `mciSendString` のみ)。映像を再生できない形式のときは、
  ゲームのフォルダにあるコーデック (`IR41_32.DLL` / `IR50_32.DLL` / `IR32_32.DLL`、または `INDEO` フォルダ内) を
  このゲームの中だけで使えるように登録してから再生する (Windows にはインストールしない)。それでも再生できなければ従来どおり飛ばす
- 動画をゲームの窓の中に表示するとき、窓の拡大に合わせて縦横比を保ったまま拡大する。動画の再生中は「画面の更新が止まった」の調査記録を取らない
- マウス: 窓を拡大しているとき、マウスのメッセージ (WM_MOUSEMOVE / ボタン) の座標をゲームから見た座標 (論理サイズ) に直す。
  全画面前提のゲームが `SetCursorPos` でカーソルを画面中央に戻す (相対移動で照準を動かす) 場合は、窓の中の位置に直して動かす。
  ゲームが前面に無いときはカーソルを動かさない

## 1.6.10
- ロックマン DASH: パッドの割り当てを PS 版の説明書の配置に合わせた Xbox の配置に変更。
  ゲームは DirectInput のボタン番号を当時の PS 風パッド向けに決め打ちしており (0=× 1=○ 2=□ 3=△ 5=R2 6=L1 7=R1 8=START 9=SELECT、
  十字キー (POV) は読まない)、Xbox のパッドでは RB=R2・Back=L1・Start=R1 のようにずれていたため、パッドはゲームに見せず (HideJoysticks=1)、
  ゲームの初期設定のキー (△=Z ×=X □=C ○=V L1=Delete L2=Insert R1=PageDown R2=PageUp START=Enter SELECT=右Shift) に割り当てる。
  A=× B=○ X=□ Y=△ LB=L1 RB=R1 RT=R2 LT=L2 Start=START Back=SELECT、十字キー/左スティック=移動

## 1.6.9
- ゲームの `MsgWaitForMultipleObjects` で、待ち対象 (イベント) を窓のメッセージより先に調べるようにした。
  Windows では窓にメッセージ (タイマー・再描画など) が来ていると待ち対象より先に「入力あり」が返るため、
  ロックマン DASH が DirectSound の通知 (ストリーミング再生の次の部分の読み込み) を受け取れず、
  ナレーションや BGM の最初の数秒 (バッファ 128KB 分) が繰り返し流れていた
- ゲームが前面に無いときは、キーボード・ゲームパッドの入力をゲームに渡さないようにした (`[Input] BackgroundInput=0`、既定)。
  DirectInput のキーボード (GetDeviceState / GetDeviceData)、ジョイスティック (中央・何も押していない状態を返す)、GetAsyncKeyState が対象。
  ゲームがバックグラウンドでも入力を読む設定にしている場合も含む。1 で従来どおり
- `Log=1` で MsgWaitForMultipleObjects の結果の内訳を記録

## 1.6.8
- `GetTickCount` / `timeGetTime` が返す時刻を、既定で「Windows の起動から 10 分後」の値から始めるようにした (`[Timing] TickBase`、既定 600000。
  `uptime` で従来どおり実際の起動からの時間)。
  ロックマン DASH は次のフレームの時刻を単精度浮動小数点 (float) で持っており、Windows を起動してから長時間 (数日) たっていると
  時刻の精度が 1 フレーム (33ms) より粗くなって、オープニングで画面が一度も更新されないまま止まっていた (ナレーションだけ流れる)。
  Windows 9x の頃は毎日再起動するのが普通だったため表に出なかった問題で、Wine で TickBase を大きくすると同じ症状を再現できた。
  49.7 日で時刻が一周する問題も避けられる

## 1.6.7
- 調査用: 画面の更新が止まったとき (Log=1)、ゲームの変数の領域を mkwrap_hang_data.bin に書き出す (ミニダンプに入らないため)。
  `Log=2` では、ゲームが開いたファイルをすべて記録する

## 1.6.6
- Direct3D (DirectX 6) の描画用デバイスを、見えない窓ではなくゲームの窓に結び付けるようにした
  (Windows 11 でゲームの窓の左上に 16x16 の白い四角が出ていた件への対処)
- `KeepActive=1` のとき、窓は応答しているのに画面の更新が 3 秒止まったら、ゲームに `WM_ACTIVATEAPP` (アクティブ) を伝え直す

## 1.6.5
- MKSetup: インストーラで展開した後、CD の直下にあるフォルダのうちゲームの実行ファイルが `フォルダ名\` で参照しているもの
  (ロックマン DASH の `Dat` `Sph` `BGM` など) を、展開先にも同じ構造で用意するようにした。
  インストーラがフォルダ無しで展開していた同じ名前・大きさのファイルは移動し、無いものは CD からコピーする
- MKWrap: `DAT\INIT_DAT.BIN` のようなフォルダ付きの相対パスが見つからない場合、ゲームのフォルダ直下に同じ名前のファイルがあればそれを開く
  (フォルダ無しで展開済みの環境の救済)
- ロックマン DASH が黒い画面のまま止まる原因は、データがフォルダ無しで展開されていたことだった
  (1.6.4 の作業フォルダの見張りは保険として残す)

## 1.6.4
- 作業フォルダの見張りを追加。ゲーム以外 (ドライバなど) が途中で作業フォルダを書き換えると、相対パスでデータを開くゲームが
  「パスが見つからない」まま止まるため、ゲームが相対パスのファイルを開く前と、DirectInput / DirectSound / Direct3D の初期化の後に、
  起動時の作業フォルダへ戻す (戻したときは何が原因だったかを mkwrap.log に記録)。
  ロックマン DASH が Windows 11 で `DAT\INIT_DAT.BIN` を開けず (エラー 3 = パスが見つからない) 黒い画面のまま止まる問題への対処

## 1.6.3
- 調査用: `Log=1` のとき、ゲームの `CreateFileA` のうち失敗したもの・100ms 以上かかったものを記録し、
  画面の更新が止まったときに直近のファイルを開く処理の回数・失敗数・戻ってきていないファイルを記録する
  (ロックマン DASH が Windows 11 で `DAT\INIT_DAT.BIN` を開く処理の中で止まっている件の調査用)

## 1.6.2
- ゲームの動画 (`mciSendString` の avivideo) で、映像の形式を伸長できる部品がこの PC に無い場合 (Indeo 5 など) は、
  再生せずに「再生が終わった」ことをゲームへ通知して飛ばすようにした (`[Display] Movie=auto`、play / skip も指定可)。
  ロックマン DASH の CAPCOM.AVI で「圧縮解除プログラム 'vids:iv50' が見つからない」と表示される問題への対処
- DirectInput のジョイスティック: 取り直しても「入力が失われた」(DIERR_INPUTLOST) のままの場合は「取得していない」と返す
  (失われる → Acquire を繰り返して固まるゲームがあるため)。ジョイスティックの協調レベルは非排他・バックグラウンドにする
  (`[Input] JoystickExclusive=1` で元のまま)。ジョイスティックの作成・取得の失敗を記録
- `Log=1` のとき、窓は応答しているのに画面の更新が 8 秒止まったら、全スレッドの状態を mkwrap_hang.log / .dmp に 1 回記録する

## 1.6.1
- `[Display] KeepActive` を追加 (既定 0)。1 にすると、他のアプリに切り替わってもゲームに `WM_ACTIVATEAPP` (非アクティブ) を伝えない。
  ロックマン DASH は非アクティブの通知を受けると描画を止めて待つ作りで、Windows 11 では起動直後に
  (黒い画面や左上の白い四角のまま) 止まってしまうことがあったため、タイトル別設定で KeepActive=1 にした
- `Log=1` で `WM_ACTIVATEAPP` を記録
- Direct3D 用の隠し窓をアクティブにならない窓 (WS_EX_NOACTIVATE) にした

## 1.6
- Direct3D (DirectX 6 世代: IDirect3D3 / IDirect3DDevice3) を MKWrap の DirectDraw エミュレーションで描画するようにした。
  3D の描画は Direct3D 9 で行い、Flip や Lock / Blt / GetDC の前に描画結果をサーフェス (DIB) へ読み戻すので、
  窓の拡大・擬似フルスクリーン・リサイズなど 2D のゲームと同じ表示機能がそのまま使える。dgVoodoo2 などの ddraw.dll は不要。
  - 対応: EnumDevices / CreateDevice / CreateViewport / CreateMaterial / CreateLight / CreateVertexBuffer / EnumZBufferFormats、
    DrawPrimitive / DrawIndexedPrimitive (Strided / VB / Begin～End も)、レンダーステート (DirectX 6 の旧ステートは Direct3D 9 のものへ変換)、
    テクスチャステージ、変換行列、材質とライト、ビューポートの Clear、テクスチャ (16/32 ビット、パレット、カラーキー透過)、Z バッファ
  - 画面のロックなどで Direct3D 9 のデバイスが失われた場合は作り直してステートを設定し直す
  - DirectX 6 以外の Direct3D (IDirect3D / IDirect3D2 / IDirect3D7) を求められた場合は、これまでどおり auto で本物の DirectDraw に切り替える
  - DirectDraw の性能情報に DDCAPS_3D、Z バッファ・テクスチャ・3D デバイスのサーフェスを追加 (これを見て 3D を使うか決めるゲームがある)
- ロックマン DASH: 上記により `DirectDraw=auto` (MKWrap が描画) に変更。窓表示 (拡大・マウスでのリサイズ)、Alt+Enter の擬似フルスクリーンに対応。
  ゲームが全画面に切り替えても `Fullscreen` の設定どおりに表示する (`ModeChangeFullscreen=0`)
- MCI の動画 (`mciSendString` の `window ... handle` + `put ... destination`) を、拡大した窓の実際の表示位置に合わせるようにした。
  動画の再生中は MKWrap の画面表示で上書きしない
- DirectDraw の排他モードで画面モードを設定したとき、窓の論理サイズだけでなく実際の窓の大きさも画面モードに合わせるようにした
  (ロックマン DASH でムービーの後の窓が画面いっぱいの大きさのままになっていた)

## 1.5.1
- DirectDraw=native のとき、本物の DirectDraw の SetCooperativeLevel / SetDisplayMode の結果と窓の状態を mkwrap.log に記録 (Log=1)。
  全画面の設定に失敗したら、窓を前面に出して再試行し、それでも失敗すれば付加フラグ (FPUSETUP など) を外して再試行する
  (ロックマン DASH で「SetCooperativeLevel to fullscreen Error!」が出る件の調査用)

## 1.5
- Direct3D (DirectX 3～7) を使うゲームへの対応: `[Display] DirectDraw` (auto / emulate / native)。
  MKWrap の DirectDraw は 2D 専用のため、native では DirectDraw を置き換えず (本物、または同じフォルダの dgVoodoo2 などの ddraw.dll に任せ)、
  窓の仮想化もしない。入力・速度・レジストリ・異常終了の記録などはそのまま使える。DPI 対応も既定で切る (窓が小さくならないように)。
  auto (既定) では、ゲームが Direct3D を求めた時点で INI を native に書き換えて自動的に起動し直す。
  MKWrap が ddraw.dll として置かれている場合も、native では本物の ddraw.dll へ転送する
- 対応タイトル追加: ロックマン DASH 鋼の冒険心 (rockman.exe、Direct3D)。DirectDraw=native、パッドはゲームに直接見せる
- MKSetup: 実行ファイルに "Direct3D" の文字列があるゲームは Direct3D 用の設定で INI を作る。
  タイトル別設定にある項目は、既定の項目と二重に書かないようにした

## 1.4.2
- ワルキューレの伝説でキャラクタ (スプライト) の色が化ける問題を修正。
  ゲームが確保したメモリ (ヒープ) を初期化せずに色の変換結果の判定に使っており、Windows 9x では新しいメモリがほぼ 0 だったため
  問題にならなかったが、今の Windows / Wine では前の内容が残っていて誤った色の表が使われていた。
  ゲームの `HeapAlloc` / `HeapReAlloc` に 0 で埋める指定を付けるようにした (`[Patch] ZeroHeap`、既定 1、全タイトル共通)
- `Log=1` の CreateSurface の記録にピクセル形式 (マスク) を追加

## 1.4.1
- 対応タイトル追加: ワルキューレの伝説 (WD1500.EXE)。リブルラブルと同じエンジンで、フレーム同期処理を `[Patch]` で置き換え 60.606Hz に。
  操作はゲームの初期設定 (移動=カーソルキー 攻撃=P ジャンプ=O) に合わせた
- 画面が化ける問題への対策 (実機 Windows でのみ起きる現象への対処):
  - Windows は GDI の描画をスレッドごとに溜めて後でまとめて実行する (Wine はすぐ実行する)。
    ゲームがサーフェスの DC に GDI で描いた直後に Blt / Lock すると、描きかけの画素を読むことがあるため、
    サーフェスを扱うスレッドでは GDI のバッチを止め、Blt / BltFast / Flip / Lock / GetDC と GDI 転送の前後で溜まった描画を実行する
  - 描画スレッドがキャンバス (プライマリ) へ書き込んでいる最中に、窓のスレッドが表示 (読み出し) しないように排他した
  - キャンバスの DC への GDI 転送も、表示処理と同時に行わないようにした (DC は複数スレッドから同時に使えない)
- `Log=2` で、使われた Blt の種類 (フラグ・拡大縮小・反転) を一度ずつ記録

## 1.4
- 対応タイトル追加: バブルボブル (InstallShield 3)。操作はゲームの初期設定 (左=X 右=C ジャンプ=Z あわ=A) に合わせた。
  速度は 16ms 間隔 (62.5Hz) → アーケードの 59.185Hz
- パッドの割り当ての学習機能: ゲーム中に Ctrl+F12 (またはパッドの LB+RB+Back を 1.5 秒押す) で学習モード。
  パッドのボタン → キーボードのキーの順に押すと、その場で割り当てを変えて mkwrap.ini に書き込む。
  十字キーと左スティックは同じ方向をまとめて割り当てる。学習中はゲームを一時停止し、案内を画面に重ねて表示する。
  未登録のタイトルでは起動時に案内を表示 (`[Input] LearnHint=1`)
- 調査用 `[Debug] TestButtons` (パッドのボタンを押したことにする)

## 1.3.1
- ガンスターヒーローズで Alt+Enter の擬似フルスクリーンが効かない問題を修正。
  ゲームのアクセラレータ表に Alt+Enter (ゲーム自身の全画面切り替え、コマンド 40051) があり、
  キー入力が `TranslateAccelerator` で WM_COMMAND に変換されて MKWrap に届いていなかった。
  `TranslateAcceleratorA/W` をフックし、擬似フルスクリーン切り替えのキーは変換させないようにした (全タイトル共通)
- DirectInput のキーボードを排他モードで使うゲームは非排他にする (排他モードでは窓にキーのメッセージが届かず、
  Alt+Enter や一時停止キーが効かないため。`[Input] KeyboardExclusive=1` で元のまま)。Alt+Enter の Enter はゲームに渡さない

## 1.3
- 対応タイトル追加: ガンスターヒーローズ (セガゲーム本舗、Gens 系メガドライブエミュレータ)。
  他のセガゲーム本舗のタイトルも、実行ファイル内の窓クラス名 "SEGA MEGADRIVE" で判定して同じ初期設定を使う
- MKSetup: インストーラの無い CD (autorun.inf があるか CD-ROM ドライブ) では、ゲームの実行ファイルがあるフォルダを展開先へコピーする
- DirectDraw: `SetSurfaceDesc` (アプリのメモリをサーフェスの実体にする) に対応。ピッチと形式が食い違う場合はピッチから形式を推定
- DirectDraw: `ChangeDisplaySettings` で色数を変えたつもりのゲームには、その色数をプライマリの形式として報告する
- `SetMenu` でメニューを付け外ししたとき、本物の Windows と同じく窓の外寸を保ってクライアント領域を変える (ゲームから見えるサイズの補正)

## 1.2.3
- 窓の大きさをマウスで変えられるようにした (`[Display] Resizable`、既定 1)。最大化もできる。
  `KeepAspect=1` なら縦横比を保ったまま変わる。変えた大きさは、ゲームが窓の大きさを変えたときも保つ。ゲームには元の窓スタイルを見せる
- `ModeChangeFullscreen` が INI に無い場合は `Fullscreen` の設定に従うようにした。
  1.1 以前の MKSetup で作った mkwrap.ini (この項目が無い) を使うと、ぽっぷんぽっぷが `Fullscreen=0` でも全画面で起動していた

## 1.2.2
- リブルラブル / ドラゴンバスター / 妖怪道中記の「ゲームの設定」「操作の設定」で落ちる問題を修正。
  ダイアログの WM_CTLCOLOR の既定処理が `DefWindowProcA(NULL, ...)` を呼んでおり、Windows 11 の USER32 はここで
  NULL を参照して落ちる (以前の Windows では何もせず 0 を返していた)。ラッパーで窓ハンドル NULL の呼び出しを補正する
  (`[Patch] FixNullDefWindowProc`、既定 1)。1.2.1 の異常終了の記録 (mkwrap_crash.log / .dmp) で特定

## 1.2.1
- 描画スレッドからの表示依頼の間隔を空けるようにした。依頼が途切れなく届くと、窓のスレッドで
  ダイアログの描画や入力が後回しにされ続け、ダイアログが真っ白のまま操作できなくなるため
  (リブルラブル / ドラゴンバスター / 妖怪道中記の設定ダイアログ)。モーダルダイアログ表示中はゲーム画面の更新を 30 回/秒に抑える
- ゲームのダイアログを DPI 非対応として作るようにした (`[Display] DialogDpiUnaware`、既定 1)。高 DPI 環境では Windows が拡大表示する
- 異常終了の自動記録 (`mkwrap_crash.log` / `mkwrap_crash.dmp`、`[Debug] CrashReport`)
- 応答なしの記録を別プロセスで行うようにした (`mkwrap_hang.log` / `mkwrap_hang.dmp`)。
  1.2 では記録処理がゲームのスレッドを止めたまま書き出していたため、記録処理自体が固まる・落ちる恐れがあった
- `Log=1` で、ExitProcess / TerminateProcess の呼び出し元、ダイアログ API の出入り、表示回数と窓のスレッドの表示処理の割合を記録

## 1.2
- 窓への表示を窓のスレッドだけで行うように変更 (リブルラブル / ドラゴンバスター / 妖怪道中記で設定ダイアログが固まる問題への対策)
- 応答なしの自動記録 (`mkwrap_hang.log`、`[Debug] HangReport`)
- 全画面専用ゲームの枠なし窓に、窓表示時にタイトルバーと枠を付ける (`FramePopupWindow`)
- ゲームの全画面切り替えを窓のまま表示する設定 (`ModeChangeFullscreen`)。ぽっぷんぽっぷは既定で枠付きの窓
- タイトル別設定に [Display] の項目を持てるようにした

## 1.1
- 対応タイトル追加: パックマン、妖怪道中記、レインボーアイランド、忍者じゃじゃ丸くん、熱血高校ドッジボール部、ぽっぷんぽっぷ
- 圧縮 (UPX 系) 実行ファイルに対応 (GetProcAddress 経由で解決される関数にもフック)
- DirectInput を使わないタイトル向けに ddraw.dll としても配置できるようにした
- 仮想 CD-ROM ドライブ (`[CD]`)
- winmm のジョイスティック API も HideJoysticks の対象に
- DirectDraw の転送・形式変換を高速化 (変換表と座標表)
- MKSetup: ヘッダ一体型の InstallShield 5 キャビネット、CD 上の非圧縮ファイルの回収に対応
- MKSetup: 無効なキャビネット項目で停止する問題を修正
- 調査用機能: `Log=2`、メッセージボックス文面の記録

## 1.0
- 最初の版。リブルラブル、ドラゴンバスター、奇々怪界、ディグダグ、ニューラリーX、スプラッターハウスに対応
- 工程1 (InstallShield 3 / 5・6 / Windows Installer の展開)、工程2 (ラッパーと INI の配置)
- 拡大表示、擬似フルスクリーン、Direct3D9 表示、DirectDraw エミュレーション、XInput、速度補正、一時停止、レジストリの INI 化
