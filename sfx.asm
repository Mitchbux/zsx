
; DialogBox example

format PE GUI 4.0
entry start

include 'win32a.inc'

ID_TEST            = 313
MSZIP              = 2

section 'C0D3' code readable executable

  start:
        invoke  CreateThread, 0, 0, Decode, 0, 0, threadid
        mov     [hThread], eax
        invoke  GetModuleHandle,0
        invoke  DialogBoxParam,eax,37,HWND_DESKTOP,DialogProc,0
        invoke  TerminateThread,[hThread], 0
        invoke  ExitProcess,0
proc FIO E1, N2, ol
     mov eax, [N2]
     mov [bytes_read], eax
     ret
endp

proc Decode param
     push edi
     push edx

     invoke CreateDecompressor, MSZIP, 0, Compressor
     invoke GlobalAlloc, GPTR, 1024*1025
     mov edx, eax
     invoke CreateFile, filename, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL , 0
     mov [hFile], eax
     invoke WriteFile, [hFile], szDone, 16, bytes_read, 0
     invoke CloseHandle, [hFile]
     invoke GlobalFree, edx
     mov eax, [max]
     mov [counter], eax
     invoke MessageBox, 0, szDecompressed, szDone, MB_OK
     pop edx
     pop edi
     ret
endp

proc DialogProc hwnddlg,msg,wparam,lparam
        push    ebx esi edi
        cmp     [msg],WM_INITDIALOG
        je      .initdlg
        cmp     [msg],WM_COMMAND
        je      .wmcommand
        cmp     [msg],WM_CLOSE
        je      .wmclose
        cmp     [msg],WM_TIMER
        je      .wmtimer
        cmp     [msg],WM_PAINT
        je      .wmpaint
        xor     eax,eax
        jmp     .finish
  .initdlg:
        mov     eax, [hwnddlg]
        mov     [dlg],eax
        invoke  GetDlgItem, [hwnddlg], ID_TEST
        mov     [hwndgroup], eax
        invoke  SetTimer, [hwnddlg],101,100,0

       jmp .processed
  .wmtimer:
        invoke  GetDC, [hwndgroup]
        mov     [hdc], eax
        fild    [counter]
        fild    [max]
        fild    [size]
        fdiv    st0, st1
        fmul    st0, st2
        fistp   [bar+RECT.right]
        invoke  FillRect, [hdc], bar, COLOR_HIGHLIGHTTEXT
        mov     eax, [max]
        cmp     [counter], eax

        jb @F
        mov     edx, [hwnddlg]
        invoke  SetWindowText, edx, szDone
        @@:

        invoke  ReleaseDC, [hwndgroup], [hdc]
        jmp     .processed
  .wmpaint:
        invoke  BeginPaint, [hwndgroup], ps
        mov     [hdc], eax

        mov     [bar+RECT.top],14
        mov     [bar+RECT.bottom],16

        fild    [counter]
        fild    [max]
        fild    [size]
        fdiv    st0, st1
        fmul    st0,st2
        fistp   [bar+RECT.right]

        invoke  FillRect, [hdc], bar, COLOR_HIGHLIGHTTEXT
        invoke  EndPaint, [hwndgroup], ps

        jmp     .zero
  .wmcommand:
        cmp     [wparam],BN_CLICKED shl 16 + IDCANCEL
        jne      .processed
  .wmclose:
        invoke  KillTimer,[hwnddlg], 101
        invoke  EndDialog,[hwnddlg],0
        jmp     .processed
  .zero:
        mov     eax, 0
        jmp     .finish
  .processed:
        mov     eax,1
  .finish:
        pop     edi esi ebx
        ret
endp


section '1MPR' import data readable writeable

  library kernel32,'KERNEL32.DLL',\
          user32,'USER32.DLL',\
          cabinet, 'CABINET.DLL'

  include 'api\kernel32.inc'
  include 'api\user32.inc'
  include 'api\cabinet.inc'

section 'R5RC' resource data readable

  directory RT_DIALOG,dialogs

  resource dialogs,\
           37,LANG_ENGLISH+SUBLANG_DEFAULT,demonstration

  dialog demonstration,'Decompressing...',70,70,200,80,WS_CAPTION+WS_POPUP+WS_SYSMENU+DS_MODALFRAME
    dialogitem 'BUTTON','',ID_TEST,10,18,180,20,WS_VISIBLE+BS_GROUPBOX
    dialogitem 'BUTTON','Cancel',IDCANCEL,80,50,45,15,WS_VISIBLE+WS_TABSTOP+BS_PUSHBUTTON
  enddialog

section 'D4T4' readable writeable
  szDone db 'Decompression '
  szOk db 'OK', 0
  szDecompressed db 'Decompressed to file : '
  filename db 'test.zip', 0
  hFile dd 0
  ol OVERLAPPED
  bytes_read dd 0
  decompressed dd 0
  dlg dd 0
  hdc dd 0
  size dd 290
  hwndgroup dd 0
  ps PAINTSTRUCT
  bar RECT
  threadid dd 0
  hThread dd 0
  Compressor dd 0
  counter dd 0
  max dd 0x2DFF00
