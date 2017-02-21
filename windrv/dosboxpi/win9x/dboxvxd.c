
#include <stdint.h>
#include <string.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdio.h>

#include <hw/dos/exelehdr.h>

#include <hw/dosboxid/iglib.h>

void vxd_control_proc(void);

/* USEFUL */
#define VXD_INT3()                  __asm int 3

/* CPU register access */
static uint32_t getEAX();
#pragma aux getEAX = \
    value [eax];

static uint32_t getEBX();
#pragma aux getEBX = \
    value [ebx];

static uint32_t getECX();
#pragma aux getECX = \
    value [ecx];

static uint32_t getEDX();
#pragma aux getEDX = \
    value [edx];

static uint32_t getESI();
#pragma aux getESI = \
    value [esi];

static uint32_t getEDI();
#pragma aux getEDI = \
    value [edi];

/* VxD C function exit macros.
 * these are related to control messages and some calls where success is signalled
 * by clearing the Carry flag (CF=0) and failure is setting the Carry flag (CF=1).
 * These macros work even from within __cdecl functions because the calling convention
 * and prologue/epilogue do not change flags */
#define VXD_CF_SUCCESS()            __asm clc
#define VXD_CF_FAILURE()            __asm stc

/* you must put this at the end of your function if it's declared __declspec(naked) */
#define VXD_RET()                   __asm ret

/* VXD control messages */
#define Sys_Critical_Init           0x0000
#define Device_Init                 0x0001
#define Init_Complete               0x0002
#define Sys_VM_Init                 0x0003
#define Sys_VM_Terminate            0x0004
#define System_Exit                 0x0005
#define Sys_Critical_Exit           0x0006
#define Create_VM                   0x0007
#define VM_Critical_Init            0x0008
#define VM_Init                     0x0009
#define VM_Terminate                0x000A
#define VM_Not_Executeable          0x000B
#define Destroy_VM                  0x000C
#define VM_Suspend                  0x000D
#define VM_Resume                   0x000E
#define Set_Device_Focus            0x000F
#define Begin_Message_Mode          0x0010
#define End_Message_Mode            0x0011
#define Reboot_Processor            0x0012
#define Query_Destroy               0x0013
#define Debug_Query                 0x0014
#define Begin_PM_App                0x0015
#define End_PM_App                  0x0016
#define Device_Reboot_Notify        0x0017
#define Crit_Reboot_Notify          0x0018
#define Close_VM_Notify             0x0019
#define Power_Event                 0x001A

/* VXD device IDs */
#define VMM_Device_ID               0x0001
#define snr_Get_VMM_Version             0x0000
#define snr_Get_Cur_VM_Handle           0x0001
#define snr_Test_Cur_VM_Handle          0x0002
#define snr_Get_Sys_VM_Handle           0x0003
#define snr_Test_Sys_VM_Handle          0x0004
#define snr_Validate_VM_Handle          0x0005
#define Debug_Device_ID             0x0002
#define VPICD_Device_ID             0x0003
#define VDMAD_Device_ID             0x0004

/* VMM services */
#define Fatal_Memory_Error          0x00BF

#define VXD_Control_Dispatch(cmsg,cproc) __asm {    \
    __asm cmp     eax,cmsg                          \
    __asm jz      cproc                             \
}

#define VxDcall(device,service) __asm {             \
    __asm int     20h                               \
    __asm dw      service                           \
    __asm dw      device                            \
}

#define VxDjmp(device,service)  VxDcall(device,service + 0x8000)

#define VMMcall(service)        VxDcall(VMM_Device_ID,service)
#define VMMjmp(service)         VxDjmp(VMM_Device_ID,service)

/* Some VXD calls return error code via CF or result via ZF.
 * Watcom C pragma aux cannot provide those flags as return value.
 * So we instead use 386 instructions SETC and SETZ to fill AL with
 * flag value and return that.
 *
 * If the Open Watcom v2 devs take up my suggestion for new compiler
 * intrinsics then this extra step will no longer be necessary.
 * The instrinsics, if used within an if() statement, would become a single
 * conditional jump statement that tests for a specific CPU flag, thus
 * allowing if/then/else blocks to work with ASM-like CF/ZF return status.
 *
 * The 386 instructions SETcc do not modify FLAGS, therefore these inline
 * asm functions can be repeated for the same result assuming nothing else
 * changes the flags. */
/* NTS: SETC can fill in any register/memory address. It would be nice
 *      if we could do GCC __asm__ style placeholders to let the compiler
 *      choose the CPU register instead of taking AL */
#pragma aux __x86_ZF = \
    "setz al" \
    parm [] \
    value [al] \
    modify exact [al]
