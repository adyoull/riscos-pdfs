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
