# this makefile is included from all the dos*.mak files, do not use directly
# NTS: HPS is either \ (DOS) or / (Linux)

CFLAGS_THIS = -fr=nul -fo=$(SUBDIR)$(HPS).obj -i=.. -i..$(HPS)..
NOW_BUILDING = HW_VGA_LIB

C_SOURCE =    vga.c
OBJS =        $(SUBDIR)$(HPS)vga.obj
TEST_EXE =    $(SUBDIR)$(HPS)test.exe

!ifeq TARGET_MSDOS 16
!ifndef TARGET_WINDOWS
VGA240_EXE = $(SUBDIR)$(HPS)vga240.exe
!endif
!endif

$(HW_VGA_LIB): $(OBJS)
	wlib -q -b -c $(HW_VGA_LIB) -+$(SUBDIR)$(HPS)vga.obj

# NTS we have to construct the command line into tmp.cmd because for MS-DOS
# systems all arguments would exceed the pitiful 128 char command line limit
.C.OBJ:
	%write tmp.cmd $(CFLAGS_THIS) $(CFLAGS_CON) $[@
	$(CC) @tmp.cmd

all: lib exe
       
lib: $(HW_VGA_LIB) .symbolic
	
exe: $(TEST_EXE) $(VGA240_EXE) .symbolic

$(TEST_EXE): $(HW_VGA_LIB) $(HW_VGA_LIB_DEPENDENCIES) $(HW_8254_LIB) $(HW_8254_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)test.obj
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) $(HW_VGA_LIB_WLINK_LIBRARIES) $(HW_8254_LIB_WLINK_LIBRARIES) file $(SUBDIR)$(HPS)test.obj name $(TEST_EXE)
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe

!ifdef VGA240_EXE
$(VGA240_EXE): $(HW_VGA_LIB) $(HW_VGA_LIB_DEPENDENCIES) $(SUBDIR)$(HPS)vga240.obj
	%write tmp.cmd option quiet system $(WLINK_CON_SYSTEM) file $(SUBDIR)$(HPS)vga240.obj $(HW_VGA_LIB_WLINK_LIBRARIES) name $(VGA240_EXE)
	@wlink @tmp.cmd
	@$(COPY) ..$(HPS)..$(HPS)dos32a.dat $(SUBDIR)$(HPS)dos4gw.exe
!endif

clean: .SYMBOLIC
          del $(SUBDIR)$(HPS)*.obj
          del $(HW_VGA_LIB)
          del tmp.cmd
          @echo Cleaning done