uint8_t __x86_ZF(void);

#pragma aux __x86_CF = \
    "setc al" \
    parm [] \
    value [al] \
    modify exact [al]
uint8_t __x86_CF(void);

/* VMM Get_VMM_Version (device=0x0001 service=0x0000)
 * In:
 *   (none)
 * Out:
 *   AH = major version
 *   AL = minor version
 *   ECX = debug revision (ignored here) */
#pragma aux Get_VMM_Version = \
    "int 20h" \
    "dw 0x0000" /* service */ \
    "dw 0x0001" /* device  */ \
    parm [] \
    value [ax] \
    modify exact [eax ecx]
uint16_t Get_VMM_Version(void);

/* VMM Get_Cur_VM_Handle (device=0x0001 service=0x0001)
 * In:
 *   (none)
 * Out:
 *   EBX = VM handle */
#pragma aux Get_Cur_VM_Handle = \
    "int 20h" \
    "dw 0x0001" /* service */ \
    "dw 0x0001" /* device  */ \
    parm [] \
    value [ebx] \
    modify exact []
uint32_t Get_Cur_VM_Handle(void);

/* VMM Test_Cur_VM_Handle (device=0x0001 service=0x0002)
 * In:
 *   EBX = VM handle to test
 * Out:
 *   ZF = set if handle matches currently running VM
 *
 * Since Watcom C offers no way for pragma aux to indicate ZF is return value,
 * you will have to call this function and then from __asm branch based on ZF.
 * This code relies on the compiler's smartness to know what registers are
 * modified inline by __asm to assume that our modification of EBX will be
 * accounted for by register allocation. */
#pragma aux Test_Cur_VM_Handle = \
    "int 20h" \
    "dw 0x0002" /* service */ \
    "dw 0x0001" /* device  */ \
    parm [ebx] \
    modify exact []
void Test_Cur_VM_Handle(const uint32_t vm_handle);

/* VMM Get_Sys_VM_Handle (device=0x0001 service=0x0003)
 * In:
 *   (none)
 * Out:
 *   EBX = VM handle */
#pragma aux Get_Sys_VM_Handle = \
    "int 20h" \
    "dw 0x0003" /* service */ \
    "dw 0x0001" /* device  */ \
    parm [] \
    value [ebx] \
    modify exact []
uint32_t Get_Sys_VM_Handle(void);

/* VMM Test_Sys_VM_Handle (device=0x0001 service=0x0004)
 * In:
 *   EBX = VM handle to test
 * Out:
 *   ZF = set if handle matches system VM
 *
 * Since Watcom C offers no way for pragma aux to indicate ZF is return value,
 * you will have to call this function and then from __asm branch based on ZF.
 * This code relies on the compiler's smartness to know what registers are
 * modified inline by __asm to assume that our modification of EBX will be
 * accounted for by register allocation. */
#pragma aux Test_Sys_VM_Handle = \
    "int 20h" \
    "dw 0x0004" /* service */ \
    "dw 0x0001" /* device  */ \
    parm [ebx] \
    modify exact []
void Test_Sys_VM_Handle(const uint32_t vm_handle);

/* VMM Validate_VM_Handle (device=0x0001 service=0x0005)
 * In:
 *   EBX = VM handle to test
 * Out:
 *   CF = set if not valid
 *
 * Since Watcom C offers no way for pragma aux to indicate CF is return value,
 * you will have to call this function and then from __asm branch based on CF.
 * This code relies on the compiler's smartness to know what registers are
 * modified inline by __asm to assume that our modification of EBX will be
 * accounted for by register allocation. */
#pragma aux Validate_VM_Handle = \
    "int 20h" \
    "dw 0x0005" /* service */ \
    "dw 0x0001" /* device  */ \
    parm [ebx] \
    modify exact []
void Validate_VM_Handle(const uint32_t vm_handle);

/* VMM Get_VMM_Reenter_Count (device=0x0001 service=0x0006)
 * In:
 *   none
 * Out:
 *   ECX = number of times the VMM has been re-entered
 *
 * Use this function in cases where VMM reentrancy may happen
 * (such as page fault, interrupt, exception, etc). If nonzero,
 * you may only call asynchronous VMM services. If you need to
 * call synchronous services you should schedule an event to do
 * it later. */
#pragma aux Get_VMM_Reenter_Count = \
    "int 20h" \
    "dw 0x0006" /* service */ \
    "dw 0x0001" /* device  */ \
    parm [] \
    value [ecx] \
    modify exact [ecx]
uint32_t Get_VMM_Reenter_Count(void);

#define VxD_DATA                __based( __segname("_CODE") )

