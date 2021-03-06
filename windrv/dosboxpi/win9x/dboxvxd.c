/* NOTE: Because of our ASM interfacing needs, you must compile this with
 *       GCC, not Open Watcom. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hw/cpu/gccioport.h>

#include <hw/dos/exelehdr.h>
#include <hw/dosboxid/iglib.h>

#include <windows/w9xvmm/dev_vxd_util.h>
#include <windows/w9xvmm/dev_vxd_types.h>

#include <windows/w9xvmm/dev_vxd_ctrl_msg.h>
#include <windows/w9xvmm/dev_vxd_ctrl_msg_95.h>

#include <windows/w9xvmm/dev_vxd_dev_vmm.h>
#include <windows/w9xvmm/dev_vxd_dev_debug.h>
#include <windows/w9xvmm/dev_vxd_dev_vpicd.h>
#include <windows/w9xvmm/dev_vxd_dev_vdmad.h>

void vxd_control_proc(void);

#if 1

uint16_t dosbox_id_baseio = 0x28;

int probe_dosbox_id() {
	uint32_t t;

	if (!dosbox_id_reset()) return 0;

	t = dosbox_id_read_identification();
	if (t != DOSBOX_ID_IDENTIFICATION) return 0;

	return 1;
}

uint32_t dosbox_id_read_regsel() {
	uint32_t r;

	dosbox_id_reset_latch();

#if TARGET_MSDOS == 32
	r  = (uint32_t)inpd(DOSBOX_IDPORT(DOSBOX_ID_INDEX));
#else
	r  = (uint32_t)inpw(DOSBOX_IDPORT(DOSBOX_ID_INDEX));
	r |= (uint32_t)inpw(DOSBOX_IDPORT(DOSBOX_ID_INDEX)) << (uint32_t)16UL;
#endif

	return r;
}

uint32_t dosbox_id_read_data_nrl() {
	uint32_t r;

#if TARGET_MSDOS == 32
	r  = (uint32_t)inpd(DOSBOX_IDPORT(DOSBOX_ID_DATA));
#else
	r  = (uint32_t)inpw(DOSBOX_IDPORT(DOSBOX_ID_DATA));
	r |= (uint32_t)inpw(DOSBOX_IDPORT(DOSBOX_ID_DATA)) << (uint32_t)16UL;
#endif

	return r;
}

uint32_t dosbox_id_read_data() {
	dosbox_id_reset_latch();
	return dosbox_id_read_data_nrl();
}


int dosbox_id_reset() {
	uint32_t t1,t2;

	/* on reset, data should return DOSBOX_ID_RESET_DATA_CODE and index should return DOSBOX_ID_RESET_INDEX_CODE */
	dosbox_id_reset_interface();
	t1 = dosbox_id_read_data();
	t2 = dosbox_id_read_regsel();
	if (t1 != DOSBOX_ID_RESET_DATA_CODE || t2 != DOSBOX_ID_RESET_INDEX_CODE) return 0;
	return 1;
}

uint32_t dosbox_id_read_identification() {
	/* now read the identify register */
	dosbox_id_write_regsel(DOSBOX_ID_REG_IDENTIFY);
	return dosbox_id_read_data();
}

void dosbox_id_write_regsel(const uint32_t reg) {
	dosbox_id_reset_latch();

	outpd(DOSBOX_IDPORT(DOSBOX_ID_INDEX),reg);
}

void dosbox_id_write_data_nrl(const uint32_t val) {
	outpd(DOSBOX_IDPORT(DOSBOX_ID_DATA),val);
}

void dosbox_id_write_data(const uint32_t val) {
	dosbox_id_reset_latch();
	dosbox_id_write_data_nrl(val);
}

#endif

const struct windows_vxd_ddb_win31 DBOXMPI_DDB = {
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
vxd_vm_handle_t System_VM_Handle = 0;
vxd_vm_handle_t Focus_VM_Handle = 0;

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
void my_sys_critical_init(void) {
    const register vxd_vm_handle_t sysvm_handle = VXD_GETEBX();

    System_VM_Handle = sysvm_handle;

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
void my_device_init(void) {
    if (Get_VMM_Version().version < 0x30A)
        goto fail;

    if (!probe_dosbox_id())
        goto fail;

    VXD_CF_SUCCESS();
    return;

fail:
    VXD_CF_FAILURE();
}

/* VxD control message Sys_VM_Init.
 *
 * Entry:
 *   EAX = Sys_VM_Init
 *   EBX = handle of System VM
 *
 * Exit:
 *   Carry flag = clear if success, set if failure */
void my_sys_vm_init(void) {
    const register vxd_vm_handle_t sysvm_handle = VXD_GETEBX();

    System_VM_Handle = sysvm_handle;

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
void my_set_device_focus(void) {
    const register vxd_vm_handle_t vm_handle = VXD_GETEBX();

    if (Focus_VM_Handle != vm_handle) {
        Focus_VM_Handle = vm_handle;

        if (Focus_VM_Handle == System_VM_Handle) {
            dosbox_id_write_regsel(DOSBOX_ID_REG_USER_MOUSE_CURSOR_NORMALIZED);
            dosbox_id_write_data(1); /* PS/2 mouse */
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
void my_sys_vm_terminate(void) {
    if (Focus_VM_Handle != 0) {
        dosbox_id_write_regsel(DOSBOX_ID_REG_USER_MOUSE_CURSOR_NORMALIZED);
        dosbox_id_write_data(0); /* disable */
    }

    System_VM_Handle = 0;
    VXD_CF_SUCCESS();
}

/* WARNING: When compiled with GCC you must use -fomit-frame-pointer to prevent this
 *          dispatch routine from pushing anything on the stack or setting up any
 *          stack frame. */
void vxd_control_proc(void) {
    VXD_CONTROL_DISPATCH(Sys_Critical_Init, my_sys_critical_init);
    VXD_CONTROL_DISPATCH(Sys_VM_Init,       my_sys_vm_init);
    VXD_CONTROL_DISPATCH(Sys_VM_Terminate,  my_sys_vm_terminate);
    VXD_CONTROL_DISPATCH(Set_Device_Focus,  my_set_device_focus);
    VXD_CONTROL_DISPATCH(Device_Init,       my_device_init);
    VXD_CF_SUCCESS();
}

