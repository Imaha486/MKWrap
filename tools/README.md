# tools

開発・動作確認用の小物です (配布物には含めません)。

## sendcmd.c

Wine などで、起動中のゲーム窓 (メニュー付きの最初の可視窓) へメッセージを送ります。

```
i686-w64-mingw32-gcc -O2 -o sendcmd.exe sendcmd.c
sendcmd.exe * cmd 32772     … WM_COMMAND (メニュー項目) を送る
sendcmd.exe * key 0x7A      … WM_KEYDOWN/UP (例 F11)
sendcmd.exe * sys 0x0D      … Alt+キー (例 Alt+Enter)
```

## Wine での確認例

```
export WINEPREFIX=~/.wine-mk WINEARCH=win32
wine build/MKSetup.exe 'Z:\path\to\cd' 'C:\Games\lr'
cd ~/.wine-mk/drive_c/Games/lr && WINEDLLOVERRIDES="dinput=n,b" wine LR1500.EXE
```

`mkwrap.ini` の `[Debug] TestInput=F2@4-4.3,F3@6-6.3` で、パッド無しでも入力を試せます。