typedef uint32_t vxd_vm_handle_t;

const struct windows_vxd_ddb_win31 VxD_DATA DBOXMPI_DDB = {
    0,                                                      // +0x00 DDB_Next
    0x030A,                                                 // +0x04 DDB_SDK_Version
    0x0000,                                                 // +0x06 DDB_Req_Device_Number (Undefined_Device_ID)
    1,0,                                                    // +0x08 DDB_Dev_Major_Version, DDB_Dev_Minor_Version
    0x0000,                                                 // +0x0A DDB_Flags
    "DBOXMPI ",                                             // +0x0C DDB_Name
    0x80000000UL,                                           // +0x14 DDB_Init_Order
    (uint32_t)vxd_control_proc,                             // +0x18 DDB_Control_Proc
    0x00000000UL,                                           // +0x1C DDB_V86_API_Proc
    0x00000000UL,                                           // +0x20 DDB_PM_API_Proc
    0x00000000UL,                                           // +0x24 DDB_V86_API_CSIP
    0x00000000UL,                                           // +0x28 DDB_PM_API_CSIP
    0x00000000UL,                                           // +0x2C DDB_Reference_Data
    0x00000000UL,                                           // +0x30 DDB_Service_Table_Ptr
    0x00000000UL                                            // +0x34 DDB_Service_Table_Size
};

/* keep track of the system VM */
vxd_vm_handle_t VxD_DATA System_VM_Handle = 0;
vxd_vm_handle_t VxD_DATA Focus_VM_Handle = 0;

/* try to hack ASM register access into C prototype */
/* VxD control messages appear to have some consistent pattern with their registers, which we represent here.
 * Watcom C won't let us specify a protocol with no return value. But, The Windows DDK samples imply
 * that Windows expects us to trash EAX anyway at least. */
#pragma aux VxDctrlmsgProcEBSDdi "__vxd_ctrlmsg_EBSDdi__*" \
    parm [eax] [ebx] [esi] [edx] [edi] \
    value [eax] \
    modify exact [eax ebx ecx edx esi edi ebp fs gs]

/* VxD control message Sys_Critical_Init.
 *
 * Entry:
 *   EAX = Sys_Critical_Init
 *   EBX = handle of System VM
 *   ESI = Pointer to command tail retrived from PSP of WIN386.EXE
 *
 * Exit:
 *   Carry flag = clear if success, set if failure
 *
 * Notes:
 *   Do not use Simulate_Int or Exec_Int at this stage. */
#pragma aux (VxDctrlmsgProcEBSDdi) my_sys_critical_init /* EAX,EBX,ESI,EDX,EDI */
void my_sys_critical_init(
    /*EAX*/const uint32_t p_msg/*Sys_Critical_Init*/,
    /*EBX*/const uint32_t p_System_VM_Handle,
    /*ESI*/const uint32_t p_win386_psp_command_tail) {
    System_VM_Handle = p_System_VM_Handle;
    VXD_CF_SUCCESS();
}

/* VxD control message Device_Init.
 *
 * Entry:
 *   EAX = Device_Init
 *   EBX = handle of System VM
 *   ESI = Pointer to command tail retrieved from PSP of WIN386.EXE
 *
 * Exit:
 *   Carry flag = clear if success, set if failure */
#pragma aux (VxDctrlmsgProcEBSDdi) my_device_init /* EAX,EBX,ESI,EDX,EDI */
void my_device_init(
    /*EAX*/const uint32_t p_msg/*Device_Init*/,
    /*EBX*/const uint32_t p_System_VM_Handle,
    /*ESI*/const uint32_t p_win386_psp_command_tail) {
    if (!probe_dosbox_id())
        goto fail;

/* do it */
    dosbox_id_write_regsel(DOSBOX_ID_REG_USER_MOUSE_CURSOR_NORMALIZED);
    dosbox_id_write_data(1); /* PS/2 notification */

/* success */
    VXD_CF_SUCCESS();
    return;
fail:
    /* indicate failure. Windows will unload this VxD without complaint and continue on. */
    VXD_CF_FAILURE();
}

/* VxD control message Init_Complete.
 *
 * Entry:
 *   EAX = Init_Complete
 *   EBX = handle of System VM
 *   ESI = Pointer to command tail retrieved from PSP of WIN386.EXE
 *
 * Exit:
 *   Carry flag = clear if success, set if failure
 *
 * Notes:
 *   The system will send this message out just before releasing it's
 *   INIT pages and taking the instance snapshot. */
#pragma aux (VxDctrlmsgProcEBSDdi) my_init_complete /* EAX,EBX,ESI,EDX,EDI */
void my_init_complete(void) {
    /* success */
    VXD_CF_SUCCESS();
}

