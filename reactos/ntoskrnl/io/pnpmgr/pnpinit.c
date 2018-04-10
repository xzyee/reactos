/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/io/pnpmgr/pnpinit.c
 * PURPOSE:         PnP Initialization Code
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

typedef struct _IOPNP_DEVICE_EXTENSION
{
    PWCHAR CompatibleIdList;
    ULONG CompatibleIdListSize;
} IOPNP_DEVICE_EXTENSION, *PIOPNP_DEVICE_EXTENSION;

PUNICODE_STRING PiInitGroupOrderTable;
USHORT PiInitGroupOrderTableCount;
INTERFACE_TYPE PnpDefaultInterfaceType;

/* FUNCTIONS ******************************************************************/

INTERFACE_TYPE
NTAPI
IopDetermineDefaultInterfaceType(VOID)
{
    /* FIXME: ReactOS doesn't support MicroChannel yet */
    return Isa;
}

NTSTATUS
NTAPI
IopInitializeArbiters(VOID)
{
     /* FIXME: TODO */
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
INIT_FUNCTION
PiInitCacheGroupInformation(VOID)
{
    HANDLE KeyHandle;
    NTSTATUS Status;
    PKEY_VALUE_FULL_INFORMATION KeyValueInformation;
    PUNICODE_STRING GroupTable;
    ULONG Count;
    UNICODE_STRING GroupString =
        RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet"
                            L"\\Control\\ServiceGroupOrder");
    
    /* ReactOS HACK for SETUPLDR */
    if (KeLoaderBlock->SetupLdrBlock)
    {
        /* Bogus data */
        PiInitGroupOrderTableCount = 0;
        PiInitGroupOrderTable = (PVOID)0xBABEB00B;
        return STATUS_SUCCESS;
    }
    
    /* Open the registry key */
    Status = IopOpenRegistryKeyEx(&KeyHandle,
                                  NULL,
                                  &GroupString,
                                  KEY_READ);
    if (NT_SUCCESS(Status))
    {
        /* Get the list */
        Status = IopGetRegistryValue(KeyHandle, L"List", &KeyValueInformation);
        ZwClose(KeyHandle);
        
        /* Make sure we got it */
        if (NT_SUCCESS(Status))
        {
            /* Make sure it's valid */
            if ((KeyValueInformation->Type == REG_MULTI_SZ) &&
                (KeyValueInformation->DataLength))
            {
                /* Convert it to unicode strings */
                Status = PnpRegMultiSzToUnicodeStrings(KeyValueInformation,
                                                       &GroupTable,
                                                       &Count);
                
                /* Cache it for later */
                PiInitGroupOrderTable = GroupTable;
                PiInitGroupOrderTableCount = (USHORT)Count;
            }
            else
            {
                /* Fail */
                Status = STATUS_UNSUCCESSFUL;
            }
            
            /* Free the information */
            ExFreePool(KeyValueInformation);
        }
    }
    
    /* Return status */
    return Status;
}

USHORT
NTAPI
PpInitGetGroupOrderIndex(IN HANDLE ServiceHandle)
{
    NTSTATUS Status;
    PKEY_VALUE_FULL_INFORMATION KeyValueInformation;
    USHORT i;
    PVOID Buffer;
    UNICODE_STRING Group;
    PAGED_CODE();
       
    /* Make sure we have a cache */
    if (!PiInitGroupOrderTable) return -1;
    
    /* If we don't have a handle, the rest is easy -- return the count */
    if (!ServiceHandle) return PiInitGroupOrderTableCount + 1;
    
    /* Otherwise, get the group value */
    Status = IopGetRegistryValue(ServiceHandle, L"Group", &KeyValueInformation);
    if (!NT_SUCCESS(Status)) return PiInitGroupOrderTableCount;

    /* Make sure we have a valid string */
    ASSERT(KeyValueInformation->Type == REG_SZ);
    ASSERT(KeyValueInformation->DataLength);
    
    /* Convert to unicode string */
    Buffer = (PVOID)((ULONG_PTR)KeyValueInformation + KeyValueInformation->DataOffset);
    PnpRegSzToString(Buffer, KeyValueInformation->DataLength, &Group.Length);
    Group.MaximumLength = (USHORT)KeyValueInformation->DataLength;
    Group.Buffer = Buffer;
    
    /* Loop the groups */
    for (i = 0; i < PiInitGroupOrderTableCount; i++)
    {
        /* Try to find a match */
        if (RtlEqualUnicodeString(&Group, &PiInitGroupOrderTable[i], TRUE)) break;
    }
    
    /* We're done */
    ExFreePool(KeyValueInformation);
    return i;
}

