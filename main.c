#include "ksm.h"
#include "dpc.h"
#include "pe.h"

static DEV_EXT g_dev_ext = { NULL, NULL };

DRIVER_INITIALIZE DriverEntry;
#pragma alloc_text(INIT, DriverEntry)

PLIST_ENTRY PsLoadedModuleList;
void *g_kernel_base = NULL;

uintptr_t pxe_base = 0xfffff6fb7dbed000ull;
uintptr_t ppe_base = 0xfffff6fb7da00000ull;
uintptr_t pde_base = 0xfffff6fb40000000ull;
uintptr_t pte_base = 0xfffff68000000000ull;

static NTSTATUS sleep_ms(int ms)
{
	LARGE_INTEGER ival;
	ival.QuadPart = -(10000 * ms);

	return KeDelayExecutionThread(KernelMode, FALSE, &ival);
}

static PVOID hkMmMapIoSpace(_In_ PHYSICAL_ADDRESS    PhysicalAddress,
			    _In_ SIZE_T              NumberOfBytes,
			    _In_ MEMORY_CACHING_TYPE CacheType)
{
	VCPU_DEBUG("Map %p, %d pages with cache %d\n",
		   PhysicalAddress.QuadPart,
		   BYTES_TO_PAGES(NumberOfBytes),
		   CacheType);

	/* Call original  */
	__vmx_vmfunc(EPTP_NORMAL, 0);
	void *ret = MmMapIoSpace(PhysicalAddress, NumberOfBytes, CacheType);
	__vmx_vmfunc(EPTP_EXHOOK, 0);
	return ret;
}

static void DriverUnload(PDRIVER_OBJECT driverObject)
{
	UNREFERENCED_PARAMETER(driverObject);
	deregister_power_callback(&g_dev_ext);
	ksm_unhook_page(MmMapIoSpace);
	VCPU_DEBUG("ret: 0x%08X\n", ksm_exit());
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
	/* On Windows build 14316+ Page table addresses are not static.  */
	RTL_OSVERSIONINFOW osv;
	osv.dwOSVersionInfoSize = sizeof(osv);

	NTSTATUS status = RtlGetVersion(&osv);
	if (!NT_SUCCESS(status))
		return status;

	LDR_DATA_TABLE_ENTRY *entry = driverObject->DriverSection;
	PsLoadedModuleList = entry->InLoadOrderLinks.Flink;
	driverObject->DriverUnload = DriverUnload;

	if (osv.dwMajorVersion >= 10 && osv.dwBuildNumber >= 14316) {
		static const u8 pattern[] = {
			0x48, 0x8b, 0x04, 0xd0,  // mov     rax, [rax+rdx*8]
			0x48, 0xc1, 0xe0, 0x19,  // shl     rax, 19h
			0x48, 0xba,              // mov     rdx, ????????`????????  ; PTE_BASE
		};

		u8 *base = (u8 *)MmGetVirtualForPhysical;
		bool found = false;
		for (size_t i = 0; i <= 0x50 - sizeof(pattern); ++i) {
			if (RtlCompareMemory(pattern, &base[i], sizeof(pattern)) == sizeof(pattern)) {
				pte_base = *(uintptr_t *)(base + i + sizeof(pattern));

				uintptr_t idx = (pte_base >> PXI_SHIFT) & PTX_MASK;
				pde_base = pte_base | (idx << PPI_SHIFT);
				ppe_base = pde_base | (idx << PDI_SHIFT);
				pxe_base = ppe_base | (idx << PTI_SHIFT);
				found = true;
			}
		}

		if (!found)
			return STATUS_NOT_FOUND;

		uintptr_t tmp = (uintptr_t)PAGE_ALIGN((uintptr_t)MmGetVirtualForPhysical);
		VCPU_DEBUG("PXE: %p PPE %p PDE %p PTE %p\n", pxe_base, ppe_base, pde_base, pte_base);
		VCPU_DEBUG("Addr 0x%X 0x%X\n", __pa((uintptr_t *)tmp), va_to_pa(tmp));
	}

	VCPU_DEBUG("We're mapped at %p (size: %d bytes (%d KB), on %d pages)\n",
		   entry->DllBase, entry->SizeOfImage, entry->SizeOfImage / 1024, entry->SizeOfImage / PAGE_SIZE);
	LDR_DATA_TABLE_ENTRY *kentry = container_of(PsLoadedModuleList->Flink, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
	g_kernel_base = kentry->DllBase;

	VCPU_DEBUG("Kernel: %p -> %p (size: 0x%X pages: %d) path: %wS\n",
		   kentry->DllBase, (uintptr_t)kentry->DllBase + kentry->SizeOfImage,
		   kentry->SizeOfImage, BYTES_TO_PAGES(kentry->SizeOfImage),
		   kentry->FullDllName.Buffer);
	ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

	status = ksm_init();
	if (NT_SUCCESS(status))
		status = register_power_callback(&g_dev_ext);

	if (NT_SUCCESS(status))
		status = ksm_hook_epage(MmMapIoSpace, hkMmMapIoSpace);

	VCPU_DEBUG("ret: 0x%08X\n", status);
	return status;
}
