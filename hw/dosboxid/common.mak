# this makefile is included from all the dos*.mak files, do not use directly
# NTS: HPS is either \ (DOS) or / (Linux)
NOW_BUILDING = HW_DOSBOXID_LIB
CFLAGS_THIS = -fr=nul -fo=$(SUBDIR)$(HPS).obj -i=.. -i..$(HPS)..
CFLAGS_THIS_VXD = -fo=$(SUBDIR)_vxd$(HPS).obj -s -zl -zc -nt=_TEXT -e=2 -zq -zw -mf -oilrtfm -wx -fp3 -3r -dTARGET_MSDOS=32 -dTARGET_WINDOWS=32 -dTARGET86=386 -DMMODE=f -q -bc -zl -zdp -dTARGET_VXD=1
CFLAGS_THIS_DRV = -fo=$(SUBDIR)_drv$(HPS).obj -s -zl -zc -nt=_TEXT
CFLAGS_THIS_DRVN = -fo=$(SUBDIR)_drvn$(HPS).obj -s -zl -zc -nt=_TEXT
CFLAGS_THIS_DRV_ND = -fo=$(SUBDIR)_drv$(HPS).obj -s -zl -zc -nt=_NDTEXT -nc=NDCODE

C_SOURCE =    iglib.c
OBJS =        $(SUBDIR)$(HPS)iglib.obj $(SUBDIR)$(HPS)igregio.obj $(SUBDIR)$(HPS)igrselio.obj $(SUBDIR)$(HPS)igprobe.obj $(SUBDIR)$(HPS)igreset.obj $(SUBDIR)$(HPS)igrident.obj $(SUBDIR)$(HPS)igverstr.obj $(SUBDIR)$(HPS)igdbgmsg.obj
MCR_EXE =     $(SUBDIR)$(HPS)mcr.$(EXEEXT)
UMC_EXE =     $(SUBDIR)$(HPS)umc.$(EXEEXT)
UMCN_EXE =    $(SUBDIR)$(HPS)umcn.$(EXEEXT)
TEST_EXE =    $(SUBDIR)$(HPS)test.$(EXEEXT)
SSHOT_EXE =   $(SUBDIR)$(HPS)sshot.$(EXEEXT)
VCAP_EXE =    $(SUBDIR)$(HPS)vcap.$(EXEEXT)
WCAP_EXE =    $(SUBDIR)$(HPS)wcap.$(EXEEXT)
KBSTAT_EXE =  $(SUBDIR)$(HPS)kbstat.$(EXEEXT)
KBINJECT_EXE =$(SUBDIR)$(HPS)kbinject.$(EXEEXT)
MSINJECT_EXE =$(SUBDIR)$(HPS)msinject.$(EXEEXT)

$(HW_DOSBOXID_LIB): $(OBJS)
	wlib -q -b -c $(HW_DOSBOXID_LIB) -+$(SUBDIR)$(HPS)iglib.obj    -+$(SUBDIR)$(HPS)igregio.obj  -+$(SUBDIR)$(HPS)igrselio.obj
	wlib -q -b -c $(HW_DOSBOXID_LIB) -+$(SUBDIR)$(HPS)igprobe.obj  -+$(SUBDIR)$(HPS)igreset.obj  -+$(SUBDIR)$(HPS)igrident.obj
	wlib -q -b -c $(HW_DOSBOXID_LIB) -+$(SUBDIR)$(HPS)igverstr.obj -+$(SUBDIR)$(HPS)igdbgmsg.obj

!ifdef HW_DOSBOXID_LIB_VXD
$(HW_DOSBOXID_LIB_VXD): $(OBJS)
	wlib -q -b -c $(HW_DOSBOXID_LIB_VXD) -+$(SUBDIR)_vxd$(HPS)iglib.obj    -+$(SUBDIR)_vxd$(HPS)igregio.obj  -+$(SUBDIR)_vxd$(HPS)igrselio.obj
	wlib -q -b -c $(HW_DOSBOXID_LIB_VXD) -+$(SUBDIR)_vxd$(HPS)igprobe.obj  -+$(SUBDIR)_vxd$(HPS)igreset.obj  -+$(SUBDIR)_vxd$(HPS)igrident.obj
	wlib -q -b -c $(HW_DOSBOXID_LIB_VXD) -+$(SUBDIR)_vxd$(HPS)igverstr.obj -+$(SUBDIR)_vxd$(HPS)igdbgmsg.obj