USHORT
NTAPI
PipGetDriverTagPriority(IN HANDLE ServiceHandle)
{
    NTSTATUS Status;
    HANDLE KeyHandle = NULL;
    PKEY_VALUE_FULL_INFORMATION KeyValueInformation = NULL;
    PKEY_VALUE_FULL_INFORMATION KeyValueInformationTag;
    PKEY_VALUE_FULL_INFORMATION KeyValueInformationGroupOrderList;
    PVOID Buffer;
    UNICODE_STRING Group;
    PULONG GroupOrder;
    ULONG Count, Tag = 0;
    USHORT i = -1;
    UNICODE_STRING GroupString =
    RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet"
                        L"\\Control\\ServiceGroupOrder");
    
    /* Open the key */
    Status = IopOpenRegistryKeyEx(&KeyHandle, NULL, &GroupString, KEY_READ);
    if (!NT_SUCCESS(Status)) goto Quickie;
    
    /* Read the group */
    Status = IopGetRegistryValue(ServiceHandle, L"Group", &KeyValueInformation);
    if (!NT_SUCCESS(Status)) goto Quickie;
    
    /* Make sure we have a group */
    if ((KeyValueInformation->Type == REG_SZ) &&
        (KeyValueInformation->DataLength))
    {
        /* Convert to unicode string */
        Buffer = (PVOID)((ULONG_PTR)KeyValueInformation + KeyValueInformation->DataOffset);
        PnpRegSzToString(Buffer, KeyValueInformation->DataLength, &Group.Length);
        Group.MaximumLength = (USHORT)KeyValueInformation->DataLength;
        Group.Buffer = Buffer;
    }

    /* Now read the tag */
    Status = IopGetRegistryValue(ServiceHandle, L"Tag", &KeyValueInformationTag);
    if (!NT_SUCCESS(Status)) goto Quickie;

    /* Make sure we have a tag */
    if ((KeyValueInformationTag->Type == REG_DWORD) &&
        (KeyValueInformationTag->DataLength))
    {
        /* Read it */
        Tag = *(PULONG)((ULONG_PTR)KeyValueInformationTag +
                        KeyValueInformationTag->DataOffset);
    }
    
    /* We can get rid of this now */
    ExFreePool(KeyValueInformationTag);

    /* Now let's read the group's tag order */
    Status = IopGetRegistryValue(KeyHandle,
                                 Group.Buffer,
                                 &KeyValueInformationGroupOrderList);
    
    /* We can get rid of this now */
Quickie:
    if (KeyValueInformation) ExFreePool(KeyValueInformation);
    if (KeyHandle) NtClose(KeyHandle);
    if (!NT_SUCCESS(Status)) return -1;
    
    /* We're on the success path -- validate the tag order*/
    if ((KeyValueInformationGroupOrderList->Type == REG_BINARY) &&
        (KeyValueInformationGroupOrderList->DataLength))
    {
        /* Get the order array */
        GroupOrder = (PULONG)((ULONG_PTR)KeyValueInformationGroupOrderList +
                              KeyValueInformationGroupOrderList->DataOffset);
        
        /* Get the count */
        Count = *GroupOrder;
        ASSERT(((Count + 1) * sizeof(ULONG)) <=
               KeyValueInformationGroupOrderList->DataLength);
        
        /* Now loop each tag */
        GroupOrder++;
        for (i = 1; i <= Count; i++)
        {
            /* If we found it, we're out */
            if (Tag == *GroupOrder) break;

            /* Try the next one */
            GroupOrder++;
        }
    }
    
    /* Last buffer to free */
    ExFreePool(KeyValueInformationGroupOrderList);
    return i;
}
 
