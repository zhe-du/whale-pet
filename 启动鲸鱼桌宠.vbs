' ============================================================
'  Whale Desktop Pet - silent launcher (no console window).
'  ASCII only: WSH reads .vbs as ANSI, non-ASCII text would be mangled.
' ============================================================
Option Explicit
Dim sh, fso, base
Set sh  = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
base = fso.GetParentFolderName(WScript.ScriptFullName)
If Not fso.FileExists(base & "\launcher.cmd") Then
  MsgBox "launcher.cmd not found next to this file:" & vbCrLf & base, 16, "Whale Desktop Pet"
  WScript.Quit 1
End If
sh.Run "cmd /c """ & base & "\launcher.cmd""", 0, False