/* VxD control message Sys_VM_Init.
 *
 * Entry:
 *   EAX = Sys_VM_Init
 *   EBX = handle of System VM
 *
 * Exit:
 *   Carry flag = clear if success, set if failure */
#pragma aux (VxDctrlmsgProcEBSDdi) my_sys_vm_init /* EAX,EBX,ESI,EDX,EDI */
void my_sys_vm_init(void) {
    /* success */
    VXD_CF_SUCCESS();
}

/* VxD control message Set_Device_Focus.
 *
 * Entry:
 *   EAX = Sys_Device_Focus
 *   EBX = Virtual machine handle
 *   ESI = Flags
 *   EDX = Virtual device to recieve focus, or 0 for all
 *   EDI = AssocVM
 *
 * Exit:
 *   CF = 0 */
#pragma aux (VxDctrlmsgProcEBSDdi) my_set_device_focus /* EAX,EBX,ESI,EDX,EDI */
void my_set_device_focus(
    /*EAX*/const uint32_t p_msg/*Device_Init*/,
    /*EBX*/const uint32_t p_VM_Handle,
    /*ESI*/const uint32_t p_Flags,
    /*EDX*/const uint32_t p_virtDevFocus,
    /*EDI*/const uint32_t p_AssocVM) {
    if (Focus_VM_Handle != p_VM_Handle) {
        Focus_VM_Handle = p_VM_Handle;
        if (p_VM_Handle == System_VM_Handle) {
            dosbox_id_write_regsel(DOSBOX_ID_REG_USER_MOUSE_CURSOR_NORMALIZED);
            dosbox_id_write_data(1); /* PS/2 notification */
        }
        else {
            dosbox_id_write_regsel(DOSBOX_ID_REG_USER_MOUSE_CURSOR_NORMALIZED);
            dosbox_id_write_data(0); /* disable */
        }
    }

    VXD_CF_SUCCESS();
}

/* VxD control message Sys_VM_Terminate.
 *
 * Entry:
 *   EAX = Sys_VM_Terminate
 *   EBX = handle of System VM
 *
 * Exit:
 *   Carry flag = clear if success, set if failure */
#pragma aux (VxDctrlmsgProcEBSDdi) my_sys_vm_terminate /* EAX,EBX,ESI,EDX,EDI */
void my_sys_vm_terminate(
    /*EAX*/const uint32_t p_msg/*Sys_VM_Terminate*/,
    /*EBX*/const uint32_t p_System_VM_Handle) {
    if (System_VM_Handle == p_System_VM_Handle) {
        dosbox_id_write_regsel(DOSBOX_ID_REG_USER_MOUSE_CURSOR_NORMALIZED);
        dosbox_id_write_data(0); /* disable */
        System_VM_Handle = 0;
    }

    /* success */
    VXD_CF_SUCCESS();
}

/* NOTED:
 *   Watcom C appears to have a bug where functions declared with __declspec(naked) can make
 *   function calls to non-naked functions, but the compiler will neither throw an error nor
 *   generate the function call. if() statements appear to have the same effect.
 *
 *   To work around this, we declare those functions as __cdecl, which incurs 12 bytes of stack
 *   overhead when the prologue/epilogue saves EBX and two other registers */
/* NOTED:
 *   I just noticed in the Windows 3.1 DDK that Microsoft declares _LDATA (data) and _LTEXT (code)
 *   as both class 'CODE'. That means code and data are combined together. Both are declared as if
 *   readonly executable in the LE header.
 *
 *   In fact, Windows 3.1 seems to ignore VxDs that have non-code LE objects, like all of it must
 *   be declared as if code.
 *
 *   This is Watcom's biggest problem making VxDs, because Watcom wants to declare data in a
 *   separate segment from code and report it as data in the LE header. */
#pragma aux (VxDctrlmsgProcEBSDdi) vxd_control_proc /* EAX,EBX,ESI,EDX,EDI */
void vxd_control_proc(void) {
    VXD_Control_Dispatch(Sys_Critical_Init, my_sys_critical_init);
    VXD_Control_Dispatch(Sys_VM_Terminate, my_sys_vm_terminate);
    VXD_Control_Dispatch(Set_Device_Focus, my_set_device_focus);
    VXD_Control_Dispatch(Init_Complete, my_init_complete);
    VXD_Control_Dispatch(Sys_VM_Init, my_sys_vm_init);
    VXD_Control_Dispatch(Device_Init, my_device_init);
    /* fall through if none of the messages match */
    VXD_CF_SUCCESS();
}

