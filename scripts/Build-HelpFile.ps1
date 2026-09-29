# Writes en-US/MegaProvider.dll-Help.xml (what Get-Help reads for a binary module) and the about_ topic
# next to a built module. The syntax, parameter sets and pipeline input come from the cmdlets themselves;
# the prose comes from src/MegaProvider/Help/MegaProvider.Help.psd1, which must describe every parameter.
# Run by dev.ps1 after each build, in a fresh pwsh (the module's DLL stays locked in whatever loads it).
param(
    [Parameter(Mandatory)] [string]$ModulePath   # the built MegaProvider.psd1
)
$ErrorActionPreference = 'Stop'
$source = Join-Path (Split-Path $PSScriptRoot -Parent) 'src/MegaProvider/Help'
$prose = Import-PowerShellDataFile (Join-Path $source 'MegaProvider.Help.psd1')
$module = Import-Module $ModulePath -PassThru -Force

$common = [Management.Automation.Cmdlet]::CommonParameters + [Management.Automation.Cmdlet]::OptionalCommonParameters
$shouldProcess = @{
    WhatIf  = 'Shows what would happen if the command runs. The command does not run.'
    Confirm = 'Prompts for confirmation before each change.'
}
$problems = [Collections.Generic.List[string]]::new()

$out = Join-Path (Split-Path $ModulePath) 'en-US'
New-Item -ItemType Directory -Force $out | Out-Null
$settings = [Xml.XmlWriterSettings]@{ Indent = $true; Encoding = [Text.UTF8Encoding]::new($false) }
$xml = [Xml.XmlWriter]::Create((Join-Path $out 'MegaProvider.dll-Help.xml'), $settings)
$ns = @{
    maml    = 'http://schemas.microsoft.com/maml/2004/10'
    command = 'http://schemas.microsoft.com/maml/dev/command/2004/10'
    dev     = 'http://schemas.microsoft.com/maml/dev/2004/10'
}
function Start-Element([string]$name) { $prefix, $local = $name -split ':'; $xml.WriteStartElement($prefix, $local, $ns[$prefix]) }
function Write-Element([string]$name, [string]$text) { Start-Element $name; $xml.WriteString($text); $xml.WriteEndElement() }
function Write-Paragraphs([string]$name, [string[]]$paragraphs) {
    Start-Element $name
    foreach ($p in $paragraphs) { Write-Element 'maml:para' $p }
    $xml.WriteEndElement()
}
function Get-TypeName([type]$type) {
    if ($type -eq [switch]) { return 'SwitchParameter' }
    if ($type.IsGenericType -and $type.GetGenericTypeDefinition() -eq [Nullable`1]) { $type = $type.GetGenericArguments()[0] }
    $type.Name
}
function Get-PipelineInput($p) {
    $how = @(if ($p.ValueFromPipeline) { 'ByValue' }; if ($p.ValueFromPipelineByPropertyName) { 'ByPropertyName' })
    $how ? "True ($($how -join ', '))" : 'False'
}
function Write-Parameter($p, [string]$description, [bool]$globbing) {
    Start-Element 'command:parameter'
    $xml.WriteAttributeString('required', "$($p.IsMandatory)".ToLower())
    $xml.WriteAttributeString('globbing', "$globbing".ToLower())
    $xml.WriteAttributeString('pipelineInput', (Get-PipelineInput $p))
    $xml.WriteAttributeString('position', ($p.Position -ge 0 ? "$($p.Position)" : 'named'))
    $xml.WriteAttributeString('aliases', ($p.Aliases.Count ? ($p.Aliases -join ', ') : 'none'))
    Write-Element 'maml:name' $p.Name
    Write-Paragraphs 'maml:description' $description
    $typeName = Get-TypeName $p.ParameterType
    if ($typeName -ne 'SwitchParameter') {
        Start-Element 'command:parameterValue'; $xml.WriteAttributeString('required', 'true'); $xml.WriteString($typeName); $xml.WriteEndElement()
    }
    Start-Element 'dev:type'; Write-Element 'maml:name' $typeName; $xml.WriteEndElement()
    $xml.WriteEndElement()
}
function Write-Types([string]$list, [string]$item, [hashtable]$types) {
    if (-not $types) { return }
    Start-Element $list
    foreach ($t in $types.Keys | Sort-Object) {
        Start-Element $item
        Start-Element 'dev:type'; Write-Element 'maml:name' $t; $xml.WriteEndElement()
        Write-Paragraphs 'maml:description' $types[$t]
        $xml.WriteEndElement()
    }
    $xml.WriteEndElement()
}

$xml.WriteStartDocument()
$xml.WriteStartElement('helpItems', 'http://msh')
$xml.WriteAttributeString('schema', 'maml')
foreach ($name in $module.ExportedCmdlets.Keys | Sort-Object) {
    $cmd = Get-Command $name -Module $module.Name
    $help = $prose[$name]
    if (-not $help) { $problems.Add("$name has no entry in MegaProvider.Help.psd1"); continue }
    $describe = {
        param($parameterName)
        if ($shouldProcess.ContainsKey($parameterName)) { return $shouldProcess[$parameterName] }
        $text = $help.Parameters.$parameterName
        if (-not $text) { $problems.Add("$name -$parameterName has no description") }
        $text
    }
    $visible = { param($p) $p.Name -notin $common -or $shouldProcess.ContainsKey($p.Name) }
    $shown = { param($p) -not ($cmd.Parameters[$p.Name].Attributes | Where-Object { $_ -is [Management.Automation.ParameterAttribute] -and $_.DontShow }) }
    # Positional first, then mandatory, then the rest in declaration order; -WhatIf and -Confirm last.
    $ordered = {
        param($parameters)
        $parameters | Sort-Object -Stable { $_.Position -ge 0 ? $_.Position : 1000 }, { -not $_.IsMandatory }, { $shouldProcess.ContainsKey($_.Name) }
    }
    $wildcards = { param($p) [bool]($cmd.Parameters[$p.Name].Attributes | Where-Object { $_ -is [Management.Automation.SupportsWildcardsAttribute] }) }

    Start-Element 'command:command'
    foreach ($prefix in $ns.Keys) { $xml.WriteAttributeString('xmlns', $prefix, $null, $ns[$prefix]) }
    Start-Element 'command:details'
    Write-Element 'command:name' $name
    Write-Element 'command:verb' $cmd.Verb
    Write-Element 'command:noun' $cmd.Noun
    Write-Paragraphs 'maml:description' $help.Synopsis
    $xml.WriteEndElement()
    Write-Paragraphs 'maml:description' $help.Description

    Start-Element 'command:syntax'
    foreach ($set in $cmd.ParameterSets) {
        Start-Element 'command:syntaxItem'
        Write-Element 'maml:name' $name
        foreach ($p in & $ordered ($set.Parameters | Where-Object { (& $visible $_) -and (& $shown $_) })) {
            Write-Parameter $p (& $describe $p.Name) (& $wildcards $p)
        }
        $xml.WriteEndElement()
    }
    $xml.WriteEndElement()

    # Parameters: each once, with its first set's attributes (for these cmdlets they agree across sets).
    Start-Element 'command:parameters'
    $seen = @{}
    $unique = foreach ($set in $cmd.ParameterSets) {
        foreach ($p in $set.Parameters | Where-Object { (& $visible $_) -and (& $shown $_) -and -not $seen[$_.Name] }) {
            $seen[$p.Name] = $true
            $p
        }
    }
    foreach ($p in & $ordered $unique) { Write-Parameter $p (& $describe $p.Name) (& $wildcards $p) }
    $xml.WriteEndElement()
    foreach ($documented in $help.Parameters.Keys) {
        if (-not $seen[$documented]) { $problems.Add("$name documents -$documented, which it does not have") }
    }

    Write-Types 'command:inputTypes' 'command:inputType' $help.Inputs
    Write-Types 'command:returnValues' 'command:returnValue' $help.Outputs
    if ($help.Notes) {
        Start-Element 'maml:alertSet'
        Write-Paragraphs 'maml:alert' $help.Notes
        $xml.WriteEndElement()
    }
    Start-Element 'command:examples'
    $i = 0
    foreach ($example in $help.Examples) {
        Start-Element 'command:example'
        Write-Element 'maml:title' "-------------------------- Example $((++$i)) --------------------------"
        Write-Element 'dev:code' $example.Code
        Start-Element 'dev:remarks'; Write-Element 'maml:para' $example.Remarks; $xml.WriteEndElement()
        $xml.WriteEndElement()
    }
    $xml.WriteEndElement()
    Start-Element 'command:relatedLinks'
    foreach ($link in $help.Links) {
        Start-Element 'maml:navigationLink'; Write-Element 'maml:linkText' $link; Write-Element 'maml:uri' ''; $xml.WriteEndElement()
    }
    $xml.WriteEndElement()
    $xml.WriteEndElement()
}
foreach ($documented in $prose.Keys) {
    if ($documented -notin $module.ExportedCmdlets.Keys) { $problems.Add("MegaProvider.Help.psd1 documents $documented, which the module does not export") }
}
$xml.WriteEndElement()
$xml.Close()

Copy-Item (Join-Path $source 'about_MegaProvider.help.txt') $out
if ($problems.Count) { throw "Help is incomplete:`n  " + ($problems -join "`n  ") }
