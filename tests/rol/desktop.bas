REM PDFS desktop test: Filer windows on a simulated phone, copy with Filer_Action
log%=0
ON ERROR PROClog("Error: "+REPORT$+" at line "+STR$ERL):END
DIM b% 1024, n% 256
SYS "Wimp_Initialise",310,&4B534154,"PDFS test",0 TO ,me%
SYS "OS_FSControl",37,"@",n%,0,0,255
here$=FNstr(n%)
log%=OPENOUT(here$+".results")
PROClog("Work directory "+here$)
REM OSCLI "PDFSLog trace"
OSCLI "PDFSMount 127.0.0.1:7015"
OSCLI "Filer_OpenDir PDFS::SimPhone-Internal.$ 60 1100 900 320"
PROCwait(250)
OSCLI "Filer_OpenDir PDFS::SimPhone-Internal.$.DCIM.Camera 60 560 900 440"
OSCLI "Filer_OpenDir PDFS::SimPhone-SD_card.$ 1000 1100 600 320"
PROCwait(350)
OSCLI "TBShot"
PROClog("shot 1: phone windows")
REM copy the folder ToPhone into the phone's Documents with Filer_Action
SYS "Wimp_StartTask","Filer_Action" TO fa%
PROClog("Filer_Action task "+STR$~fa%)
PROCmsg(fa%,&403,here$)
PROCmsg(fa%,&404,"ToPhone ")
$(b%+20)=""
b%!20=0:b%!24=1:$(b%+28)="PDFS::SimPhone-Internal.$.Documents":b%?(28+LEN"PDFS::SimPhone-Internal.$.Documents")=0
PROCsend(fa%,&405,28+LEN"PDFS::SimPhone-Internal.$.Documents"+1)
OSCLI "Filer_OpenDir PDFS::SimPhone-Internal.$.Documents 1000 560 600 440"
PROCwait(150)
OSCLI "TBShot"
PROCwait(400)
OSCLI "TBShot"
OSCLI "Filer_OpenDir PDFS::SimPhone-Internal.$.Documents.ToPhone 1000 160 600 300"
PROCwait(300)
OSCLI "TBShot"
PROClog("shot 2: after copying ToPhone")
REM and back: copy a photo from the phone to the work directory
SYS "Wimp_StartTask","Filer_Action" TO fa%
PROCmsg(fa%,&403,"PDFS::SimPhone-Internal.$.DCIM.Camera")
PROCmsg(fa%,&404,"IMG_20261029_0910/jpg ")
b%!20=0:b%!24=0:$(b%+28)=here$:b%?(28+LEN here$)=0
PROCsend(fa%,&405,28+LEN here$+1)
PROCwait(400)
SYS "OS_File",17,here$+".IMG_20261029_0910/jpg" TO t%,,l%,,len%
PROClog("photo copied back: type "+STR$t%+" length "+STR$len%+" filetype "+STR$~((l%>>>8)AND&FFF))
SYS "XOS_File",17,"PDFS::SimPhone-Internal.$.Documents.ToPhone.ReadMe" TO t%,,,,len%
PROClog("ReadMe on phone: type "+STR$t%+" length "+STR$len%)
OSCLI "TBShot"
PROClog("done")
CLOSE#log%
END

DEF PROClog(s$)
IF log% THEN BPUT#log%,s$
ENDPROC

DEF FNstr(p%):LOCAL s$:WHILE ?p%>=32:s$+=CHR$?p%:p%+=1:ENDWHILE:=s$

DEF PROCmsg(t%,a%,s$)
$(b%+20)=s$:b%?(20+LEN s$)=0
PROCsend(t%,a%,20+LEN s$+1)
ENDPROC

DEF PROCsend(t%,a%,len%)
b%!0=(len%+3)AND NOT 3:b%!12=0:b%!16=a%
SYS "Wimp_SendMessage",17,b%,t%
ENDPROC

DEF PROCwait(cs%)
LOCAL e%,r%,now%
SYS "OS_ReadMonotonicTime" TO e%:e%+=cs%
REPEAT
  SYS "Wimp_PollIdle",&1830,b%,e% TO r%
  IF (r%=17 OR r%=18) AND b%!16=0 THEN CLOSE#log%:SYS "Wimp_CloseDown":END
  SYS "OS_ReadMonotonicTime" TO now%
UNTIL now%-e%>=0
ENDPROC
