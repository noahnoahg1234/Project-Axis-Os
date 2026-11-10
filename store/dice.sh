# Dice Roller - AxisScript app
input N How many dice (1-6)?
if "%N%" == "" then let N = 1
if %N% > 6 then let N = 6
let I = 0
let TOTAL = 0
:roll
random D 6
let TOTAL = %TOTAL% + %D%
let I = %I% + 1
echo Die %I%: %D%
if %I% < %N% goto roll
echo Total: %TOTAL%
