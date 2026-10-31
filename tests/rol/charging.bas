REM PDFS desktop test: a phone plugged in set to charging, switched to file
REM transfer a few seconds later (the USB emulator's "charging" scenario),
REM optionally while !IrisRAM copies Iris to the RAM disc (as at a Pi's boot);
REM or ("allow") a tablet that asks its owner to allow access to its data
REM (the simulator's --needs-allow)
log%=0:maxgap%=0:ON ERROR PROClog("Error: "+REPORT$+" at line "+STR$ERL):PROCfinish:END
DIM b% 1024, n% 256
SYS "Wimp_Initialise",310,&4B534154,"PDFS charging test",0 TO ,me%
SYS "OS_FSControl",37,"@",n%,0,0,255
here$=FNstr(n%)
log%=OPENOUT(here$+".results")
f%=OPENIN(here$+".opts"):scen$=GET$#f%:iris%=VAL GET$#f%:limit%=VAL GET$#f%:sw$=GET$#f%:CLOSE#f%
PROClog("scenario "+scen$+", IrisRAM "+STR$iris%+", watching for "+STR$(limit%DIV100)+"s")
IF LEFT$(scen$,5)<>"allow" THEN OSCLI "Set PDFSEmu$Scenario "+scen$
IF scen$="allow-reconnect" THEN OSCLI "Set PDFSEmu$Scenario reconnect"
IF sw$<>"" THEN OSCLI "Set PDFSEmu$SwitchAfter "+sw$:PROClog("the phone is switched "+sw$+"cs after PDFS's first command")
SYS "OS_ReadMonotonicTime" TO t0%
REM PDFS as at boot: its filer starts with the desktop, and mounts what's plugged in
SYS "Wimp_StartTask","Run "+here$+".PDFS"
PROClog(FNt+"PDFS started")
IF iris% THEN
  SYS "XOS_ChangeDynamicArea",5,120*1024*1024 TO ;f%
  PROCwait(20)
  PROClog(FNt+"starting !IrisRAM")
  SYS "Wimp_StartTask","Run Boot:^.Documents.Iris.IrisRAMv2.!IrisRAM"
  PROClog(FNt+"!IrisRAM's task returned")
ENDIF
icons%=0:iconsat%=-1
REPEAT
  PROCwait(50)
  icons%=FNicons
  IF icons%>=2 AND iconsat%<0 THEN iconsat%=FNnow:PROClog(FNt+"the phone's icons are on the icon bar")
UNTIL FNnow>=limit% OR (icons%>=2 AND FNnow>iconsat%+300)
PROClog("longest time the desktop was held: "+STR$maxgap%+"cs")
PROClog("icons: "+STR$icons%)
SYS "XOS_File",17,"RAM::0.$.!Iris.!Run" TO o%
PROClog("iris in RAM disc: "+FNyes(o%=1))
PROClog("hazards: "+FNvar("PDFSEmu$Hazards","0"))
PROClog("leftover streams: "+FNvar("PDFSEmu$Leftover","0"))
IF icons%>=2 THEN OSCLI "Spool "+here$+".cat":OSCLI "Cat PDFS::SimPhone-Internal.$":OSCLI "Spool"
OSCLI "TBShot"
PROCfinish
END

DEF PROCfinish
OSCLI "Spool "+here$+".pdfslog":OSCLI "PDFSLog":OSCLI "Spool"
PROClog("done")
IF log% THEN CLOSE#log%:log%=0
SYS "Wimp_CloseDown"
ENDPROC

DEF FNnow:LOCAL t%:SYS "OS_ReadMonotonicTime" TO t%:=t%-t0%
DEF FNt:="["+STR$(FNnow DIV 100)+"."+RIGHT$("0"+STR$(FNnow MOD 100),2)+"s] "
DEF FNyes(x%):IF x% THEN ="yes" ELSE ="no"

DEF FNvar(v$,d$)
LOCAL l%,f%
SYS "XOS_ReadVarVal",v$,b%,64,0,0 TO ,,l%;f%
IF (f% AND 1) THEN =d$
b%?l%=13:=FNstr(b%)

DEF FNicons
LOCAL i%,c%,f%
FOR i%=0 TO 80
  b%!0=-2:b%!4=i%
  SYS "XWimp_GetIconState",,b% TO ;f%
  IF (f% AND 1)=0 THEN IF (b%!24 AND &101)=&101 THEN IF FNstr(b%!32)="Spdfs_dev" THEN c%+=1
NEXT
=c%

DEF PROClog(s$)
IF log% THEN BPUT#log%,s$
ENDPROC

DEF FNstr(p%):LOCAL s$:WHILE ?p%>=32:s$+=CHR$?p%:p%+=1:ENDWHILE:=s$

DEF PROCwait(cs%)
LOCAL e%,r%,now%,last%
SYS "OS_ReadMonotonicTime" TO e%:last%=e%:e%+=cs%
REPEAT
  SYS "Wimp_PollIdle",&1830,b%,last%+5 TO r%
  IF (r%=17 OR r%=18) AND b%!16=0 THEN PROCfinish:END
  SYS "OS_ReadMonotonicTime" TO now%
  IF now%-last%>maxgap% THEN maxgap%=now%-last%
  IF now%-last%>200 THEN PROClog(FNt+"the desktop was held for "+STR$(now%-last%)+"cs")
  last%=now%
UNTIL now%-e%>=0
ENDPROC
