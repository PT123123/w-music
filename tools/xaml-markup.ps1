#requires -Version 5.1
<#
    xaml-markup.ps1 -- drive the real C++ XAML markup compiler (XamlCompiler.exe)
    without MSBuild. Dot-source from dev-build.ps1.

    Exposed:
        New-CombinedAppIdl     one .idl that #includes every app idl
        New-FoundationWinmd    a "Windows.Foundation"-named copy of FoundationContract
        Get-ContractWinmd      newest winmd of a Windows SDK API contract
        Invoke-AppMidl         midlrt the combined idl -> a single w_music.winmd
        Invoke-XamlMarkup      XamlCompiler Pass1 + Pass2 -> .xaml.g.h/.hpp/.xbf
        Publish-XamlCodegen    place generated headers where cppwinrt's .g.h finds them

    WHY THIS EXISTS
    ---------------
    The VS component that normally invokes the C++ XAML markup compiler
    ("C++ (v143) Universal Windows Platform tools" ->
    Microsoft.Windows.UI.Xaml.Cpp.targets) is not installed on this machine.
    The compiler itself, however, ships inside the WindowsAppSDK NuGet package
    (microsoft.windowsappsdk.winui\tools\net472\XamlCompiler.exe) and speaks a
    plain JSON protocol:

        XamlCompiler.exe <input.json> <output.json>

    It is run twice (this is the real MSBuild contract, mirrored from
    Microsoft.UI.Xaml.Markup.Compiler.interop.targets):

        Pass1  IsPass1=true   CompileMode=RealBuildPass1
               -> <Page>.xaml.g.h, <Page>.xbf
        Pass2  IsPass1=false  CompileMode=RealBuildPass2
               -> <Page>.xaml.g.hpp (InitializeComponent/Connect bodies),
                  XamlTypeInfo.g.cpp, XamlBindingInfo.xaml.g.hpp

    Pass2 additionally REQUIRES LocalAssembly -- the app's own metadata -- and it
    must be a SINGLE winmd whose internal assembly name equals RootNamespace.

    THREE CONSTRAINTS THAT COST REAL TIME TO FIND
    ---------------------------------------------
    1. midlrt names the output winmd after the *input .idl file*, NOT the /winmd
       path. So the combined idl must literally be called w_music.idl, else
       LocalAssembly's claimed identity (w_music) will not match the winmd's
       internal identity -> the local reference never resolves.
       Also: passing N idls to a single midlrt call silently compiles only the
       FIRST one (verified in midl-all.log), and per-idl + mdmerge.exe fails with
       MDM2018 on the cross-winmd references. Hence: ONE combined idl, ONE midlrt
       call, N #includes.

    2. midlrt ALWAYS emits a bare
       "Windows.Foundation, Version=255.255.255.255" assembly reference, and its
       /metadata_dir MUST be the union folder (UnionMetadata\<ver>) or it dies
       with MIDL4034 (cannot load Windows.winmd). No shipped winmd has the
       internal identity "Windows.Foundation", and XamlCompiler resolves assembly
       references by that identity -- so the plain contract winmd
       (Windows.Foundation.FoundationContract) does NOT satisfy it -> WMC1006.

    3. Referencing the union Windows.winmd in XamlCompiler *does* resolve that
       reference, but the union also re-declares the Windows.UI.Xaml.* types that
       UniversalApiContract defines -> WMC0901 "type exists twice".

    Fix for 2+3: take FoundationContract -- it defines exactly the
    Windows.Foundation.* types and NONE of the Windows.UI.* ones -- and rename its
    internal assembly to "Windows.Foundation". XamlCompiler then gets WinUI +
    that renamed winmd + both contracts, and never sees the union. Both passes
    come out with zero WMC.

    The rename is a raw in-place edit of the metadata #Strings heap: the new name
    is shorter than the old, so we overwrite it plus its NUL and leave the tail as
    unreferenced garbage -- no other offset in the file moves.
#>

Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# internal helpers
# ---------------------------------------------------------------------------

function Invoke-CapturedNative {
    # Same reason as dev-build.ps1's Invoke-Native: PowerShell turns a native
    # tool's stderr into ErrorRecords and a 'Stop' preference would abort the call
    # before the tool finishes. midlrt writes progress to stderr.
    param([scriptblock]$Action, [string]$LogPath)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Action *>&1 | Out-File -FilePath $LogPath -Encoding utf8 -Width 4096 }
    finally { $ErrorActionPreference = $previous }
    return $LASTEXITCODE
}

function New-XamlItem {
    # One XamlCompiler MSBuildItem, in the exact shape its JSON deserializer wants.
    param([string]$Path, [string]$AssemblyName)
    return [ordered]@{
        ItemSpec = $Path
        Metadata = [ordered]@{ Identity = $Path; FullPath = $Path; ReferenceAssemblyName = $AssemblyName }
    }
}

