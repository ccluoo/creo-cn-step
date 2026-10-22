$here = "C:/Users/Administrator/WorkBuddy/2026-10-20-14-05-22/creo_toolkit_cn_step"
$map = @{ "8"="8.0.12.0"; "9"="9.0.10.0"; "10"="10.0.8.0"; "11"="11.0.4.0"; "12"="12.4.1.0"; "13"="13.4.1.0" }
foreach ($v in 8,9,10,11,12,13) {
  Write-Output "########## Creo $v ($($map[[string]$v])) ##########"
  & "$here\test_one.ps1" -Version $v -Install $map[[string]$v] -WaitSeconds 120
}
