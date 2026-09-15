# Compila ed esegue i test su host su Windows, dove non c'è g++.
#
#   powershell -ExecutionPolicy Bypass -File test\esegui.ps1
#
# Copre la stessa suite di esegui.sh, che resta il runner di riferimento su
# macOS e Linux. Il fuso orario dei test non viene dalla libc ma dalle regole
# POSIX di stub\fuso_posix.h, che è ciò che rende test_scheduler eseguibile
# anche qui: il CRT Windows non sa leggere i campi di transizione di una
# stringa TZ POSIX.
#
# Il compilatore è MSVC di Visual Studio, già installato su questa macchina:
# non si installa niente e l'ambiente del toolset vive solo dentro il processo
# figlio, quindi nessuna variabile permanente. La toolchain xtensa del core
# ESP32 non serve allo scopo: è un cross-compiler e non produce eseguibili
# Windows.
#
# Gli artefatti stanno fuori dal repo, sotto A:\tmp come ogni altro prodotto di
# compilazione di questa macchina.

$ErrorActionPreference = 'Stop'

# La console di Windows PowerShell parte in codepage OEM: senza questa riga gli
# accenti e i separatori dei messaggi escono illeggibili.
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false

$radice = Split-Path -Parent $PSScriptRoot
$test   = $PSScriptRoot
$out    = 'A:\tmp\epd-test'

# /Zc:preprocessor NON è opzionale: Log.h usa __VA_OPT__, che il preprocessore
# tradizionale di MSVC non conosce, e senza il flag la compilazione si ferma
# con un errore di sintassi invece che con un avviso.
$flags = '/nologo /std:c++20 /Zc:preprocessor /EHsc /W4'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere))
{
	Write-Host "vswhere non trovato: serve Visual Studio con il toolset C++ (workload Desktop development with C++)."
	exit 1
}

$vs = & $vswhere -latest -products * `
	-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
	-property installationPath
if (-not $vs)
{
	Write-Host "Nessuna installazione di Visual Studio con il toolset C++ x64."
	exit 1
}

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars))
{
	Write-Host "vcvars64.bat non trovato in $vs."
	exit 1
}

New-Item -ItemType Directory -Force -Path $out | Out-Null

$falliti = 0

foreach ($nome in @('test_timings', 'test_scheduler'))
{
	Write-Host "── $nome ─────────────────────────────────────────────────────"

	$exe = Join-Path $out "$nome.exe"
	$src = Join-Path $test "$nome.cpp"
	# Compilazione dentro un .bat invece che con cmd /c "...": vcvars64 va
	# chiamato nello stesso processo di cl, perchè imposta INCLUDE, LIB e PATH
	# del toolset, e passare l'intera riga a cmd la espone a un secondo giro di
	# quoting che perde gli argomenti con spazi.
	$bat = Join-Path $out 'compila.bat'
	@(
		'@echo off'
		"call `"$vcvars`" >nul 2>&1"
		# Il backslash finale di /Fo va raddoppiato: cl interpreta \" come una
		# virgoletta letterale e si mangerebbe il resto della riga di comando.
		"cl $flags /I `"$test\stub`" /I `"$radice`" /Fo`"$out\\`" /Fe`"$exe`" `"$src`""
	) | Set-Content -Path $bat -Encoding ASCII
	cmd /c "`"$bat`""

	if ($LASTEXITCODE -ne 0)
	{
		Write-Host "compilazione FALLITA"
		$falliti++
	}
	else
	{
		& $exe
		if ($LASTEXITCODE -ne 0) { $falliti++ }
	}
	Write-Host ""
}

if ($falliti -ne 0)
{
	Write-Host "$falliti test con fallimenti"
	exit 1
}
Write-Host "tutti i test superati"