!endif

!ifdef HW_DOSBOXID_LIB_DRV
$(HW_DOSBOXID_LIB_DRV): $(OBJS)
	wlib -q -b -c $(HW_DOSBOXID_LIB_DRV) -+$(SUBDIR)_drv$(HPS)iglib.obj    -+$(SUBDIR)_drv$(HPS)igregio.obj  -+$(SUBDIR)_drv$(HPS)igrselio.obj
	wlib -q -b -c $(HW_DOSBOXID_LIB_DRV) -+$(SUBDIR)_drv$(HPS)igprobe.obj  -+$(SUBDIR)_drv$(HPS)igreset.obj  -+$(SUBDIR)_drv$(HPS)igrident.obj
	wlib -q -b -c $(HW_DOSBOXID_LIB_DRV) -+$(SUBDIR)_drv$(HPS)igverstr.obj -+$(SUBDIR)_drv$(HPS)igdbgmsg.obj
!endif

!ifdef HW_DOSBOXID_LIB_DRVN
$(HW_DOSBOXID_LIB_DRVN): $(OBJS)
	wlib -q -b -c $(HW_DOSBOXID_LIB_DRVN) -+$(SUBDIR)_drvn$(HPS)iglib.obj    -+$(SUBDIR)_drvn$(HPS)igregio.obj  -+$(SUBDIR)_drvn$(HPS)igrselio.obj
	wlib -q -b -c $(HW_DOSBOXID_LIB_DRVN) -+$(SUBDIR)_drvn$(HPS)igprobe.obj  -+$(SUBDIR)_drvn$(HPS)igreset.obj  -+$(SUBDIR)_drvn$(HPS)igrident.obj
	wlib -q -b -c $(HW_DOSBOXID_LIB_DRVN) -+$(SUBDIR)_drvn$(HPS)igverstr.obj -+$(SUBDIR)_drvn$(HPS)igdbgmsg.obj
!endif

