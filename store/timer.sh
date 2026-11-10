# Countdown Timer - AxisScript app
input S Seconds to count down:
if "%S%" == "" exit
:tick
echo %S%...
sleep 1
let S = %S% - 1
if %S% > 0 goto tick
echo Time is up!
