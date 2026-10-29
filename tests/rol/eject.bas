REM Eject: refused while a file is open
F%=OPENIN "PDFS::SimPhone-Internal.$.Download.notes/txt"
SYS "XOS_CLI","PDFSEject SimPhone-Internal all" TO e%;f%
IF f% AND 1 THEN PRINT "eject refused: &";~!e%;" ";FNz(e%+4) ELSE PRINT "eject NOT refused"
CLOSE#F%
END
DEF FNz(p%):LOCAL s$:WHILE ?p%:s$+=CHR$?p%:p%+=1:ENDWHILE:=s$