/*
1.打开注册表看看CurrentControlSet\Control\Class\{....}\Property存在不？
2.加载下层filter驱动：IopAttachFilterDrivers
3.加载驱动：IopInitializeDevice，调用驱动的AddDevice函数
4.加载上层filter驱动：IopAttachFilterDrivers
5.启动驱动：IopStartDevice
*/
NTSTATUS
NTAPI
PipCallDriverAddDevice(IN PDEVICE_NODE DeviceNode,
                       IN BOOLEAN LoadDriver,
                       IN PDRIVER_OBJECT DriverObject)
{
    NTSTATUS Status;
    HANDLE EnumRootKey, SubKey, ControlKey, ClassKey, PropertiesKey;
    UNICODE_STRING ClassGuid, Properties;
    UNICODE_STRING EnumRoot = RTL_CONSTANT_STRING(ENUM_ROOT);
    UNICODE_STRING ControlClass =
    RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class");
    PKEY_VALUE_FULL_INFORMATION KeyValueInformation = NULL;
    PWCHAR Buffer;
    
    /* Open enumeration root key */
    
    Status = IopOpenRegistryKeyEx(&EnumRootKey,//输出，获得Enum的key
                                  NULL,
                                  &EnumRoot,//L"\\Registry\\Machine\\System\\CurrentControlSet\\Enum"
                                  KEY_READ);
...
    
    /* Open instance subkey */
    Status = IopOpenRegistryKeyEx(&SubKey, //输出，获得instance的Key，比如：ACPI\PNP0303\0
                                  EnumRootKey,
                                  &DeviceNode->InstancePath,//比如："ACPI\PNP0303\0"
                                  KEY_READ);
    ZwClose(EnumRootKey);
...
    
    /* Get class GUID */
    Status = IopGetRegistryValue(SubKey,
                                 REGSTR_VAL_CLASSGUID,//"ClassGUID"
                                 &KeyValueInformation);
    if (NT_SUCCESS(Status))
    {
        /* Convert to unicode string,取得的$ClassGuid保存在ClassGuid中 */
        Buffer = (PVOID)((ULONG_PTR)KeyValueInformation + KeyValueInformation->DataOffset);
        PnpRegSzToString(Buffer, KeyValueInformation->DataLength, &ClassGuid.Length);
        ClassGuid.MaximumLength = (USHORT)KeyValueInformation->DataLength;
        ClassGuid.Buffer = Buffer;//比如：{4d36e96b-e325-11ce-bfc1-08002be10318}
        
        /* Open the key */
        Status = IopOpenRegistryKeyEx(&ControlKey,//...CurrentControlSet\Control\Class
                                      NULL,
                                      &ControlClass,//
                                      KEY_READ);
        if (!NT_SUCCESS(Status))
        {
            /* No class key */
            DPRINT1("IopOpenRegistryKeyEx() failed with Status %08X\n", Status);
            ClassKey = NULL;
        }
        else
        {
            /* Open the class key */
            Status = IopOpenRegistryKeyEx(&ClassKey,//被打开的子键,比如：{4d36e96b-e325-11ce-bfc1-08002be10318}
                                          ControlKey,//父key=...CurrentControlSet\Control\Class
                                          &ClassGuid,
                                          KEY_READ);
            ZwClose(ControlKey);
            if (!NT_SUCCESS(Status))
            {
                /* No class key */
                DPRINT1("IopOpenRegistryKeyEx() failed with Status %08X\n", Status);
                ClassKey = NULL;
            }
        }
        
        /* Check if we made it till here */
        if (ClassKey) //如果键存在
        {
            /* Get the device properties */
            RtlInitUnicodeString(&Properties, REGSTR_KEY_DEVICE_PROPERTIES/*"Properties"*/);
            Status = IopOpenRegistryKeyEx(&PropertiesKey,
                                          ClassKey,//父key={...Guid...}
                                          &Properties,//"Properties"
                                          KEY_READ);
            ZwClose(ClassKey);
            if (!NT_SUCCESS(Status))
            {
                /* No properties */
                DPRINT("IopOpenRegistryKeyEx() failed with Status %08X\n", Status);
                PropertiesKey = NULL;
            }
            else
            {
                ZwClose(PropertiesKey);
            }
        }
        
        /* Free the registry data */
        ExFreePool(KeyValueInformation);
    }
    
    /* Do ReactOS-style setup */
    Status = IopAttachFilterDrivers(DeviceNode, TRUE);//加载下层过滤驱动
    if (!NT_SUCCESS(Status))
    {
        IopRemoveDevice(DeviceNode);
        return Status;
    }
    Status = IopInitializeDevice(DeviceNode, DriverObject);//加载本驱动
    if (NT_SUCCESS(Status))
    {
        Status = IopAttachFilterDrivers(DeviceNode, FALSE);//加载上层过滤驱动
        if (!NT_SUCCESS(Status))
        {
            IopRemoveDevice(DeviceNode);
            return Status;
        }
            
        Status = IopStartDevice(DeviceNode);
    }
    
    /* Return status */
    return Status;
}

