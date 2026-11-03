/* タイトル別の初期設定 (実行ファイル名で判定) */
#ifndef MK_PROFILES_H
#define MK_PROFILES_H

typedef struct {
    const wchar_t *exe;        /* 実行ファイル名 (大文字小文字は区別しない) */
    const wchar_t *title;
    const wchar_t *timing;     /* [Timing] に追加する行 */
    const wchar_t *input;      /* [Input] のボタン割り当て */
    const wchar_t *patch;      /* [Patch] */
    const wchar_t *registry;   /* [Registry] */
    const wchar_t *note;       /* 先頭コメントに入れる注意書き */
    const wchar_t *extra;      /* そのほかのセクション ([CD] など) */
    const wchar_t *display;    /* [Display] に追加する行 */
} mk_profile_t;

#define DIR_KEYS(u, d, l, r) \
    L"DPadUp=" u L"\r\nDPadDown=" d L"\r\nDPadLeft=" l L"\r\nDPadRight=" r L"\r\n" \
    L"LStickUp=" u L"\r\nLStickDown=" d L"\r\nLStickLeft=" l L"\r\nLStickRight=" r L"\r\n"
#define STD_DIR DIR_KEYS(L"UP", L"DOWN", L"LEFT", L"RIGHT")
#define STD_SYS L"Back=F2\r\nStart=F3\r\nRB=F4\r\n"
#define D3D_DISPLAY \
    L"; Direct3D を使うゲームです。DirectX 6 世代 (IDirect3D3) は MKWrap が Direct3D 9 で描き、窓の拡大・擬似フルスクリーンが使えます。\r\n" \
    L"; それより古い/新しい Direct3D の場合は、自動で本物の DirectDraw に切り替えて起動し直します (窓の拡大は使えません)。\r\n" \
    L"; 表示がおかしい場合は DirectDraw=native にしてください\r\n" \
    L"ModeChangeFullscreen=1\r\n"
/* DirectX 6 の Direct3D (MKWrap が描く) */
#define D3D6_DISPLAY \
    L"; Direct3D (DirectX 6) の 3D 描画も MKWrap が Direct3D 9 で行います。窓の拡大・擬似フルスクリーン (Alt+Enter) が使えます。\r\n" \
    L"; 表示がおかしい場合は DirectDraw=native にすると本物の DirectDraw に任せます (窓の拡大は使えなくなります)\r\n" \
    L"; ゲームが全画面に切り替えても、Fullscreen の設定どおり (0 = 窓) に表示します\r\n" \
    L"ModeChangeFullscreen=0\r\n" \
    L"; ゲームは非アクティブになると止まる作りで、起動直後に止まったままになることがあるため、非アクティブを伝えない\r\n" \
    L"KeepActive=1\r\n"

/* セガゲーム本舗 (メディアカイト / 2004 年、Gens 系のメガドライブエミュレータ) */
#define SEGA_TIMING \
    L"; メガドライブ (NTSC) は 59.92Hz。ゲームは垂直同期待ちで速度を決めるので、その周期を合わせます\r\n" \
    L"RefreshRate=59.922\r\n"
#define SEGA_INPUT \
    STD_DIR \
    L"; キーボード (1P): A=J B=K C=L START=Enter / ESC=ポーズ TAB=リセット Shift+F5=ステート保存 Shift+F8=ステート読込\r\n" \
    L"; パッドはメガドライブの A/B/C の並びに合わせて X=A A=B B=C。Y も A ボタン\r\n" \
    L"X=J\r\nA=K\r\nB=L\r\nY=J\r\nStart=ENTER\r\nBack=ESC\r\n"
#define SEGA_NOTE \
    L"; セガゲーム本舗のタイトルです。CD から直接起動する形式のため、ゲームのフォルダをコピーして使います。\r\n" \
    L"; セーブとステート保存はマイドキュメント\\sega\\gamehompo に作られます。\r\n"
#define SEGA_DISPLAY \
    L"; 元は起動時に画面を 640x480・16 ビットカラーに切り替えます。ここでは枠付きの窓で表示します (Alt+Enter で全画面)\r\n" \
    L"ModeChangeFullscreen=0\r\n"

