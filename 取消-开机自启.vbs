' Remove the autostart entry.
Option Explicit
Dim sh
Set sh = CreateObject("WScript.Shell")
On Error Resume Next
sh.RegDelete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WhaleDesktopPet"
If Err.Number <> 0 Then
  MsgBox "Autostart entry was not present.", 64, "Whale Desktop Pet"
Else
  MsgBox "Autostart removed.", 64, "Whale Desktop Pet"
End If
