$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
# Read the original command line: PowerShell's -File argument binder splits
# native switches containing colons (for example -std:c++20 and -IC:\...).
$nativeCommand = [regex]::Match([Environment]::CommandLine,
    '(?i)\s-File\s+(?:"[^"]*"|\S+)\s+(?:"(?<compiler>[^"]+)"|(?<compiler>\S+))(?<arguments>.*)$')
if (-not $nativeCommand.Success) { throw 'Missing compiler command after -File.' }

$compilerProcess = [Diagnostics.Process]::new()
$compilerProcess.StartInfo.FileName = $nativeCommand.Groups['compiler'].Value
$compilerProcess.StartInfo.Arguments = $nativeCommand.Groups['arguments'].Value
$compilerProcess.StartInfo.UseShellExecute = $false
$compilerProcess.StartInfo.RedirectStandardOutput = $true
$compilerProcess.StartInfo.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
$compilerProcess.StartInfo.EnvironmentVariables['VSLANG'] = '1033'

# Some Korean MSVC installations contain only clui.dll/1042, so VSLANG=1033
# falls back to Korean. Normalize that prefix and use UTF-8 for compiler output
# before Ninja reads /showIncludes; do not depend on the configuring shell's code page.
$koreanPrefix = '^\uCC38\uACE0: \uD3EC\uD568 \uD30C\uC77C:'
try {
    $null = $compilerProcess.Start()
    while (($compilerLine = $compilerProcess.StandardOutput.ReadLine()) -ne $null) {
        [Console]::WriteLine([regex]::Replace($compilerLine, $koreanPrefix, 'Note: including file:'))
    }
    $compilerProcess.WaitForExit()
    $compilerExit = $compilerProcess.ExitCode
} finally {
    $compilerProcess.Dispose()
}
exit $compilerExit