# ---------------------------------------------------------------------------
# inputs XamlCompiler / midlrt cannot derive on their own
# ---------------------------------------------------------------------------

function Get-ContractWinmd {
    param([string]$SdkRoot, [string]$SdkVersion, [string]$ContractName)
    $root = Join-Path $SdkRoot "References\$SdkVersion\$ContractName"
    if (-not (Test-Path $root)) { return $null }
    $ver = Get-ChildItem $root -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $ver) { return $null }
    return (Join-Path $ver.FullName "$ContractName.winmd")
}

function New-CombinedAppIdl {
    # One translation unit for midlrt. The FILE NAME is load-bearing: it becomes
    # the winmd's assembly identity (see constraint 1 in the header).
    param([string]$OutDir, [string[]]$IdlPaths)
    $sb = [System.Text.StringBuilder]::new()
    foreach ($full in $IdlPaths) {
        if (-not (Test-Path $full)) { throw "idl not found: $full" }
        # Absolute paths: the combined file lives in build\, not next to the idls.
        [void]$sb.AppendLine("#include `"$full`"")
    }
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $path = Join-Path $OutDir 'w_music.idl'
    $text = $sb.ToString()
    if (-not (Test-Path $path) -or ([IO.File]::ReadAllText($path) -ne $text)) {
        [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
    }
    return $path
}

function New-MetadataProviderIdl {
    # The markup compiler emits w_music/XamlMetaDataProvider.h, which implements
    # Microsoft.UI.Xaml.Markup.IXamlMetadataProvider on the app's behalf -- but it
    # needs a matching runtimeclass in the app's OWN metadata, or cppwinrt never
    # generates the XamlMetaDataProvider.g.h that header includes. The generated
    # header documents exactly this ("you may be missing a declaration for the
    # XamlMetaDataProvider runtimeclass in your IDL").
    param([string]$OutDir, [string]$Namespace)
    # [default_interface] is required: midlrt only marks a class's own interface as
    # the default one, and a runtimeclass whose sole interface is imported from
    # another assembly otherwise ends up with NO default interface -- which
    # cppwinrt -component rejects ("does not have a default interface").
    $text = @"
namespace $Namespace
{
    [default_interface]
    runtimeclass XamlMetaDataProvider : Microsoft.UI.Xaml.Markup.IXamlMetadataProvider
    {
        XamlMetaDataProvider();
    };
}
"@
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $path = Join-Path $OutDir 'XamlMetaDataProvider.idl'
    if (-not (Test-Path $path) -or ([IO.File]::ReadAllText($path) -ne $text)) {
        [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
    }
    return $path
}

function New-FoundationWinmd {
    # FoundationContract with its internal assembly name shortened to
    # "Windows.Foundation" -- the only identity XamlCompiler can resolve the bare
    # Windows.Foundation reference against without dragging in the union.
    param([string]$SdkRoot, [string]$SdkVersion, [string]$OutDir)
    $src = Get-ContractWinmd $SdkRoot $SdkVersion 'Windows.Foundation.FoundationContract'
    if (-not $src) { throw 'Windows.Foundation.FoundationContract.winmd not found under the Windows SDK.' }
    $dst = Join-Path $OutDir 'Windows.Foundation.winmd'
    if ((Test-Path $dst) -and ((Get-Item $dst).LastWriteTimeUtc -ge (Get-Item $src).LastWriteTimeUtc)) { return $dst }

    $bytes = [IO.File]::ReadAllBytes($src)
    $latin1 = [Text.Encoding]::GetEncoding(28591)   # ISO-8859-1: byte <-> char is 1:1
    $old = 'Windows.Foundation.FoundationContract'
    $idx = $latin1.GetString($bytes).IndexOf($old + [char]0)
    if ($idx -lt 0) { throw "assembly name '$old' not found inside $src" }
    $newBytes = $latin1.GetBytes('Windows.Foundation')
    [Array]::Copy($newBytes, 0, $bytes, $idx, $newBytes.Length)
    $bytes[$idx + $newBytes.Length] = 0
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    [IO.File]::WriteAllBytes($dst, $bytes)
    Write-Host "  foundation: patched FoundationContract -> $(Split-Path -Leaf $dst) (offset $idx)"
    return $dst
}

# ---------------------------------------------------------------------------
# midlrt + XamlCompiler
# ---------------------------------------------------------------------------

function Invoke-AppMidl {
    # Compile the combined idl into the single winmd that doubles as
    # XamlCompiler's LocalAssembly and cppwinrt -component's -in.
    param(
        [string]$CombinedIdl, [string]$OutWinmd, [string]$OutHdr,
        [string]$SdkRoot, [string]$SdkVersion, [string]$SrcDir,
        [string[]]$ReferenceWinmds, [string]$LogPath,
        [scriptblock]$InvokeNative
    )
    $refsRoot = Join-Path $SdkRoot "References\$SdkVersion"
    $a = @($CombinedIdl, '/nologo', '/W1', '/nomidl')
    # /metadata_dir MUST be the union folder (MIDL4034 otherwise) and midlrt pulls
    # Windows.winmd in from there automatically -- so the union must NOT also be
    # listed as a /reference: its FoundationContract types would then be defined
    # twice (MIDL1002).
    $a += @('/metadata_dir', (Join-Path $SdkRoot "UnionMetadata\$SdkVersion"))
    foreach ($w in $ReferenceWinmds) { $a += @('/reference', $w) }
    $fc = Get-ContractWinmd $SdkRoot $SdkVersion 'Windows.Foundation.FoundationContract'
    $uc = Get-ContractWinmd $SdkRoot $SdkVersion 'Windows.Foundation.UniversalApiContract'
    if ($fc) { $a += @('/reference', $fc) }
    if ($uc) { $a += @('/reference', $uc) }
    $a += @('/I', $SrcDir, '/I', (Join-Path $SrcDir 'Views'), '/I', (Join-Path $SdkRoot "Include\$SdkVersion\winrt"))
    $a += @('/winmd', $OutWinmd, '/h', $OutHdr)

    $md = Split-Path -Parent $OutWinmd
    New-Item -ItemType Directory -Force -Path $md | Out-Null
    $exit = & $InvokeNative { & midlrt.exe @a } $LogPath
    if ($exit -ne 0) {
        Get-Content $LogPath -Tail 15 | ForEach-Object { Write-Host "  [midlrt] $_" }
        throw "midlrt (combined idl) failed with exit $exit; full output in $LogPath"
    }
    return $OutWinmd
}

function Invoke-XamlMarkup {
    # Runs XamlCompiler Pass1 then Pass2 and returns what it produced.
    param(
        [string]$SrcDir, [string]$OutDir, [string]$MergedWinmd, [string]$FoundationWinmd,
        [string]$SdkRoot, [string]$SdkVersion, [string]$XamlCompiler, [string]$ProjectPath,
        [string]$RootNamespace, [string]$VcMeta32, [string]$VcMeta64, [string]$GenXbfPath,
        [System.Collections.IDictionary]$ReferenceAssemblies,   # winmd path -> assembly name
        [string[]]$ReferencePaths, [string[]]$PageXaml, [string[]]$AppXaml,
        [string]$LogPath, [scriptblock]$InvokeNative
    )
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

    $refs = @()
    foreach ($key in $ReferenceAssemblies.Keys) { $refs += (New-XamlItem $key ([string]$ReferenceAssemblies[$key])) }
    $paths = @($ReferencePaths | ForEach-Object { New-XamlItem $_ '' })
    $pages = @($PageXaml | ForEach-Object { New-XamlItem $_ '' })
    $apps = @($AppXaml | ForEach-Object { New-XamlItem $_ '' })

    function New-Input([bool]$pass1) {
        $mode = if ($pass1) { 'RealBuildPass1' } else { 'RealBuildPass2' }
        return [ordered]@{
            Language = 'CppWinRT'; LanguageSourceExtension = '.cpp'
            RootNamespace = $RootNamespace; ProjectName = 'w-music'
            ProjectPath = $ProjectPath
            IsPass1 = $pass1; CompileMode = $mode; OutputPath = $OutDir
            XamlPages = $pages; XamlApplications = $apps
            ReferenceAssemblies = $refs; ReferenceAssemblyPaths = $paths
            WindowsSdkPath = $SdkRoot
            VCInstallPath32 = $VcMeta32; VCInstallPath64 = $VcMeta64
            GenXbfPath = $GenXbfPath
            SavedStateFile = (Join-Path $OutDir 'XamlSaveStateFile.xml')
            FeatureControlFlags = 'EnableDefaultValidationContextGeneration'
            UseVCMetaManaged = $true; XAMLFingerprint = $true
            TargetPlatformMinVersion = '10.0.19041.0'; PrecompiledHeaderFile = ''; CIncludeDirectories = ''
            PriIndexName = $RootNamespace; CodeGenerationControlFlags = ''
            EnabledXamlOptionalChanges = ''; DisabledXamlOptionalChanges = ''
            XamlResourceMapName = ''; XamlComponentResourceLocation = ''; VCInstallDir = ''
            FingerprintIgnorePaths = @($SdkRoot, (Join-Path $env:USERPROFILE '.nuget\packages'))
            SuppressWarnings = $null; DisableXbfGeneration = $false; OutputType = 'WinExe'
            LocalAssembly = @(New-XamlItem $MergedWinmd $RootNamespace)
        }
    }

    $result = [ordered]@{ Errors = @{}; CodeFiles = @(); XbfFiles = @(); XamlPagesFiles = @() }
    foreach ($pass1 in @($true, $false)) {
        $n = if ($pass1) { 1 } else { 2 }
        $inputJson = Join-Path $OutDir "xamlc-input.pass$n.json"
        $outputJson = Join-Path $OutDir "xamlc-output.pass$n.json"
        $text = (New-Input $pass1) | ConvertTo-Json -Depth 5
        [IO.File]::WriteAllText($inputJson, $text, (New-Object System.Text.UTF8Encoding($false)))
        # Remove any stale output so a silently-failing pass cannot be mistaken for
        # a successful one. Deliberately NOT Remove-Item: this environment guards
        # that cmdlet with a fail-closed safe-delete policy.
        if ([IO.File]::Exists($outputJson)) { [IO.File]::Delete($outputJson) }

        # XamlCompiler returns a non-zero exit code for informational reasons too,
        # so the output JSON -- not $LASTEXITCODE -- is the source of truth.
        $exit = & $InvokeNative { & $XamlCompiler $inputJson $outputJson } $LogPath
        if (-not (Test-Path $outputJson)) {
            Get-Content $LogPath -Tail 15 | ForEach-Object { Write-Host "  [xamlc] $_" }
            throw "XamlCompiler pass$n produced no output json (exit $exit)"
        }
        $o = Get-Content $outputJson -Raw | ConvertFrom-Json
        $errs = @($o.MSBuildLogEntries | Where-Object {
            $_.ErrorCode -match 'WMC' -or ($_.Message -match 'error' -and $_.Message -notmatch 'perfXC')
        })
        $result.Errors["Pass$n"] = $errs
        Write-Host "  pass$n : code=$(@($o.GeneratedCodeFiles).Count) xbf=$(@($o.GeneratedXbfFiles).Count) errors=$($errs.Count)"
        foreach ($e in ($errs | Sort-Object -Property Message -Unique | Select-Object -First 10)) {
            Write-Host "    [$($e.ErrorCode)] $($e.Message)"
        }
        if ($n -eq 2) {
            $result.CodeFiles = @($o.GeneratedCodeFiles)
            $result.XbfFiles = @($o.GeneratedXbfFiles)
            $result.XamlPagesFiles = @($o.GeneratedXamlPagesFiles)
        }
    }
    if ($result.Errors['Pass1'].Count -or $result.Errors['Pass2'].Count) {
        throw 'XamlCompiler reported errors (see above).'
    }
    return [pscustomobject]$result
}

function Publish-XamlCodegen {
    # cppwinrt's <Page>.g.h ends with
    #     #if __has_include("w_music/<Page>.xaml.g.h")
    #       #include "w_music/<Page>.xaml.g.h"
    #     #else  using <Page>T = <Page>_base<D, I...>;  #endif
    # so dropping the real markup-compiler headers into gen\component\w_music\
    # replaces the old hand-written stand-in and makes the page code-behind
    # compile against the genuine x:Name fields and InitializeComponent.
    param([string]$FromDir, [string]$ToDir, [string[]]$Names)
    New-Item -ItemType Directory -Force -Path $ToDir | Out-Null
    # EVERYTHING the compiler generated that C++ consumes goes to ONE directory.
    # If a header with the same name also stays reachable through another
    # include-path directory, a translation unit can pull in two distinct files
    # with identical content and cl.exe reports C2011 "type redefinition".
    $copied = 0
    $files = @()
    foreach ($n in $Names) {
        $files += (Join-Path $FromDir "$n.xaml.g.h")
        $files += (Join-Path $FromDir "$n.xaml.g.hpp")
    }
    foreach ($n in @('XamlBindingInfo.xaml.g.h', 'XamlBindingInfo.xaml.g.hpp',
                     'XamlTypeInfo.xaml.g.h', 'XamlTypeInfo.g.cpp',
                     'XamlTypeInfo.Impl.g.cpp', 'XamlMetaDataProvider.h',
                     'XamlLibMetadataProvider.g.cpp')) {
        $files += (Join-Path $FromDir $n)
    }
    foreach ($f in $files) {
        if (-not (Test-Path $f)) { continue }
        $dst = Join-Path $ToDir (Split-Path -Leaf $f)
        # content-compare so ninja's incremental mode keeps working (mtime stable)
        if ((Test-Path $dst) -and ([IO.File]::ReadAllText($dst) -eq [IO.File]::ReadAllText($f))) { continue }
        Copy-Item $f $dst -Force
        $copied++
    }
    return $copied
}
