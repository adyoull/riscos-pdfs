10 SYS "OS_FSControl",49,"PDFS::SimPhone-Internal.$" TO free%,big%,size%
20 PRINT "FSControl 49: free ";free%;" largest ";big%;" size ";size%
30 SYS "OS_FSControl",55,"PDFS::SimPhone-SD_card.$" TO lo%,hi%,big%,slo%,shi%
40 PRINT "FSControl 55: free ";hi%;" ";lo%;" size ";shi%;" ";slo%
