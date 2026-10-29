10 F%=OPENOUT "PDFS::SimPhone-Internal.$.Books.made"
20 PRINT#F%,"written by BASIC"
30 BPUT#F%,"second line"
40 CLOSE#F%
50 F%=OPENUP "PDFS::SimPhone-Internal.$.Books.made"
60 PTR#F%=EXT#F%
70 BPUT#F%,"appended"
80 CLOSE#F%
90 F%=OPENIN "PDFS::SimPhone-Internal.$.Books.made"
100 PRINT "length ";EXT#F%
110 CLOSE#F%
120 REM a 12MB file written a bit at a time, with no size given: it outgrows
130 REM its first memory (a dynamic area) and moves to a scrap file
140 DIM B% 262144
150 F%=OPENOUT "PDFS::SimPhone-Internal.$.Download.grown"
160 FOR C%=0 TO 47
170 FOR I%=0 TO 262143 STEP 4096:B%!I%=C%*262144+I%:NEXT
180 SYS "OS_GBPB",2,F%,B%,262144
190 NEXT
200 CLOSE#F%
210 F%=OPENIN "PDFS::SimPhone-Internal.$.Download.grown"
220 OK%=EXT#F%=12582912
230 FOR C%=0 TO 47
240 SYS "OS_GBPB",4,F%,B%,262144
250 FOR I%=0 TO 262143 STEP 4096:IF B%!I%<>C%*262144+I% THEN OK%=FALSE
260 NEXT
270 NEXT
280 CLOSE#F%
290 IF OK% THEN PRINT "grown ok" ELSE PRINT "grown wrong"