/*
CurrentControlSet已经存在

创建CurrentControlSet\Control
    CurrentControlSet\Control\DeviceClasses

创建CurrentControlSet\Enum
    CurrentControlSet\Enum\Root
    CurrentControlSet\Enum\HTREE\ROOT\0

*/
NTSTATUS
NTAPI
INIT_FUNCTION
IopInitializePlugPlayServices(VOID)
{
    NTSTATUS Status;
    ULONG Disposition;
    HANDLE KeyHandle, EnumHandle, ParentHandle, TreeHandle, ControlHandle;
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet");
    UNICODE_STRING PnpManagerDriverName = RTL_CONSTANT_STRING(DRIVER_ROOT_NAME L"PnpManager");
    PDEVICE_OBJECT Pdo;
    
    /* Initialize locks and such */
    KeInitializeSpinLock(&IopDeviceTreeLock);
        
    /* Get the default interface */
    PnpDefaultInterfaceType = IopDetermineDefaultInterfaceType();
    
    /* Initialize arbiters */
    Status = IopInitializeArbiters();
    if (!NT_SUCCESS(Status)) return Status;
    
    /* Setup the group cache */
    Status = PiInitCacheGroupInformation();
    if (!NT_SUCCESS(Status)) return Status;
    
    /* Open the current control set */
    Status = IopOpenRegistryKeyEx(&KeyHandle,//输出，=..\CurrentControlSet
                                  NULL,
                                  &KeyName,//L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet"
                                  KEY_ALL_ACCESS);
...
    /* Create the control key */
    RtlInitUnicodeString(&KeyName, L"Control");
    Status = IopCreateRegistryKeyEx(&ControlHandle,//输出，=..\CurrentControlSet\Control
                                    KeyHandle,
                                    &KeyName,//L"Control"
                                    KEY_ALL_ACCESS,
                                    REG_OPTION_NON_VOLATILE,
                                    &Disposition);
   ...

    /* Check if it's a new key */
    if (Disposition == REG_CREATED_NEW_KEY)
    {
        HANDLE DeviceClassesHandle;

        /* Create the device classes key */
        //在CurrentControlSet\Control下DeviceClasses
        RtlInitUnicodeString(&KeyName, L"DeviceClasses");
        Status = IopCreateRegistryKeyEx(&DeviceClassesHandle,//输出，=CurrentControlSet\Control\DeviceClasses
                                        ControlHandle,//父=Control
                                        &KeyName,//L"DeviceClasses"
                                        KEY_ALL_ACCESS,
                                        REG_OPTION_NON_VOLATILE,
                                        &Disposition);
        if (!NT_SUCCESS(Status)) return Status;

        ZwClose(DeviceClassesHandle);
    }

    ZwClose(ControlHandle);

    /* Create the enum key */
    //在CurrentControlSet下创建Enum
    RtlInitUnicodeString(&KeyName, REGSTR_KEY_ENUM);//"Enum"
    Status = IopCreateRegistryKeyEx(&EnumHandle,//输出，=CurrentControlSet\Enum
                                    KeyHandle,//父=CurrentControlSet
                                    &KeyName,
                                    KEY_ALL_ACCESS,
                                    REG_OPTION_NON_VOLATILE,
                                    &Disposition);
    if (!NT_SUCCESS(Status)) return Status;
    
    /* Check if it's a new key */
    if (Disposition == REG_CREATED_NEW_KEY)
    {
        /* FIXME: DACLs */
        DPRINT1("Need to build DACL\n");
    }
    
    /* Create the root key */
    ParentHandle = EnumHandle;
    RtlInitUnicodeString(&KeyName, REGSTR_KEY_ROOTENUM);
    Status = IopCreateRegistryKeyEx(&EnumHandle, //输出，=.\CurrentControlSet\Enum\Root
                                    ParentHandle, //父=Enum
                                    &KeyName,//"Root"
                                    KEY_ALL_ACCESS,
                                    REG_OPTION_NON_VOLATILE,
                                    &Disposition);
    NtClose(ParentHandle);
    if (!NT_SUCCESS(Status)) return Status;
    NtClose(EnumHandle);
    
    /* Open the root key now */
    RtlInitUnicodeString(&KeyName, L"\\REGISTRY\\MACHINE\\SYSTEM\\CURRENTCONTROLSET\\ENUM");
    Status = IopOpenRegistryKeyEx(&EnumHandle,
                                  NULL,
                                  &KeyName,
                                  KEY_ALL_ACCESS);
    if (NT_SUCCESS(Status))
    {
        /* Create the root dev node */
        RtlInitUnicodeString(&KeyName, REGSTR_VAL_ROOT_DEVNODE);//"HTREE\\ROOT\\0"
        Status = IopCreateRegistryKeyEx(&TreeHandle,//输出，=CurrentControlSet\Enum\HTREE\ROOT\0
                                        EnumHandle,//父=Enum
                                        &KeyName,//"HTREE\\ROOT\\0"
                                        KEY_ALL_ACCESS,
                                        REG_OPTION_NON_VOLATILE,
                                        NULL);
        NtClose(EnumHandle);
        if (NT_SUCCESS(Status)) NtClose(TreeHandle);
    }

    /* Create the root driver */
    Status = IoCreateDriver(&PnpManagerDriverName, PnpRootDriverEntry);
    //                          L"PnpManager"
...
    
    /* Create the root PDO */
    Status = IoCreateDevice(IopRootDriverObject, //上面一行刚创建的，在PnpRootDriverEntry中被赋值
                            sizeof(IOPNP_DEVICE_EXTENSION),
                            NULL,
                            FILE_DEVICE_CONTROLLER,//控制器?
                            0,
                            FALSE,
                            &Pdo);
...
    
    /* This is a bus enumerated device */
    Pdo->Flags |= DO_BUS_ENUMERATED_DEVICE;//虚拟BUS？
    
    /* Create the root device node */
    IopRootDeviceNode = PipAllocateDeviceNode(Pdo);//第一个DeviceNode
    /* Set flags */
    IopRootDeviceNode->Flags |= DNF_STARTED + DNF_PROCESSED + DNF_ENUMERATED +
                                DNF_MADEUP + DNF_NO_RESOURCE_REQUIRED +
                                DNF_ADDED;
    
    /* Create instance path */
    RtlCreateUnicodeString(&IopRootDeviceNode->InstancePath,
                           REGSTR_VAL_ROOT_DEVNODE); //"HTREE\\ROOT\\0"
    
    /* Call the add device routine */
    IopRootDriverObject->DriverExtension->AddDevice(IopRootDriverObject,
                                                    IopRootDeviceNode->PhysicalDeviceObject);

    /* Initialize PnP-Event notification support */
    Status = IopInitPlugPlayEvents();
    ...
    
    /* Report the device to the user-mode pnp manager */
    IopQueueTargetDeviceEvent(&GUID_DEVICE_ARRIVAL,
                              &IopRootDeviceNode->InstancePath);
    
    /* Initialize the Bus Type GUID List */
    PnpBusTypeGuidList = ExAllocatePool(PagedPool, sizeof(IO_BUS_TYPE_GUID_LIST));
    RtlZeroMemory(PnpBusTypeGuidList, sizeof(IO_BUS_TYPE_GUID_LIST));
    ExInitializeFastMutex(&PnpBusTypeGuidList->Lock);
    
    /* Launch the firmware mapper */
    Status = IopUpdateRootKey();
    ...
    
    /* Close the handle to the control set */
    NtClose(KeyHandle);
    
    /* We made it */
    return STATUS_SUCCESS;
}

/* EOF */
