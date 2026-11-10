# Prova ping_core su Linux dentro Docker (contro un finto room server con ENet vero).
# Uso:  powershell -File tests\run_tests.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
docker build -q -t azahar-ping-test "$PSScriptRoot"
if ($LASTEXITCODE -ne 0) { exit 1 }
docker run --rm --cap-add NET_ADMIN -v "${root}:/work" azahar-ping-test bash -c "sed -i 's/\r$//' /work/tests/run_tests.sh && bash /work/tests/run_tests.sh"
exit $LASTEXITCODE
