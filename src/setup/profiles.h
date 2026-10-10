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
};

static const mk_profile_t g_generic_profile = {
    L"", L"(未登録のタイトル)",
    L"; 速度がおかしい場合は GameHz / TargetHz を設定してください\r\n",
    STD_DIR L"A=Z\r\nB=X\r\nX=C\r\nY=V\r\n" STD_SYS,
    L"", L"",
    L"; このタイトル用の設定は未登録です。ボタン割り当てはゲームの「操作の設定」に合わせて変更してください。\r\n",
    L"\r\n[CD]\r\n"
    L"; 1 = 仮想 CD ドライブを作り、このフォルダを CD-ROM として見せる (CD を要求されるとき)\r\n"
    L"Emulate=0\r\nPath=\r\nLetter=\r\nLabel=\r\n"
};

#endif