static const mk_profile_t g_profiles[] = {
    /* ---- 第1弾 ---- */
    { L"lr1500.exe", L"リブルラブル",
      L"; 元のフレーム同期処理は 1 フレームおきに待ちが抜ける作りのため、下の [Patch] で置き換えて 60.606Hz にしています\r\n"
      L"TargetHz=60.606\r\n",
      L"; 左レバー (リブル) = D/X/Z/C キー、右レバー (ラブル) = カーソルキー\r\n"
      DIR_KEYS(L"D", L"X", L"Z", L"C")
      L"RStickUp=UP\r\nRStickDown=DOWN\r\nRStickLeft=LEFT\r\nRStickRight=RIGHT\r\n"
      L"; 右スティックの代わりに ABXY ボタンでも右レバーを操作できます\r\n"
      L"Y=UP\r\nA=DOWN\r\nX=LEFT\r\nB=RIGHT\r\n" STD_SYS,
      L"FrameSync=4052e3,framewait,40531b,expect=FF 15 D8 01 44 00\r\n",
      L"", NULL, NULL },
    { L"db1500.exe", L"ドラゴンバスター",
      L"; 元は 16ms 間隔 (62.5Hz) で動くため、時計を遅らせてアーケードの 60.606Hz に合わせます\r\n"
      L"GameHz=62.5\r\nTargetHz=60.606\r\n",
      STD_DIR L"; A=剣(X) B=ファイア(Z) Y=ジャンプ(↑)\r\nA=X\r\nB=Z\r\nY=UP\r\n" STD_SYS,
      L"", L"", NULL, NULL },
    { L"kkkaikai.exe", L"奇々怪界",
      L"; 元は 17ms 間隔 (約58.8Hz) で動くため、時計を進めて 60Hz に合わせます\r\n"
      L"GameHz=58.824\r\nTargetHz=60\r\n",
      STD_DIR L"; A=御札(X) B=御祓い(Z) Y=水晶玉(X+Z)\r\nA=X\r\nB=Z\r\nY=X+Z\r\n" STD_SYS,
      L"",
      L"HKCU\\SOFTWARE\\TAITO\\KikiKaikai\\InstallDir=%GAMEDIR%\r\n"
      L"HKLM\\SOFTWARE\\TAITO\\KikiKaikai\\InstallDir=%GAMEDIR%\r\n", NULL, NULL },
    { L"digdug.exe", L"ディグダグ",
      L"; 元から 60.606Hz で動作しているため調整なし\r\n",
      STD_DIR L"; A/B=ポンプ(SPACE)\r\nA=SPACE\r\nB=SPACE\r\n" STD_SYS,
      L"", L"", NULL, NULL },
    { L"nrallyx.exe", L"ニューラリーX",
      L"; 元の速度のまま (調整なし)\r\n",
      STD_DIR L"; A/B=煙幕(SPACE)\r\nA=SPACE\r\nB=SPACE\r\n" STD_SYS,
      L"", L"", NULL, NULL },
    { L"splatter.exe", L"スプラッターハウス",
      L"; 元は 60Hz で動くため、時計を進めてアーケードの 60.606Hz に合わせます\r\n"
      L"GameHz=60\r\nTargetHz=60.606\r\n",
      STD_DIR L"; A=ジャンプ(X) B=アタック(Z)\r\nA=X\r\nB=Z\r\n" STD_SYS,
      L"",
      L"HKLM\\SOFTWARE\\MediaKite\\SplatterHouse\\InstallPath=%GAMEDIR%\r\n", NULL, NULL },
    /* ---- 第2弾 ---- */
    { L"pacman.exe", L"パックマン",
      L"; 元から 60.606Hz で動作しているため調整なし\r\n",
      STD_DIR STD_SYS,
      L"", L"", NULL, NULL },
    { L"yd1500.exe", L"妖怪道中記",
      L"; 元は 17ms 間隔 (約58.8Hz) で動くため、時計を進めてアーケードの 60.606Hz に合わせます\r\n"
      L"GameHz=58.824\r\nTargetHz=60.606\r\n",
      STD_DIR L"; A=ジャンプ(Z) B=攻撃(X)\r\nA=Z\r\nB=X\r\n" STD_SYS,
      L"", L"", NULL, NULL },
    { L"rainbow.exe", L"レインボーアイランド",
      L"; 元の速度のまま (調整なし)\r\n",
      L"; 左右移動はゲーム初期設定どおり Z/X キー\r\n"
      DIR_KEYS(L"", L"", L"Z", L"X")
      L"; A=ジャンプ(B) B=虹(V)\r\nA=B\r\nB=V\r\n" STD_SYS,
      L"", L"", NULL, NULL },
    { L"jajamaru.exe", L"忍者じゃじゃ丸くん",
      L"; 元の速度のまま (調整なし)\r\n",
      L"; 移動はゲーム初期設定どおり W/A/S/D キー\r\n"
      DIR_KEYS(L"W", L"S", L"A", L"D")
      L"; A=ジャンプ(K) B=手裏剣(J)\r\nA=K\r\nB=J\r\n" STD_SYS,
      L"", L"", NULL, NULL },
    { L"dodgeball.exe", L"熱血高校ドッジボール部",
      L"; 元の速度のまま (調整なし)\r\n",
      L"; ファミコン版と同じ操作。移動は W/A/S/D キー (プレイヤー1)\r\n"
      DIR_KEYS(L"W", L"S", L"A", L"D")
      L"; A=Aボタン(G) B=Bボタン(F) Start=スタート(ENTER) Back=セレクト(右SHIFT)\r\n"
      L"A=G\r\nB=F\r\nStart=ENTER\r\nBack=RSHIFT\r\n",
      L"", L"", NULL, NULL },
    { L"popnpop.exe", L"ぽっぷんぽっぷ",
      L"; 元は約62Hz (16ms 間隔) で動くため、時計を遅らせてアーケード (タイトー F3) の 58.97Hz に合わせます\r\n"
      L"GameHz=62\r\nTargetHz=58.97\r\n",
      STD_DIR L"; A=ショット(X) B=Cキー Start=スタート(Z)\r\nA=X\r\nB=C\r\nStart=Z\r\n",
      L"", L"", NULL,
      L"\r\n[CD]\r\n"
      L"; 起動時と BGM 再生時に CD-ROM を探すため、仮想 CD ドライブを作ってこのフォルダを見せます\r\n"
      L"Emulate=1\r\n"
      L"; CD の中身の場所 (空欄 = このフォルダ)。ドライブ文字は空欄なら空いている文字を自動で使います\r\n"
      L"Path=\r\nLetter=\r\nLabel=POPNPOP\r\n",
      L"; 元は全画面専用のため、枠付きの窓で表示します (Alt+Enter で全画面)\r\nModeChangeFullscreen=0\r\n" },
    { L"wd1500.exe", L"ワルキューレの伝説",
      L"; 元のフレーム同期処理はリブルラブルと同じく待ちが抜ける作りのため、下の [Patch] で置き換えて 60.606Hz にしています\r\n"
      L"TargetHz=60.606\r\n",
      L"; ゲームの初期設定 (1P): 移動=カーソルキー 攻撃=P ジャンプ=O クレジット=F2 スタート=F3\r\n"
      STD_DIR L"A=O\r\nB=P\r\nX=P\r\nY=O\r\n" STD_SYS,
      L"FrameSync=4155f3,framewait,415614,expect=FF 15 E0 E2 43 00\r\n",
      L"", NULL, NULL },
    { L"rockman.exe", L"ロックマン DASH 鋼の冒険心",
      L"; 速度はゲーム自身が調整するため補正なし\r\n",
      L"; ゲームはパッドを DirectInput で直接読みますが、ボタン番号が当時の PS 風パッド向けで、Xbox のパッドでは\r\n"
      L"; RB=R2 / Back=L1 / Start=R1 / 左スティック押し込み=START のようにずれ、十字キーも効きません。\r\n"
      L"; そのためパッドはゲームに見せず、ゲームの初期設定のキーに割り当てます (PS 版の説明書の配置を Xbox の配置に合わせたもの)。\r\n"
      L";   キーの初期設定: 移動=カーソル △=Z ×=X(Space) □=C ○=V L1=Delete L2=Insert R1=PageDown R2=PageUp START=Enter SELECT=右Shift\r\n"
      L";   十字キー/左スティック=移動  A=×(ジャンプ/キャンセル)  B=○(調べる/決定)  X=□(バスター)  Y=△(特殊武器)\r\n"
      L";   LB=L1 / RB=R1 (左右に向きを変える)  RT=R2 (ロックオン)  LT=L2 (未使用)  Start=START (メニュー)  Back=SELECT (マップ)\r\n"
      L"HideJoysticks=1\r\n"
      DIR_KEYS(L"UP", L"DOWN", L"LEFT", L"RIGHT")
      L"A=X\r\nB=V\r\nX=C\r\nY=Z\r\nLB=DELETE\r\nRB=PGDN\r\nRT=PGUP\r\nLT=INSERT\r\nStart=ENTER\r\nBack=RSHIFT\r\n",
      L"", L"",
      L"; オープニングムービー (CAPCOM.AVI) は Indeo 5 形式で、Windows 10/11 には再生用の部品がないため、MKWrap が自動で飛ばします (Movie=auto)。\r\n"
      L"; 映像の形式を変換した CAPCOM.AVI に差し替えた場合や、コーデックを入れた場合は再生されます (表示位置は窓の拡大に合わせて直します)\r\n",
      NULL, D3D6_DISPLAY },
    { L"bublbobl.exe", L"バブルボブル",
      L"; 元は 16ms 間隔 (62.5Hz) で動くため、時計を遅らせてアーケード (タイトー) の 59.185Hz に合わせます\r\n"
      L"GameHz=62.5\r\nTargetHz=59.185\r\n",
      L"; ゲームの初期設定 (1P): 左=X 右=C ジャンプ=Z あわ=A クレジット=F2 スタート=F3 (2P スタート=HOME)\r\n"
      DIR_KEYS(L"", L"", L"X", L"C")
      L"A=Z\r\nB=A\r\nX=A\r\nY=Z\r\nBack=F2\r\nStart=F3\r\nRB=HOME\r\n",
      L"", L"", NULL, NULL },
    { L"ppjdd.exe", L"バーチャコップ (Windows 版)",
      L"; 速度はゲーム自身が timeGetTime で調整するため補正なし\r\n",
      L"; 操作方法はゲームの F5 (Device Settings) で選びます。初期設定はマウス (照準=マウス、撃つ=左ボタン、リロード=右ボタンを素早く 2 回)。\r\n"
      L"; 窓表示でもマウスは窓の中で使えます (ゲームが前面にある間はカーソルが窓の中に留まります。Alt+Tab で他の窓へ)。\r\n"
      L"; パッドは 2 通りで使えます:\r\n"
      L";   (1) F5 で GamePad を選ぶ → ゲームがパッドを直接読みます (方向=照準、ボタン 1=撃つ、ボタン 2 を素早く 2 回=リロード)\r\n"
      L";   (2) F5 で KeyBoard を選ぶ → 下の割り当てでキーボード (1P) として操作します\r\n"
      L";       左スティック/十字キー=照準 (WASD)  A/RT=撃つ (V)  B=リロード (H、素早く 2 回)  X/RB=照準の加速 (G)  Start=スタート (Space)  Back=ポーズ (F3)\r\n"
      L"; マウスの速さ: ゲームは移動量を横 1/3・縦 1/2.25 にして照準を動かすため、元のままだと画面の端から端まで\r\n"
      L"; マウスを画面幅の 3 倍動かす必要があります。MouseGameDivX/Y でこれを打ち消し、MouseSpeed=1 で照準が窓の上のマウスの動きと\r\n"
      L"; ほぼ同じだけ動くようにしています。速すぎ/遅すぎるときは MouseSpeed を変えてください (2 = 2 倍速、0.5 = 半分)。\r\n"
      L"; 元の速さに戻すには MouseGameDivX=0 / MouseGameDivY=0 / MouseSpeed=1 にします\r\n"
      L"MouseSpeed=1\r\nMouseGameDivX=3\r\nMouseGameDivY=2.25\r\n"
      L"HideJoysticks=0\r\n"
      DIR_KEYS(L"W", L"S", L"A", L"D")
      L"A=V\r\nRT=V\r\nB=H\r\nX=G\r\nRB=G\r\nStart=SPACE\r\nBack=F3\r\n",
      L"", L"",
      L"; CD を入れていないと起動しないため、ゲームのフォルダを仮想 CD ドライブとして見せます ([CD])。\r\n"
      L"; BGM は CD の音楽トラック (CD-DA) です。取り込んだ音楽ファイル (Track02.wav / Track02.mp3 … Track19) を\r\n"
      L"; ゲームのフォルダの CDDA フォルダに置くと再生します。無い場合は BGM なしで進みます。\r\n"
      L"; オープニングムービー (OPEN320.AVI) は Indeo 4 形式です。CD の INDEO フォルダのコーデック (IR41_32.DLL) を\r\n"
      L"; ゲームのフォルダにコピーしてあり、MKWrap がゲームの中だけで使えるようにして再生します (Windows には登録しません)。\r\n"
      L"; 通信対戦用の DPLAY.dll は、DirectPlay が無効な Windows 10/11 でも起動できるようにする代わりの部品です (通信対戦は DirectPlay を有効にした場合のみ)。\r\n",
      L"\r\n[CD]\r\n"
      L"; 仮想 CD ドライブ (中身はゲームのフォルダ)\r\n"
      L"Emulate=1\r\nLabel=VCOP\r\n"
      L"; CD の音楽トラック: トラック数と各トラックの長さ (音楽ファイルが無い場合はこの長さで再生したことにします)\r\n"
      L"Tracks=19\r\n"
      L"TrackLengths=16:04,0:13,2:23,0:57,1:11,4:15,3:42,3:35,1:43,2:43,2:24,2:21,2:48,0:11,0:13,1:21,3:56,4:11,3:00\r\n"
      L"; 取り込んだ音楽ファイルのフォルダと音量 (0-100)\r\n"
      L"AudioPath=CDDA\r\nVolume=100\r\n",
      L"; 元は画面を 640x480 に切り替えて全画面で遊びます。ここでは枠付きの窓で表示します (Alt+Enter で擬似フルスクリーン)\r\n"
      L"ModeChangeFullscreen=0\r\n" },
    /* ---- 第3弾: セガゲーム本舗 ---- */
    { L"gunstar.exe", L"ガンスターヒーローズ (セガゲーム本舗)", SEGA_TIMING, SEGA_INPUT, L"", L"", SEGA_NOTE, NULL, SEGA_DISPLAY },
};

