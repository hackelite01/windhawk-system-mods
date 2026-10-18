' Copies every file in copytest-src to copytest-dst using the Windows shell copy
' (shows the normal progress dialog), then waits for the copy to finish.
Option Explicit
Dim fso, sh, src, dst, total, waited, f, size, goal
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh = CreateObject("Shell.Application")
src = "C:\Users\Public\copytest-src"
dst = "C:\Users\Public\copytest-dst"
If Not fso.FolderExists(dst) Then fso.CreateFolder dst

goal = 0
For Each f In fso.GetFolder(src).Files
    goal = goal + CDbl(f.Size)
Next

sh.NameSpace(dst).CopyHere sh.NameSpace(src).Items, 0

waited = 0
Do
    WScript.Sleep 500
    waited = waited + 1
    size = 0
    For Each f In fso.GetFolder(dst).Files
        size = size + CDbl(f.Size)
    Next
Loop Until size >= goal Or waited > 240

WScript.Sleep 3000
