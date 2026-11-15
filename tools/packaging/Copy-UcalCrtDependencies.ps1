[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackageRoot,
      [Parameter(Mandatory=$true)][string]$RedistRoot)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot '../../ucal/tools/UcalPeContract.psm1') -Force
$root=(Resolve-Path -LiteralPath $PackageRoot).Path
# Each independently loaded frontend directory and each worker executable needs
# its own loader-visible dependencies. Backend DLLs execute in those workers.
$directories=@(Get-ChildItem -LiteralPath (Join-Path $root 'runtime') -Directory)
$frontends=Join-Path $root 'frontends'
foreach($module in Get-ChildItem -LiteralPath $frontends -Directory){
    $directories+=@(Get-ChildItem -LiteralPath $module.FullName -Directory | Where-Object {$_.Name -in @('x86','x64')})
}
foreach($directory in $directories){
    $arch=$directory.Name
    $crt=@(Get-ChildItem -LiteralPath (Join-Path $RedistRoot $arch) -Directory | Where-Object {$_.Name -like '*.CRT'})
    if($crt.Count -ne 1){throw "Microsoft C++ runtime directory missing or ambiguous: $arch"}
    $available=@{}
    foreach($file in Get-ChildItem -LiteralPath $crt[0].FullName -Filter '*.dll' -File){$available[$file.Name]=$file.FullName}
    $queue=[Collections.Generic.Queue[string]]::new()
    foreach($file in Get-ChildItem -LiteralPath $directory.FullName -File | Where-Object {$_.Extension -in @('.dll','.exe')}){$queue.Enqueue($file.FullName)}
    $seen=@{}
    while($queue.Count){
        $path=$queue.Dequeue()
        if($seen.ContainsKey($path)){continue}; $seen[$path]=$true
        $pe=Get-UcalPeContract $path -ImportsOnly
        if($pe.Machine -ne $arch){throw "Wrong architecture in runtime directory: $path"}
        foreach($import in $pe.Imports){
            $name=[string]$import.Dll
            if(-not $available.ContainsKey($name)){continue}
            $target=Join-Path $directory.FullName $name
            if(-not(Test-Path -LiteralPath $target)){
                Copy-Item -LiteralPath $available[$name] -Destination $target
                $queue.Enqueue($target)
            }
        }
    }
}
