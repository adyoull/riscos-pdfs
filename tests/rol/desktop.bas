REM PDFS desktop test: Filer windows on a simulated phone, copy with Filer_Action
log%=0:icon%=-1:inticon%=-1:maxgap%=0
ON ERROR PROClog("Error: "+REPORT$+" at line "+STR$ERL):END
DIM b% 1024, n% 256
SYS "Wimp_Initialise",310,&4B534154,"PDFS test",0 TO ,me%
SYS "OS_FSControl",37,"@",n%,0,0,255
here$=FNstr(n%)
log%=OPENOUT(here$+".results")
PROClog("Work directory "+here$)
REM OSCLI "PDFSLog trace"
REM load PDFS as double-clicking it does (*RMRun): its filer starts
SYS "Wimp_StartTask","Run "+here$+".PDFS"
PROCwait(100)
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
maxgap%=0
PROCwait(150)
OSCLI "TBShot"
PROCwait(800)
PROClog("longest pause in the desktop during the folder copy: "+STR$maxgap%+"cs")
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
REM which tasks are running?
t%=0
REPEAT
  SYS "XTaskManager_EnumerateTasks",t%,b%+512,256 TO t%,e%
  FOR q%=b%+512 TO e%-16 STEP 16:PROClog("task "+FNstr(q%!4)):NEXT
UNTIL t%<0
REM drop a file on the SD card's icon bar icon
icon%=-1
FOR i%=0 TO 60
  b%!0=-2:b%!4=i%
  SYS "XWimp_GetIconState",,b% TO ;f%
  IF (f% AND 1)=0 THEN
    IF (b%!24 AND &101)=&101 THEN
      IF FNstr(b%!32)="Spdfs_dev" THEN PROClog("icon "+STR$i%+": "+FNstr(b%!28)):IF FNstr(b%!28)="SD card" THEN icon%=i%
      IF FNstr(b%!32)="Spdfs_dev" AND FNstr(b%!28)="Internal" THEN inticon%=i%
    ENDIF
  ENDIF
NEXT
IF icon%>=0 THEN
  b%!20=-2:b%!24=icon%:b%!28=0:b%!32=0:b%!36=18:b%!40=&FFF
  $(b%+44)=here$+".local":b%?(44+LEN(here$+".local"))=0
  PROCsend(-2,3,44+LEN(here$+".local")+1)
  PROCwait(400)
  SYS "XOS_File",17,"PDFS::SimPhone-SD_card.$.local" TO t%,,l%,,len%
  PROClog("dropped on SD card icon: type "+STR$t%+" length "+STR$len%+" filetype "+STR$~((l%>>>8)AND&FFF))
ENDIF
REM a 6MB file: written through a scrap file in <Wimp$ScrapDir>.PDFS
SYS "XOS_CLI","Copy "+here$+".big6 PDFS::SimPhone-Internal.$.Download.big6 ~C~V" TO e%;f%
IF f% AND 1 THEN PROClog("big copy: "+FNstr(e%+4))
SYS "XOS_File",17,"PDFS::SimPhone-Internal.$.Download.big6" TO t%,,,,len%
PROClog("big file on phone: type "+STR$t%+" length "+STR$len%)
n%=0:c%=0
REPEAT
  SYS "XOS_GBPB",9,"<Wimp$ScrapDir>.PDFS",b%+512,8,c%,256,"*" TO ,,,r%,c%;f%
  IF (f% AND 1)=0 THEN n%+=r%
UNTIL c%<0 OR (f% AND 1)
SYS "XOS_File",17,"<Wimp$ScrapDir>.PDFS" TO t%
PROClog("scrap directory type "+STR$t%+", files left: "+STR$n%)
REM one OS_File Save of 12MB, as Filer_Action does for a file that fits its
REM memory: the save returns at once, and the desktop keeps going while it's sent
DIM big% 12*1048576
FOR i%=0 TO 12*1048576-4 STEP 4096:big%!i%=i%:NEXT
SYS "OS_ReadMonotonicTime" TO t0%
SYS "XOS_File",10,"PDFS::SimPhone-Internal.$.Download.save12",&FFD,,big%,big%+12*1048576 TO e%;f%
SYS "OS_ReadMonotonicTime" TO t1%
IF f% AND 1 THEN PROClog("save12: "+FNstr(e%+4))
PROClog("OS_File save of 12MB took "+STR$(t1%-t0%)+"cs")
maxgap%=0
PROCwait(150)
OSCLI "TBShot"
PROCwait(500)
PROClog("longest pause in the desktop while it was sent: "+STR$maxgap%+"cs")
SYS "XOS_File",17,"PDFS::SimPhone-Internal.$.Download.save12" TO t%,,,,len%
PROClog("save12 on phone: type "+STR$t%+" length "+STR$len%)
REM the icon menu's Free entry
SYS "XOS_CLI","ShowFree -fs PDFS SimPhone-Internal" TO e%;f%
IF f% AND 1 THEN PROClog("ShowFree: "+FNstr(e%+4)) ELSE PROClog("ShowFree ok")
PROCwait(200)
OSCLI "TBShot"
REM the icon's menu (MENU on Internal): a screenshot
IF inticon%>=0 THEN
  b%!0=120:b%!4=40:b%!8=2:b%!12=-2:b%!16=inticon%
  SYS "Wimp_SendMessage",6,b%,-2,inticon%
  PROCwait(100)
  OSCLI "TBShot"
  PROClog("shot: icon menu")
  SYS "Wimp_CreateMenu",,-1
ENDIF
REM unplug (the USB emulator's device handle is &9999)
REM (the USB emulator: the output stream's first close fails, as on a Pi)
OSCLI "Set PDFSEmu$CloseFails 1"
SYS "OS_ServiceCall",,&79,&9999,"USB9"
PROCwait(300)
n%=0
FOR i%=0 TO 60
  b%!0=-2:b%!4=i%
  SYS "XWimp_GetIconState",,b% TO ;f%
  IF (f% AND 1)=0 THEN IF (b%!24 AND &101)=&101 THEN IF FNstr(b%!32)="Spdfs_dev" THEN n%+=1
NEXT
PROClog("icons after unplug: "+STR$n%)
SYS "XOS_ReadVarVal","PDFSEmu$Open",b%,64,0,0 TO ,,l%;f%
IF (f% AND 1)=0 THEN b%?l%=13:PROClog("streams open after unplug: "+FNstr(b%))
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
IF t%=-2 THEN SYS "Wimp_SendMessage",17,b%,-2,icon% ELSE SYS "Wimp_SendMessage",17,b%,t%
ENDPROC

DEF PROCwait(cs%)
LOCAL e%,r%,now%,last%
SYS "OS_ReadMonotonicTime" TO e%:last%=e%:e%+=cs%
REPEAT
  SYS "Wimp_PollIdle",&1830,b%,last%+5 TO r%
  IF (r%=17 OR r%=18) AND b%!16=0 THEN CLOSE#log%:SYS "Wimp_CloseDown":END
  SYS "OS_ReadMonotonicTime" TO now%
  IF now%-last%>maxgap% THEN maxgap%=now%-last%
  last%=now%
UNTIL now%-e%>=0
ENDPROC