static const mk_profile_t g_sega_profile = {
    L"", L"セガゲーム本舗 (メガドライブ)", SEGA_TIMING, SEGA_INPUT, L"", L"", SEGA_NOTE, NULL, SEGA_DISPLAY
};

/* Direct3D を使う未登録のゲーム: DirectX 6 なら MKWrap が描き、それ以外は実行時に本物の DirectDraw に切り替える */
static const mk_profile_t g_d3d_profile = {
    L"", L"(未登録のタイトル / Direct3D)",
    L"; 速度がおかしい場合は GameHz / TargetHz を設定してください\r\n",
    L"; 未登録のタイトルです。ゲーム中に Ctrl+F12 (またはパッドの LB+RB+Back 長押し) で割り当てを覚えさせてください\r\n"
    L"LearnHint=1\r\n"
    STD_DIR L"A=Z\r\nB=X\r\nX=C\r\nY=V\r\n" STD_SYS,
    L"", L"",
    L"; Direct3D を使うタイトルです。MKWrap は表示を置き換えず、入力・速度・レジストリなどを補正します。\r\n",
    NULL, D3D_DISPLAY
};

static const mk_profile_t g_generic_profile = {
    L"", L"(未登録のタイトル)",
    L"; 速度がおかしい場合は GameHz / TargetHz を設定してください\r\n",
    L"; 未登録のタイトルです。ゲーム中に Ctrl+F12 (またはパッドの LB+RB+Back 長押し) で割り当てを覚えさせてください\r\n"
    L"LearnHint=1\r\n"
    STD_DIR L"A=Z\r\nB=X\r\nX=C\r\nY=V\r\n" STD_SYS,
    L"", L"",
    L"; このタイトル用の設定は未登録です。ゲーム中に Ctrl+F12 (またはパッドの LB+RB+Back を 1.5 秒押す) で\r\n"
    L"; パッドのボタン → キーの順に押すと割り当てを覚えて、この INI に書き込みます。\r\n",
    L"\r\n[CD]\r\n"
    L"; 1 = 仮想 CD ドライブを作り、このフォルダを CD-ROM として見せる (CD を要求されるとき)\r\n"
    L"Emulate=0\r\nPath=\r\nLetter=\r\nLabel=\r\n"
};

#endif