# NTS we have to construct the command line into tmp.cmd because for MS-DOS
# systems all arguments would exceed the pitiful 128 char command line limit
.C.OBJ:
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS) $[@
	@$(CC) @tmp.cmd
!ifdef HW_DOSBOXID_LIB_VXD
	mkdir -p $(SUBDIR)_vxd
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_THIS_VXD) $[@
	@$(CC) @tmp.cmd
!endif
!ifdef HW_DOSBOXID_LIB_DRV
	mkdir -p $(SUBDIR)_drv
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS) $(CFLAGS_THIS_DRV) $[@
	@$(CC) @tmp.cmd
!endif
!ifdef HW_DOSBOXID_LIB_DRVN
	mkdir -p $(SUBDIR)_drvn
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS) $(CFLAGS_THIS_DRVN) $[@
	@$(CC) @tmp.cmd
!endif

$(SUBDIR)$(HPS)igregio.obj: igregio.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS) $[@
	@$(CC) @tmp.cmd
!ifdef HW_DOSBOXID_LIB_VXD
	mkdir -p $(SUBDIR)_vxd
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_THIS_VXD) $[@
	@$(CC) @tmp.cmd
!endif
!ifdef HW_DOSBOXID_LIB_DRV
	mkdir -p $(SUBDIR)_drv
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS) $(CFLAGS_THIS_DRV_ND) $[@
	@$(CC) @tmp.cmd
!endif
!ifdef HW_DOSBOXID_LIB_DRVN
	mkdir -p $(SUBDIR)_drvn
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS) $(CFLAGS_THIS_DRVN) $[@
	@$(CC) @tmp.cmd
!endif

$(SUBDIR)$(HPS)igrselio.obj: igrselio.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS) $[@
	@$(CC) @tmp.cmd
!ifdef HW_DOSBOXID_LIB_VXD
	mkdir -p $(SUBDIR)_vxd
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_THIS_VXD) $[@
	@$(CC) @tmp.cmd
!endif
!ifdef HW_DOSBOXID_LIB_DRV
	mkdir -p $(SUBDIR)_drv
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS) $(CFLAGS_THIS_DRV_ND) $[@
	@$(CC) @tmp.cmd
!endif
!ifdef HW_DOSBOXID_LIB_DRVN
	mkdir -p $(SUBDIR)_drvn
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS) $(CFLAGS_THIS_DRVN) $[@
	@$(CC) @tmp.cmd
!endif

$(SUBDIR)$(HPS)msinject.obj: msinject.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	@$(CC) @tmp.cmd

$(SUBDIR)$(HPS)kbinject.obj: kbinject.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	@$(CC) @tmp.cmd

$(SUBDIR)$(HPS)kbstat.obj: kbstat.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	@$(CC) @tmp.cmd

$(SUBDIR)$(HPS)sshot.obj: sshot.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	@$(CC) @tmp.cmd

$(SUBDIR)$(HPS)vcap.obj: vcap.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	@$(CC) @tmp.cmd

$(SUBDIR)$(HPS)wcap.obj: wcap.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	@$(CC) @tmp.cmd

$(SUBDIR)$(HPS)test.obj: test.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	@$(CC) @tmp.cmd

$(SUBDIR)$(HPS)umcn.obj: umcn.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	@$(CC) @tmp.cmd

$(SUBDIR)$(HPS)umc.obj: umc.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	@$(CC) @tmp.cmd

$(SUBDIR)$(HPS)mcr.obj: mcr.c
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	@$(CC) @tmp.cmd

all: $(OMFSEGDG) lib exe

lib: $(HW_DOSBOXID_LIB) $(HW_DOSBOXID_LIB_VXD) $(HW_DOSBOXID_LIB_DRV) $(HW_DOSBOXID_LIB_DRVN) .symbolic

exe: $(TEST_EXE) $(MCR_EXE) $(UMC_EXE) $(UMCN_EXE) $(SSHOT_EXE) $(VCAP_EXE) $(WCAP_EXE) $(KBSTAT_EXE) $(KBINJECT_EXE) $(MSINJECT_EXE) .symbolic

!ifdef UMC_EXE
$(UMC_EXE): $(HW_DOSBOXID_LIB) $(HW_DOSBOXID_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)umc.obj $(HW_8254_LIB) $(HW_8254_LIB_DEPENDENCIES) $(HW_DOS_LIB) $(HW_DOS_LIB_DEPENDENCIES)
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) file $(SUBDIR)$(HPS)umc.obj $(HW_DOSBOXID_LIB_WLINK_LIBRARIES) $(HW_8254_LIB_WLINK_LIBRARIES) $(HW_DOS_LIB_WLINK_LIBRARIES) name $(UMC_EXE) option map=$(UMC_EXE).map
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

!ifdef UMCN_EXE
$(UMCN_EXE): $(HW_DOSBOXID_LIB) $(HW_DOSBOXID_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)umcn.obj $(HW_8254_LIB) $(HW_8254_LIB_DEPENDENCIES) $(HW_DOS_LIB) $(HW_DOS_LIB_DEPENDENCIES)
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) file $(SUBDIR)$(HPS)umcn.obj $(HW_DOSBOXID_LIB_WLINK_LIBRARIES) $(HW_8254_LIB_WLINK_LIBRARIES) $(HW_DOS_LIB_WLINK_LIBRARIES) name $(UMCN_EXE) option map=$(UMCN_EXE).map
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

!ifdef MCR_EXE
$(MCR_EXE): $(HW_DOSBOXID_LIB) $(HW_DOSBOXID_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)mcr.obj $(HW_8254_LIB) $(HW_8254_LIB_DEPENDENCIES) $(HW_DOS_LIB) $(HW_DOS_LIB_DEPENDENCIES)
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) file $(SUBDIR)$(HPS)mcr.obj $(HW_DOSBOXID_LIB_WLINK_LIBRARIES) $(HW_8254_LIB_WLINK_LIBRARIES) $(HW_DOS_LIB_WLINK_LIBRARIES) name $(MCR_EXE) option map=$(MCR_EXE).map
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

!ifdef TEST_EXE
$(TEST_EXE): $(HW_DOSBOXID_LIB) $(HW_DOSBOXID_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)test.obj $(HW_8254_LIB) $(HW_8254_LIB_DEPENDENCIES) $(HW_DOS_LIB) $(HW_DOS_LIB_DEPENDENCIES)
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) file $(SUBDIR)$(HPS)test.obj $(HW_DOSBOXID_LIB_WLINK_LIBRARIES) $(HW_8254_LIB_WLINK_LIBRARIES) $(HW_DOS_LIB_WLINK_LIBRARIES) name $(TEST_EXE) option map=$(TEST_EXE).map
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

