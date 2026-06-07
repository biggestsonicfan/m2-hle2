# Build the standalone unit tests (Release/x64) and run them via ctest.
$ms = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
foreach ($t in "mem_test","i960_test","rom_test","emu_test","boot_test","cop_test","geo_test","m68k_test","input_test") {
    & $ms "$PSScriptRoot\build\$t.vcxproj" /p:Configuration=Release /p:Platform=x64 /m:16 /v:m
    if ($LASTEXITCODE -ne 0) { Write-Host "build failed: $t"; exit 1 }
}
$ctest = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
Push-Location "$PSScriptRoot\build"
& $ctest -C Release --output-on-failure
$rc = $LASTEXITCODE
Pop-Location
exit $rc
