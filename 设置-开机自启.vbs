' Enable autostart via the HKCU Run key (no admin rights needed).
Option Explicit
Dim sh, fso, base, target
Set sh  = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
base = fso.GetParentFolderName(WScript.ScriptFullName)
If Not fso.FileExists(base & "\启动鲸鱼桌宠.vbs") Then
  MsgBox "启动鲸鱼桌宠.vbs not found next to this file:" & vbCrLf & base, 16, "Whale Desktop Pet"
  WScript.Quit 1
End If
target = "wscript.exe """ & base & "\启动鲸鱼桌宠.vbs"""
On Error Resume Next
sh.RegWrite "HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WhaleDesktopPet", target, "REG_SZ"
If Err.Number <> 0 Then
  MsgBox "写入注册表失败：" & vbCrLf & Err.Description, 16, "鲸鱼桌宠"
  WScript.Quit 1
End If
On Error GoTo 0
MsgBox "开机自启已开启。" & vbCrLf & vbCrLf & "下次登录 Windows 时桌宠会自动出现。", 64, "鲸鱼桌宠"