!ifdef SSHOT_EXE
$(SSHOT_EXE): $(HW_DOSBOXID_LIB) $(HW_DOSBOXID_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)sshot.obj $(HW_8254_LIB) $(HW_8254_LIB_DEPENDENCIES) $(HW_DOS_LIB) $(HW_DOS_LIB_DEPENDENCIES)
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) file $(SUBDIR)$(HPS)sshot.obj $(HW_DOSBOXID_LIB_WLINK_LIBRARIES) $(HW_8254_LIB_WLINK_LIBRARIES) $(HW_DOS_LIB_WLINK_LIBRARIES) name $(SSHOT_EXE) option map=$(SSHOT_EXE).map
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

!ifdef VCAP_EXE
$(VCAP_EXE): $(HW_DOSBOXID_LIB) $(HW_DOSBOXID_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)vcap.obj $(HW_8254_LIB) $(HW_8254_LIB_DEPENDENCIES) $(HW_DOS_LIB) $(HW_DOS_LIB_DEPENDENCIES)
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) file $(SUBDIR)$(HPS)vcap.obj $(HW_DOSBOXID_LIB_WLINK_LIBRARIES) $(HW_8254_LIB_WLINK_LIBRARIES) $(HW_DOS_LIB_WLINK_LIBRARIES) name $(VCAP_EXE) option map=$(VCAP_EXE).map
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

!ifdef WCAP_EXE
$(WCAP_EXE): $(HW_DOSBOXID_LIB) $(HW_DOSBOXID_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)wcap.obj $(HW_8254_LIB) $(HW_8254_LIB_DEPENDENCIES) $(HW_DOS_LIB) $(HW_DOS_LIB_DEPENDENCIES)
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) file $(SUBDIR)$(HPS)wcap.obj $(HW_DOSBOXID_LIB_WLINK_LIBRARIES) $(HW_8254_LIB_WLINK_LIBRARIES) $(HW_DOS_LIB_WLINK_LIBRARIES) name $(WCAP_EXE) option map=$(WCAP_EXE).map
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

!ifdef KBSTAT_EXE
$(KBSTAT_EXE): $(HW_DOSBOXID_LIB) $(HW_DOSBOXID_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)kbstat.obj $(HW_8254_LIB) $(HW_8254_LIB_DEPENDENCIES) $(HW_DOS_LIB) $(HW_DOS_LIB_DEPENDENCIES)
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) file $(SUBDIR)$(HPS)kbstat.obj $(HW_DOSBOXID_LIB_WLINK_LIBRARIES) $(HW_8254_LIB_WLINK_LIBRARIES) $(HW_DOS_LIB_WLINK_LIBRARIES) name $(KBSTAT_EXE) option map=$(KBSTAT_EXE).map
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

!ifdef KBINJECT_EXE
$(KBINJECT_EXE): $(HW_DOSBOXID_LIB) $(HW_DOSBOXID_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)kbinject.obj $(HW_8254_LIB) $(HW_8254_LIB_DEPENDENCIES) $(HW_DOS_LIB) $(HW_DOS_LIB_DEPENDENCIES)
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) file $(SUBDIR)$(HPS)kbinject.obj $(HW_DOSBOXID_LIB_WLINK_LIBRARIES) $(HW_8254_LIB_WLINK_LIBRARIES) $(HW_DOS_LIB_WLINK_LIBRARIES) name $(KBINJECT_EXE) option map=$(KBINJECT_EXE).map
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

!ifdef MSINJECT_EXE
$(MSINJECT_EXE): $(HW_DOSBOXID_LIB) $(HW_DOSBOXID_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)msinject.obj $(HW_8254_LIB) $(HW_8254_LIB_DEPENDENCIES) $(HW_DOS_LIB) $(HW_DOS_LIB_DEPENDENCIES)
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) file $(SUBDIR)$(HPS)msinject.obj $(HW_DOSBOXID_LIB_WLINK_LIBRARIES) $(HW_8254_LIB_WLINK_LIBRARIES) $(HW_DOS_LIB_WLINK_LIBRARIES) name $(MSINJECT_EXE) option map=$(MSINJECT_EXE).map
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

clean: .SYMBOLIC
          del $(SUBDIR)$(HPS)*.obj
          del $(HW_DOSBOXID_LIB)
          del tmp.cmd
          @echo Cleaning done

