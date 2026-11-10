# Computer Quiz - AxisScript app
let SCORE = 0
echo Answer with the letter of your choice.
echo
echo 1. Which command lists files in Windows?   a) ls   b) dir   c) cat
input A Answer:
if %A% == b then let SCORE = %SCORE% + 1
echo 2. What does CPU stand for?   a) Central Processing Unit   b) Computer Power Unit
input A Answer:
if %A% == a then let SCORE = %SCORE% + 1
echo 3. Which system uses the sysctl command?   a) FreeBSD   b) MS-DOS
input A Answer:
if %A% == a then let SCORE = %SCORE% + 1
echo 4. How many bits are in a byte?   a) 4   b) 8   c) 16
input A Answer:
if %A% == b then let SCORE = %SCORE% + 1
echo 5. Which program starts first when a PC boots?   a) the bootloader   b) the web browser
input A Answer:
if %A% == a then let SCORE = %SCORE% + 1
echo
echo You scored %SCORE% out of 5.
