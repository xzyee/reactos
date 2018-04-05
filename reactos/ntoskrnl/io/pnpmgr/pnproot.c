/*
 * PROJECT:         ReactOS Kernel
 * COPYRIGHT:       GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/io/pnpmgr/pnproot.c
 * PURPOSE:         PnP manager root device
 * PROGRAMMERS:     Casper S. Hornstrup (chorns@users.sourceforge.net)
 *                  Copyright 2007 Herv? Poussineau (hpoussin@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

#define ENUM_NAME_ROOT L"Root"

/* DATA **********************************************************************/

typedef struct _PNPROOT_DEVICE
{
    // Entry on device list
    LIST_ENTRY ListEntry;
    // Physical Device Object of device
    PDEVICE_OBJECT Pdo;
    // Device ID
    UNICODE_STRING DeviceID;
    // Instance ID
    UNICODE_STRING InstanceID;
    // Device description
    UNICODE_STRING DeviceDescription;
    // Resource requirement list
    PIO_RESOURCE_REQUIREMENTS_LIST ResourceRequirementsList;
    // Associated resource list
    PCM_RESOURCE_LIST ResourceList;
    ULONG ResourceListSize;
} PNPROOT_DEVICE, *PPNPROOT_DEVICE;

typedef enum
{
    dsStopped,
    dsStarted,
    dsPaused,
    dsRemoved,
    dsSurpriseRemoved
} PNPROOT_DEVICE_STATE;

typedef struct _PNPROOT_COMMON_DEVICE_EXTENSION
{
    // Wether this device extension is for an FDO or PDO
    BOOLEAN IsFDO;
} PNPROOT_COMMON_DEVICE_EXTENSION, *PPNPROOT_COMMON_DEVICE_EXTENSION;

/* Physical Device Object device extension for a child device */
typedef struct _PNPROOT_PDO_DEVICE_EXTENSION
{
    // Common device data
    PNPROOT_COMMON_DEVICE_EXTENSION Common;
    // Informations about the device
    PPNPROOT_DEVICE DeviceInfo;
} PNPROOT_PDO_DEVICE_EXTENSION, *PPNPROOT_PDO_DEVICE_EXTENSION;

/* Physical Device Object device extension for the Root bus device object */
typedef struct _PNPROOT_FDO_DEVICE_EXTENSION
{
    // Common device data
    PNPROOT_COMMON_DEVICE_EXTENSION Common;
    // Lower device object
    PDEVICE_OBJECT Ldo;
    // Current state of the driver
    PNPROOT_DEVICE_STATE State;
    // Namespace device list
    LIST_ENTRY DeviceListHead;
    // Number of (not removed) devices in device list
    ULONG DeviceListCount;
    // Lock for namespace device list
    KGUARDED_MUTEX DeviceListLock;
} PNPROOT_FDO_DEVICE_EXTENSION, *PPNPROOT_FDO_DEVICE_EXTENSION;

typedef struct _BUFFER
{
    PVOID *Data;
    PULONG Length;
} BUFFER, *PBUFFER;

static PDEVICE_OBJECT PnpRootDeviceObject = NULL;

/* FUNCTIONS *****************************************************************/

//在PnpRootDeviceObject->DeviceExtension的串串上找PNPROOT_DEVICE，
//注意PNPROOT_DEVICE不是DEVICE_OBJECT，两者两码事
static NTSTATUS
LocateChildDevice(
    IN PPNPROOT_FDO_DEVICE_EXTENSION DeviceExtension,
    IN PCWSTR DeviceId,
    IN PCWSTR InstanceId,
    OUT PPNPROOT_DEVICE* ChildDevice)
{
    PPNPROOT_DEVICE Device;
    UNICODE_STRING DeviceIdU, InstanceIdU;
    PLIST_ENTRY NextEntry;

    /* Initialize the strings to compare  */
    RtlInitUnicodeString(&DeviceIdU, DeviceId);
    RtlInitUnicodeString(&InstanceIdU, InstanceId);

    /* Start looping */
    for (NextEntry = DeviceExtension->DeviceListHead.Flink;
         NextEntry != &DeviceExtension->DeviceListHead;
         NextEntry = NextEntry->Flink)
    {
        /* Get the entry */
        Device = CONTAINING_RECORD(NextEntry, PNPROOT_DEVICE, ListEntry);

        /* See if the strings match */
        if (RtlEqualUnicodeString(&DeviceIdU, &Device->DeviceID, TRUE) &&
            RtlEqualUnicodeString(&InstanceIdU, &Device->InstanceID, TRUE))
        {
            /* They do, so set the pointer and return success */
            *ChildDevice = Device;
            return STATUS_SUCCESS;
        }
    }

    /* No device found */
    return STATUS_NO_SUCH_DEVICE;
}
/* 
(1)创建一个PNPROOT_DEVICE结构,并适当初始化
typedef struct _PNPROOT_DEVICE
{
    LIST_ENTRY ListEntry; //腰带
 
    PDEVICE_OBJECT Pdo; //输入参数DeviceObject

    UNICODE_STRING DeviceID; 比如"ROOT\LEGACY_VMBUS"

    UNICODE_STRING InstanceID; 比如"0000"
    //以下未被初始化
    UNICODE_STRING DeviceDescription;
    PIO_RESOURCE_REQUIREMENTS_LIST ResourceRequirementsList;
    PCM_RESOURCE_LIST ResourceList;
    ULONG ResourceListSize;
} PNPROOT_DEVICE, *PPNPROOT_DEVICE;
 
(2)然后挂到PnpRootDeviceObject->DeviceExtension的串串上
注意以DeviceNode为纽带，DeviceObject->DeviceNode->InstancePath->设备ID和实例ID
*/

NTSTATUS
PnpRootRegisterDevice(
    IN PDEVICE_OBJECT DeviceObject //PDO
    )
{
    PPNPROOT_FDO_DEVICE_EXTENSION DeviceExtension = PnpRootDeviceObject->DeviceExtension;
    PPNPROOT_DEVICE Device;
    PDEVICE_NODE DeviceNode;//从中找到InstancePath信息
    PWSTR InstancePath;
    UNICODE_STRING InstancePathCopy;

    Device = ExAllocatePoolWithTag(PagedPool, sizeof(PNPROOT_DEVICE), TAG_PNP_ROOT);
    if (!Device) return STATUS_NO_MEMORY;

    DeviceNode = IopGetDeviceNode(DeviceObject);
    if (!RtlCreateUnicodeString(&InstancePathCopy, DeviceNode->InstancePath.Buffer))
    {
        ExFreePoolWithTag(Device, TAG_PNP_ROOT);
        return STATUS_NO_MEMORY;
    }

    //InstancePath = "$DeviceID\$InstanceID"
    InstancePath = wcsrchr(InstancePathCopy.Buffer, L'\\');
    ASSERT(InstancePath);

    if (!RtlCreateUnicodeString(&Device->InstanceID, InstancePath + 1))
    {
        RtlFreeUnicodeString(&InstancePathCopy);
        ExFreePoolWithTag(Device, TAG_PNP_ROOT);
        return STATUS_NO_MEMORY;
    }

    InstancePath[0] = UNICODE_NULL; //相当于去掉InstanceID部分，只保留DeviceID部分
    if (!RtlCreateUnicodeString(&Device->DeviceID, InstancePathCopy.Buffer))
    {
        RtlFreeUnicodeString(&InstancePathCopy);
        RtlFreeUnicodeString(&Device->InstanceID);
        ExFreePoolWithTag(Device, TAG_PNP_ROOT);
        return STATUS_NO_MEMORY;
    }

    InstancePath[0] = L'\\';//还原
 
    Device->Pdo = DeviceObject;

    //串在全局唯一FDO的设备扩展（PNPROOT_FDO_DEVICE_EXTENSION）里的那个串串上
    KeAcquireGuardedMutex(&DeviceExtension->DeviceListLock);
    InsertTailList(&DeviceExtension->DeviceListHead,
                   &Device->ListEntry);
    DeviceExtension->DeviceListCount++;
    KeReleaseGuardedMutex(&DeviceExtension->DeviceListLock);

    RtlFreeUnicodeString(&InstancePathCopy);

    return STATUS_SUCCESS;
}

/* Creates a new PnP device for a legacy driver 
(1)在\\Registry\\Machine\\System\\CurrentControlSet\\Enum下面创建root\ServiceName\0000
(2)创建PNPROOT_DEVICE结构，被串在静态指针PnpRootDeviceObject的设备扩展PNPROOT_FDO_DEVICE_EXTENSION上
(3)创建pdo，其设备扩展为PNPROOT_PDO_DEVICE_EXTENSION
对上面(2)(3)的理解：PnpRootDeviceObject为FDO，全局只有一个，而创建的pdo为PDO，并且有很多，被串联起来，这就容易理解了
DevicePath = "ROOT\LEGAY_VMBUS"
DeviceID = "Root\ServiceName"
FullInstancePath ="ROOT\LEGAY_VMBUS\0000"
pPNPROOTDevice->DeviceID = "Root\ServiceName"
pPNPROOTDevice->InstanceID = "0000"
*/
NTSTATUS
PnpRootCreateDevice(
    IN PUNICODE_STRING ServiceName,//比如："LEGAY_VMBUS"
    IN OPTIONAL PDRIVER_OBJECT DriverObject,//如果为NULL，则用PnpRootDeviceObject->DriverObject代替
    OUT PDEVICE_OBJECT *PhysicalDeviceObject,//输出
    OUT OPTIONAL PUNICODE_STRING FullInstancePath)//输出，比如："ROOT\LEGAY_VMBUS\0000"
{
    PPNPROOT_FDO_DEVICE_EXTENSION DeviceExtension;
    PPNPROOT_PDO_DEVICE_EXTENSION PdoDeviceExtension;
    WCHAR DevicePath[MAX_PATH + 1];
    WCHAR InstancePath[5];
    PPNPROOT_DEVICE Device = NULL;
    NTSTATUS Status;
    UNICODE_STRING PathSep = RTL_CONSTANT_STRING(L"\\");
    ULONG NextInstance;
    UNICODE_STRING EnumKeyName = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\" REGSTR_PATH_SYSTEMENUM);
    //                                                                                   |
    //                                                                      "System\\CurrentControlSet\\Enum"
    //
    //EnumKeyName = L"\\Registry\\Machine\\System\\CurrentControlSet\\Enum"
    
    HANDLE EnumHandle, DeviceKeyHandle = INVALID_HANDLE_VALUE, InstanceKeyHandle;
    RTL_QUERY_REGISTRY_TABLE QueryTable[2];
    OBJECT_ATTRIBUTES ObjectAttributes;

    DeviceExtension = PnpRootDeviceObject->DeviceExtension;
    //                           |
    //                 静态PDEVICE_OBJECT指针
    KeAcquireGuardedMutex(&DeviceExtension->DeviceListLock);

    DPRINT("Creating a PnP root device for service '%wZ'\n", ServiceName);

    _snwprintf(DevicePath, sizeof(DevicePath) / sizeof(WCHAR), L"%s\\%wZ", REGSTR_KEY_ROOTENUM, ServiceName);//DevicePath="Root\ServiceName"

    /* 创建PNPROOT_DEVICE结构，只有其DeviceID被确定为"Root\ServiceName"*/
    Device = ExAllocatePoolWithTag(PagedPool, sizeof(PNPROOT_DEVICE), TAG_PNP_ROOT);
...
    RtlZeroMemory(Device, sizeof(PNPROOT_DEVICE));
    RtlCreateUnicodeString(&Device->DeviceID, DevicePath);//"Root\ServiceName"
...

    Status = IopOpenRegistryKeyEx(&EnumHandle/*输出*/, NULL, &EnumKeyName, KEY_READ);
    //                                                             |
    //                                      "\\Registry\\Machine\\System\\CurrentControlSet\\Enum"
    
... 
    //创建DeviceKeyHandle，在Registry\Machine\System\\CurrentControlSet\\Enum下，比如
    // HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\Root\LEGACY_NDIS
        InitializeObjectAttributes(&ObjectAttributes,/*输出*/
                                   &Device->DeviceID,//"Root\ServiceName"
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   EnumHandle,//RootDirectory
                                   NULL);
        Status = ZwCreateKey(&DeviceKeyHandle/*输出*/, KEY_SET_VALUE, &ObjectAttributes, 0, NULL, REG_OPTION_VOLATILE, NULL);
        ObCloseHandle(EnumHandle, KernelMode);//不再需要
 /*综上所述：
    EnumKeyName =  "\Registry\Machine\System\CurrentControlSet\Enum",其句柄为EnumHandle
    DeviceID = "Root\ServiceName"
    DeviceKeyHandle为\Registry\Machine\System\CurrentControlSet\Enum\Root\ServiceName的句柄 
    打开目录（EnumKeyName），创建key（Root\ServiceName）
  */
...
tryagain:
    RtlZeroMemory(QueryTable, sizeof(QueryTable));
    QueryTable[0].Name = L"NextInstance";
    QueryTable[0].EntryContext = &NextInstance;//查询参数将在此保存
    QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED;

    Status = RtlQueryRegistryValues(RTL_REGISTRY_HANDLE, //path参数实际上是一个句柄
                                    (PWSTR)DeviceKeyHandle, //path
                                    QueryTable,
                                    NULL,//Context
                                    NULL); //Environment
    if (!NT_SUCCESS(Status)) //处理如果没有NextInstance值的情况，正常会有NextInstance的
    {
        for (NextInstance = 0; NextInstance <= 9999; NextInstance++)
        {
             _snwprintf(InstancePath, sizeof(InstancePath) / sizeof(WCHAR), L"%04lu", NextInstance);//0000~9999?
             Status = LocateChildDevice(DeviceExtension, DevicePath, InstancePath, &Device/*输出*/);
             if (Status == STATUS_NO_SUCH_DEVICE) //找到一个没用的跳出
                 break;
        }

        if (NextInstance > 9999)
        {
            DPRINT1("Too many legacy devices reported for service '%wZ'\n", ServiceName);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto cleanup;
        }
    }
    
    //现在有了NextInstance将转换为InstancePath
    //NextInstance和InstancePath总是4字节无符号长整数
    //InstancePath居然是个数字字符串，比如"0001"
    _snwprintf(InstancePath, sizeof(InstancePath) / sizeof(WCHAR), L"%04lu", NextInstance);
    Status = LocateChildDevice(DeviceExtension, DevicePath/*"Root\ServiceName"*/, InstancePath/*"0001"~"9999"*/, &Device);
    
    //InstancePath所指的设备肯定不在 PnpRootDeviceObject->DeviceExtension表上，因为还没有创建。如果找到肯定出错了
    if (Status != STATUS_NO_SUCH_DEVICE || NextInstance > 9999)
    {
        DPRINT1("NextInstance value is corrupt! (%lu)\n", NextInstance);
        RtlDeleteRegistryValue(RTL_REGISTRY_HANDLE,
                               (PWSTR)DeviceKeyHandle,
                               L"NextInstance");
        goto tryagain;
    }

    //更新一下注册表中的NextInstance值的data
    NextInstance++;
    Status = RtlWriteRegistryValue(RTL_REGISTRY_HANDLE,
                                   (PWSTR)DeviceKeyHandle,//"Root\ServiceName"
                                   L"NextInstance",
                                   REG_DWORD,
                                   &NextInstance,
                                   sizeof(NextInstance));
...
    RtlCreateUnicodeString(&Device->InstanceID, InstancePath);
    //                          比如"0000"
 
    /*下面生成一个键，其层次为：
    \Registry\Machine\System\CurrentControlSet\Enum
    \Root\ServiceName
    \0000  
    这意味着，如果注册表中没有\0000类似的东西，就没有实际那个硬件存在？
    */
    
    /* Finish creating the instance path in the registry */
    InitializeObjectAttributes(&ObjectAttributes,
                               &Device->InstanceID,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               DeviceKeyHandle,
                               NULL);
    Status = ZwCreateKey(&InstanceKeyHandle, KEY_QUERY_VALUE, &ObjectAttributes, 0, NULL, REG_OPTION_VOLATILE, NULL);
...
    /* Just close the handle */
    ObCloseHandle(InstanceKeyHandle, KernelMode);

    if (FullInstancePath)
    {
        FullInstancePath->MaximumLength = Device->DeviceID.Length + PathSep.Length + Device->InstanceID.Length;
        FullInstancePath->Length = 0;
        FullInstancePath->Buffer = ExAllocatePool(PagedPool, FullInstancePath->MaximumLength);
...
        //FullInstancePath示:"\Root\ServiceName\0000"
        RtlAppendUnicodeStringToString(FullInstancePath, &Device->DeviceID);
        RtlAppendUnicodeStringToString(FullInstancePath, &PathSep);
        RtlAppendUnicodeStringToString(FullInstancePath, &Device->InstanceID);
    }

    /* !!! Initialize a device object !!!*/
    Status = IoCreateDevice(
        DriverObject ? DriverObject : PnpRootDeviceObject->DriverObject,
        sizeof(PNPROOT_PDO_DEVICE_EXTENSION),
        NULL,
        FILE_DEVICE_CONTROLLER,//控制器？
        FILE_AUTOGENERATED_DEVICE_NAME,
        FALSE,
        &Device->Pdo);
...
    PdoDeviceExtension = (PPNPROOT_PDO_DEVICE_EXTENSION)Device->Pdo->DeviceExtension;
    RtlZeroMemory(PdoDeviceExtension, sizeof(PNPROOT_PDO_DEVICE_EXTENSION));
    PdoDeviceExtension->Common.IsFDO = FALSE;
    PdoDeviceExtension->DeviceInfo = Device; //PNPROOT_DEVICE结构
/* 
    typedef struct _PNPROOT_DEVICE
{

    LIST_ENTRY ListEntry; //凡是"ROOT\XXX"的PNPROOT_DEVICE设备都穿在一起
 
    PDEVICE_OBJECT Pdo;//刚刚创建的设备对象结构，其扩展为PNPROOT_PDO_DEVICE_EXTENSION

    UNICODE_STRING DeviceID; 比如"ROOT\LEGACY_VMBUS"

    UNICODE_STRING InstanceID; 比如"0000"

    UNICODE_STRING DeviceDescription;当前为"\0"

    PIO_RESOURCE_REQUIREMENTS_LIST ResourceRequirementsList; 当前为NULL
    PCM_RESOURCE_LIST ResourceList;当前为NULL
    ULONG ResourceListSize;当前为0
} PNPROOT_DEVICE, *PPNPROOT_DEVICE;
*/
    
    
    Device->Pdo->Flags |= DO_BUS_ENUMERATED_DEVICE;//ROOT的东西也算是一种BUS
    Device->Pdo->Flags &= ~DO_DEVICE_INITIALIZING;

    //串在静态指针PnpRootDeviceObject的设备扩展PNPROOT_FDO_DEVICE_EXTENSION上
    //注意PnpRootDeviceObject也是一个标准的PDEVICE_OBJECT
    InsertTailList(
        &DeviceExtension->DeviceListHead,
        &Device->ListEntry);
    DeviceExtension->DeviceListCount++;

    *PhysicalDeviceObject = Device->Pdo;
    DPRINT("Created PDO %p (%wZ\\%wZ)\n", *PhysicalDeviceObject, &Device->DeviceID, &Device->InstanceID);
    Device = NULL;
    Status = STATUS_SUCCESS;

cleanup:
    KeReleaseGuardedMutex(&DeviceExtension->DeviceListLock);
    if (Device)
    {
        if (Device->Pdo)
            IoDeleteDevice(Device->Pdo);
        RtlFreeUnicodeString(&Device->DeviceID);
        RtlFreeUnicodeString(&Device->InstanceID);
        ExFreePoolWithTag(Device, TAG_PNP_ROOT);
    }
    if (DeviceKeyHandle != INVALID_HANDLE_VALUE)
        ObCloseHandle(DeviceKeyHandle, KernelMode);
    return Status;
}

//构造作为PUNICODE_STRING的EntryContext，使其完整正确
static NTSTATUS NTAPI
QueryStringCallback(
    IN PWSTR ValueName,
    IN ULONG ValueType,
    IN PVOID ValueData,
    IN ULONG ValueLength,
    IN PVOID Context,
    IN PVOID EntryContext) //EntryContext,字符串，对其进行处理，排除错误，看看能否复制，能复制为正常
{
    PUNICODE_STRING Destination = (PUNICODE_STRING)EntryContext;
    UNICODE_STRING Source;

    if (ValueType != REG_SZ || ValueLength == 0 || ValueLength % sizeof(WCHAR) != 0)
    {
        Destination->Length = 0;
        Destination->MaximumLength = 0;
        Destination->Buffer = NULL;
        return STATUS_SUCCESS;
    }

    Source.MaximumLength = Source.Length = (USHORT)ValueLength;
    Source.Buffer = ValueData;

    return RtlDuplicateUnicodeString(RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE, &Source, Destination);
}
//构造作为PBUFFER的EntryContext，使其完整正确
static NTSTATUS NTAPI
QueryBinaryValueCallback(
    IN PWSTR ValueName,
    IN ULONG ValueType,
    IN PVOID ValueData,//Buffer->Data将等于此值
    IN ULONG ValueLength,//Buffer->ValueLength将等于此值
    IN PVOID Context,
    IN PVOID EntryContext) //PBUFFER,使其Data和Length字段完整正确
{
    PBUFFER Buffer = (PBUFFER)EntryContext;
    PVOID BinaryValue;

    if (ValueLength == 0)
    {
        *Buffer->Data = NULL;
        return STATUS_SUCCESS;
    }

    BinaryValue = ExAllocatePoolWithTag(PagedPool, ValueLength, TAG_PNP_ROOT);
...
    RtlCopyMemory(BinaryValue, ValueData, ValueLength);
    *Buffer->Data = BinaryValue;
    if (Buffer->Length) *Buffer->Length = ValueLength;
    return STATUS_SUCCESS;
}
 
/*
搜索注册表 \\Registry\\Machine\\System\\CurrentControlSet\\Enum\\Root下的设备
如果为传统设备，忽略之
如果为pnp设备，那么把Instance比如0000所代表的实例要创建一个PNPROOT_DEVICE，
其中全部参数都来自于读取的注册表信息，并且PNPROOT_DEVICE加入到串串中
*/
static NTSTATUS
EnumerateDevices(
    IN PDEVICE_OBJECT DeviceObject)
{
    PPNPROOT_FDO_DEVICE_EXTENSION DeviceExtension;
    PKEY_BASIC_INFORMATION KeyInfo = NULL, SubKeyInfo = NULL;
    UNICODE_STRING LegacyU = RTL_CONSTANT_STRING(L"LEGACY_");
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\" REGSTR_PATH_SYSTEMENUM L"\\" REGSTR_KEY_ROOTENUM);
    //                                             \\Registry\\Machine\\System\\CurrentControlSet\\Enum\\Root
    UNICODE_STRING SubKeyName;
    WCHAR DevicePath[MAX_PATH + 1];
    RTL_QUERY_REGISTRY_TABLE QueryTable[4];
    PPNPROOT_DEVICE Device = NULL;
    HANDLE KeyHandle = INVALID_HANDLE_VALUE;
    HANDLE SubKeyHandle = INVALID_HANDLE_VALUE;
    HANDLE DeviceKeyHandle = INVALID_HANDLE_VALUE;
    ULONG BufferSize;
    ULONG ResultSize;
    ULONG Index1, Index2;
    BUFFER Buffer1, Buffer2;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    DPRINT("EnumerateDevices(FDO %p)\n", DeviceObject);

    DeviceExtension = (PPNPROOT_FDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    KeAcquireGuardedMutex(&DeviceExtension->DeviceListLock);

    BufferSize = sizeof(KEY_BASIC_INFORMATION) + (MAX_PATH + 1) * sizeof(WCHAR);
    KeyInfo = ExAllocatePoolWithTag(PagedPool, BufferSize, TAG_PNP_ROOT);
...
    SubKeyInfo = ExAllocatePoolWithTag(PagedPool, BufferSize, TAG_PNP_ROOT);
...
/*
KeyInfo和SubKeyInfo的结构：
    typedef struct _KEY_BASIC_INFORMATION {
    LARGE_INTEGER LastWriteTime;
    ULONG         TitleIndex;
    ULONG         NameLength;
    WCHAR         Name[1]; //变长
} KEY_BASIC_INFORMATION, *PKEY_BASIC_INFORMATION;
 */   
    
    //打开\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\Root
    Status = IopOpenRegistryKeyEx(&KeyHandle/*输出*/, NULL, &KeyName, KEY_ENUMERATE_SUB_KEYS);
...
    /* Devices are sub-sub-keys of 'KeyName'. KeyName is already opened as
     * KeyHandle. We'll first do a first enumeration to have first level keys,
     * and an inner one to have the real devices list.
     */
    Index1 = 0;
    while (TRUE)
    {
        Status = ZwEnumerateKey(
            KeyHandle,
            Index1,
            KeyBasicInformation,//枚举，KEY_INFORMATION_CLASS
            KeyInfo,/*输出*/
            BufferSize,
            &ResultSize);/*输出*/
        if (Status == STATUS_NO_MORE_ENTRIES)
        {
            Status = STATUS_SUCCESS; //Root\没有任何东西也是一种可能的
            break;
        }
...

        /* Terminate the string */
        KeyInfo->Name[KeyInfo->NameLength / sizeof(WCHAR)] = 0;

        /* Check if it is a legacy driver */
        RtlInitUnicodeString(&SubKeyName, KeyInfo->Name);
        if (RtlPrefixUnicodeString(&LegacyU, &SubKeyName, FALSE)) //如果"LEGACY_"字样则是legacy driver，将忽略
        {
            DPRINT("Ignoring legacy driver '%wZ'\n", &SubKeyName);
            Index1++;
            continue;
        }

        /* Open the key */
        Status = IopOpenRegistryKeyEx(&SubKeyHandle, KeyHandle, &SubKeyName, KEY_ENUMERATE_SUB_KEYS);
                                           |
        //现在SubKeyHandle为Root下面的项，比如"mssmbios","RDP_KBD","RDP_MOU"
...

        /* Enumerate the sub-keys */
        Index2 = 0;
        while (TRUE)
        {
            Status = ZwEnumerateKey(
                SubKeyHandle,//比如在"mssmbios","RDP_KBD","RDP_MOU"下寻找
                Index2,
                KeyBasicInformation,//枚举，KEY_INFORMATION_CLASS
                SubKeyInfo,/*输出*/
                BufferSize,
                &ResultSize);/*输出*/
            if (Status == STATUS_NO_MORE_ENTRIES)
                break;
...
            /* Terminate the string */
            //现在SubKeyInfo->Name实际是InstanceId，可能是"RDP_KBD"什么的
            SubKeyInfo->Name[SubKeyInfo->NameLength / sizeof(WCHAR)] = 0;

            //构造DevicePath，比如DevicePath = "Root\RDP_KBD"
            _snwprintf(DevicePath, sizeof(DevicePath) / sizeof(WCHAR),
                       L"%s\\%s", REGSTR_KEY_ROOTENUM, KeyInfo->Name);
            DPRINT("Found device %S\\%s!\n", DevicePath, SubKeyInfo->Name);
            if (LocateChildDevice(DeviceExtension, DevicePath, SubKeyInfo->Name, &Device) == STATUS_NO_SUCH_DEVICE)
            //                                     DeviceId    InstanceId
            {
                /* Create a PPNPROOT_DEVICE object, and add if in the list of known devices */
                Device = (PPNPROOT_DEVICE)ExAllocatePoolWithTag(PagedPool, sizeof(PNPROOT_DEVICE), TAG_PNP_ROOT);
...
                RtlZeroMemory(Device, sizeof(PNPROOT_DEVICE));

                /* Fill device ID and instance ID */
                if (!RtlCreateUnicodeString(&Device->DeviceID, DevicePath))
                {
...
                }

                if (!RtlCreateUnicodeString(&Device->InstanceID, SubKeyInfo->Name))
                {
...
                }

                /* Open registry key to fill other informations */
                Status = IopOpenRegistryKeyEx(&DeviceKeyHandle, SubKeyHandle, &Device->InstanceID, KEY_READ);
                //                                                  |相当于          |
                //                                              Root\RDP_KBD       "0000"
...

                /* Fill information from the device instance key */
                RtlZeroMemory(QueryTable, sizeof(QueryTable));
                QueryTable[0].QueryRoutine = QueryStringCallback;
                QueryTable[0].Name = L"DeviceDesc";
                QueryTable[0].EntryContext = &Device->DeviceDescription;//将保存在此，比如"@machine.inf,%rdp_kbd.devicedesc%;Terminal Server Keyboard Driver"
                RtlQueryRegistryValues(RTL_REGISTRY_HANDLE,
                                       (PCWSTR)DeviceKeyHandle,//比如0000的句柄
                                       QueryTable,
                                       NULL,
                                       NULL);

                /* Fill information from the LogConf subkey */
                Buffer1.Data = (PVOID *)&Device->ResourceRequirementsList;//原来来自注册表
                Buffer1.Length = NULL;
                Buffer2.Data = (PVOID *)&Device->ResourceList;
                Buffer2.Length = &Device->ResourceListSize;
                RtlZeroMemory(QueryTable, sizeof(QueryTable));
                QueryTable[0].Flags = RTL_QUERY_REGISTRY_SUBKEY;
                QueryTable[0].Name = L"LogConf";
                QueryTable[1].QueryRoutine = QueryBinaryValueCallback;
                QueryTable[1].Name = L"BasicConfigVector";
                QueryTable[1].EntryContext = &Buffer1;
                QueryTable[2].QueryRoutine = QueryBinaryValueCallback;
                QueryTable[2].Name = L"BootConfig";
                QueryTable[2].EntryContext = &Buffer2;

                if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_HANDLE,
                                                       (PCWSTR)DeviceKeyHandle,//比如0000的句柄
                                                       QueryTable,
                                                       NULL,
                                                       NULL)))
...
                ZwClose(DeviceKeyHandle);
                DeviceKeyHandle = INVALID_HANDLE_VALUE;

                /* Insert the newly created device into the list */
                InsertTailList(
                    &DeviceExtension->DeviceListHead,
                    &Device->ListEntry);
                DeviceExtension->DeviceListCount++;
            }
            Device = NULL;

            Index2++;
        }

        ZwClose(SubKeyHandle);
        SubKeyHandle = INVALID_HANDLE_VALUE;
        Index1++;
    }

cleanup:
    if (Device)
    {
        /* We have a device that has not been added to device list. We need to clean it up */
        /* FIXME */
        ExFreePoolWithTag(Device, TAG_PNP_ROOT);
    }
    if (DeviceKeyHandle != INVALID_HANDLE_VALUE)
        ZwClose(DeviceKeyHandle);
    if (SubKeyHandle != INVALID_HANDLE_VALUE)
        ZwClose(SubKeyHandle);
    if (KeyHandle != INVALID_HANDLE_VALUE)
        ZwClose(KeyHandle);
    if (KeyInfo)
        ExFreePoolWithTag(KeyInfo, TAG_PNP_ROOT);
    if (SubKeyInfo)
        ExFreePoolWithTag(SubKeyInfo, TAG_PNP_ROOT);
    KeReleaseGuardedMutex(&DeviceExtension->DeviceListLock);
    return Status;
}

/* FUNCTION: Handle IRP_MN_QUERY_DEVICE_RELATIONS IRPs for the root bus device object
 * ARGUMENTS:
 *     DeviceObject = Pointer to functional device object of the root bus driver
 *     Irp          = Pointer to IRP that should be handled
 * RETURNS:
 *     Status
 */
static NTSTATUS
PnpRootQueryDeviceRelations(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PPNPROOT_PDO_DEVICE_EXTENSION PdoDeviceExtension;
    PPNPROOT_FDO_DEVICE_EXTENSION DeviceExtension;
    PDEVICE_RELATIONS Relations = NULL, OtherRelations = (PDEVICE_RELATIONS)Irp->IoStatus.Information;
    PPNPROOT_DEVICE Device = NULL;
    ULONG Size;
    NTSTATUS Status;
    PLIST_ENTRY NextEntry;

    DPRINT("PnpRootQueryDeviceRelations(FDO %p, Irp %p)\n", DeviceObject, Irp);

    Status = EnumerateDevices(DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("EnumerateDevices() failed with status 0x%08lx\n", Status);
        return Status;
    }

    DeviceExtension = (PPNPROOT_FDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    Size = FIELD_OFFSET(DEVICE_RELATIONS, Objects) + sizeof(PDEVICE_OBJECT) * DeviceExtension->DeviceListCount;
    if (OtherRelations)
    {
        /* Another bus driver has already created a DEVICE_RELATIONS
         * structure so we must merge this structure with our own */

        Size += sizeof(PDEVICE_OBJECT) * OtherRelations->Count;
    }
    Relations = (PDEVICE_RELATIONS)ExAllocatePool(PagedPool, Size);
    if (!Relations)
    {
        DPRINT("ExAllocatePoolWithTag() failed\n");
        Status = STATUS_NO_MEMORY;
        goto cleanup;
    }
    RtlZeroMemory(Relations, Size);
    if (OtherRelations)
    {
        Relations->Count = OtherRelations->Count;
        RtlCopyMemory(Relations->Objects, OtherRelations->Objects, sizeof(PDEVICE_OBJECT) * OtherRelations->Count);
    }

    KeAcquireGuardedMutex(&DeviceExtension->DeviceListLock);

    /* Start looping */
    for (NextEntry = DeviceExtension->DeviceListHead.Flink;
         NextEntry != &DeviceExtension->DeviceListHead;
         NextEntry = NextEntry->Flink)
    {
        /* Get the entry */
        Device = CONTAINING_RECORD(NextEntry, PNPROOT_DEVICE, ListEntry);

        if (!Device->Pdo)
        {
            /* Create a physical device object for the
             * device as it does not already have one */
            Status = IoCreateDevice(
                DeviceObject->DriverObject,
                sizeof(PNPROOT_PDO_DEVICE_EXTENSION),
                NULL,
                FILE_DEVICE_CONTROLLER,
                FILE_AUTOGENERATED_DEVICE_NAME,
                FALSE,
                &Device->Pdo);
            if (!NT_SUCCESS(Status))
            {
                DPRINT("IoCreateDevice() failed with status 0x%08lx\n", Status);
                break;
            }

            PdoDeviceExtension = (PPNPROOT_PDO_DEVICE_EXTENSION)Device->Pdo->DeviceExtension;
            RtlZeroMemory(PdoDeviceExtension, sizeof(PNPROOT_PDO_DEVICE_EXTENSION));
            PdoDeviceExtension->Common.IsFDO = FALSE;
            PdoDeviceExtension->DeviceInfo = Device;

            Device->Pdo->Flags |= DO_BUS_ENUMERATED_DEVICE;
            Device->Pdo->Flags &= ~DO_DEVICE_INITIALIZING;
        }

        /* Reference the physical device object. The PnP manager
         will dereference it again when it is no longer needed */
        ObReferenceObject(Device->Pdo);

        Relations->Objects[Relations->Count++] = Device->Pdo;
    }
    KeReleaseGuardedMutex(&DeviceExtension->DeviceListLock);

    Irp->IoStatus.Information = (ULONG_PTR)Relations;

cleanup:
    if (!NT_SUCCESS(Status))
    {
        if (OtherRelations)
            ExFreePool(OtherRelations);
        if (Relations)
            ExFreePool(Relations);
        if (Device && Device->Pdo)
        {
            IoDeleteDevice(Device->Pdo);
            Device->Pdo = NULL;
        }
    }

    return Status;
}

/*
 * FUNCTION: Handle Plug and Play IRPs for the root bus device object
 * ARGUMENTS:
 *     DeviceObject = Pointer to functional device object of the root bus driver
 *     Irp          = Pointer to IRP that should be handled
 * RETURNS:
 *     Status
 */
static NTSTATUS
PnpRootFdoPnpControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PPNPROOT_FDO_DEVICE_EXTENSION DeviceExtension;
    PIO_STACK_LOCATION IrpSp;
    NTSTATUS Status;

    DeviceExtension = (PPNPROOT_FDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    Status = Irp->IoStatus.Status;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (IrpSp->MinorFunction)
    {
        case IRP_MN_QUERY_DEVICE_RELATIONS:
            DPRINT("IRP_MJ_PNP / IRP_MN_QUERY_DEVICE_RELATIONS\n");
            Status = PnpRootQueryDeviceRelations(DeviceObject, Irp);
            break;

        case IRP_MN_START_DEVICE:
            DPRINT("IRP_MJ_PNP / IRP_MN_START_DEVICE\n");
            if (!IoForwardIrpSynchronously(DeviceExtension->Ldo, Irp))
                Status = STATUS_UNSUCCESSFUL;
            else
            {
                Status = Irp->IoStatus.Status;
                if (NT_SUCCESS(Status))
                    DeviceExtension->State = dsStarted;
            }

            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;

         case IRP_MN_STOP_DEVICE:
             DPRINT("IRP_MJ_PNP / IRP_MN_STOP_DEVICE\n");
             /* Root device cannot be stopped */
             Irp->IoStatus.Status = Status = STATUS_INVALID_DEVICE_REQUEST;
             IoCompleteRequest(Irp, IO_NO_INCREMENT);
             return Status;

        default:
            DPRINT("IRP_MJ_PNP / Unknown minor function 0x%lx\n", IrpSp->MinorFunction);
            break;
    }

    if (Status != STATUS_PENDING)
    {
       Irp->IoStatus.Status = Status;
       IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    return Status;
}

static NTSTATUS
PdoQueryDeviceRelations(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp)
{
    PDEVICE_RELATIONS Relations;
    NTSTATUS Status = Irp->IoStatus.Status;

    if (IrpSp->Parameters.QueryDeviceRelations.Type != TargetDeviceRelation)
        return Status;

    DPRINT("IRP_MJ_PNP / IRP_MN_QUERY_DEVICE_RELATIONS / TargetDeviceRelation\n");
    Relations = (PDEVICE_RELATIONS)ExAllocatePool(PagedPool, sizeof(DEVICE_RELATIONS));
    if (!Relations)
    {
        DPRINT("ExAllocatePoolWithTag() failed\n");
        Status = STATUS_NO_MEMORY;
    }
    else
    {
        ObReferenceObject(DeviceObject);
        Relations->Count = 1;
        Relations->Objects[0] = DeviceObject;
        Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = (ULONG_PTR)Relations;
    }

    return Status;
}

static NTSTATUS
PdoQueryCapabilities(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp)
{
    PDEVICE_CAPABILITIES DeviceCapabilities;

    DeviceCapabilities = IrpSp->Parameters.DeviceCapabilities.Capabilities;

    if (DeviceCapabilities->Version != 1)
        return STATUS_REVISION_MISMATCH;

    DeviceCapabilities->UniqueID = TRUE;
    /* FIXME: Fill other fields */

    return STATUS_SUCCESS;
}

static NTSTATUS
PdoQueryResources(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp)
{
    PPNPROOT_PDO_DEVICE_EXTENSION DeviceExtension;
    PCM_RESOURCE_LIST ResourceList;

    DeviceExtension = (PPNPROOT_PDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (DeviceExtension->DeviceInfo->ResourceList)
    {
        /* Copy existing resource requirement list */
        ResourceList = ExAllocatePool(
            PagedPool,
            DeviceExtension->DeviceInfo->ResourceListSize);
        if (!ResourceList)
            return STATUS_NO_MEMORY;

        RtlCopyMemory(
            ResourceList,
            DeviceExtension->DeviceInfo->ResourceList,
            DeviceExtension->DeviceInfo->ResourceListSize);

        Irp->IoStatus.Information = (ULONG_PTR)ResourceList;

        return STATUS_SUCCESS;
    }
    else
    {
        /* No resources so just return without changing the status */
        return Irp->IoStatus.Status;
    }
}

static NTSTATUS
PdoQueryResourceRequirements(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp)
{
    PPNPROOT_PDO_DEVICE_EXTENSION DeviceExtension;
    PIO_RESOURCE_REQUIREMENTS_LIST ResourceList;

    DeviceExtension = (PPNPROOT_PDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (DeviceExtension->DeviceInfo->ResourceRequirementsList)
    {
        /* Copy existing resource requirement list */
        ResourceList = ExAllocatePool(PagedPool, DeviceExtension->DeviceInfo->ResourceRequirementsList->ListSize);
        if (!ResourceList)
            return STATUS_NO_MEMORY;

        RtlCopyMemory(
            ResourceList,
            DeviceExtension->DeviceInfo->ResourceRequirementsList,
            DeviceExtension->DeviceInfo->ResourceRequirementsList->ListSize);

        Irp->IoStatus.Information = (ULONG_PTR)ResourceList;

        return STATUS_SUCCESS;
    }
    else
    {
        /* No resource requirements so just return without changing the status */
        return Irp->IoStatus.Status;
    }
}

static NTSTATUS
PdoQueryDeviceText(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp)
{
    PPNPROOT_PDO_DEVICE_EXTENSION DeviceExtension;
    DEVICE_TEXT_TYPE DeviceTextType;
    NTSTATUS Status = Irp->IoStatus.Status;

    DeviceExtension = (PPNPROOT_PDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    DeviceTextType = IrpSp->Parameters.QueryDeviceText.DeviceTextType;

    switch (DeviceTextType)
    {
        case DeviceTextDescription:
        {
            UNICODE_STRING String;
            DPRINT("IRP_MJ_PNP / IRP_MN_QUERY_DEVICE_TEXT / DeviceTextDescription\n");

            if (DeviceExtension->DeviceInfo->DeviceDescription.Buffer != NULL)
            {
                Status = RtlDuplicateUnicodeString(RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE,
                                                   &DeviceExtension->DeviceInfo->DeviceDescription,
                                                   &String);
                Irp->IoStatus.Information = (ULONG_PTR)String.Buffer;
            }
            break;
        }

        case DeviceTextLocationInformation:
        {
            DPRINT("IRP_MJ_PNP / IRP_MN_QUERY_DEVICE_TEXT / DeviceTextLocationInformation\n");
            break;
        }

        default:
        {
            DPRINT1("IRP_MJ_PNP / IRP_MN_QUERY_DEVICE_TEXT / unknown query id type 0x%lx\n", DeviceTextType);
        }
    }

    return Status;
}

static NTSTATUS
PdoQueryId(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp)
{
    PPNPROOT_PDO_DEVICE_EXTENSION DeviceExtension;
    BUS_QUERY_ID_TYPE IdType;
    NTSTATUS Status = Irp->IoStatus.Status;

    DeviceExtension = (PPNPROOT_PDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    IdType = IrpSp->Parameters.QueryId.IdType;

    switch (IdType)
    {
        case BusQueryDeviceID:
        {
            UNICODE_STRING String;
            DPRINT("IRP_MJ_PNP / IRP_MN_QUERY_ID / BusQueryDeviceID\n");

            Status = RtlDuplicateUnicodeString(
                RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE,
                &DeviceExtension->DeviceInfo->DeviceID,
                &String);
            Irp->IoStatus.Information = (ULONG_PTR)String.Buffer;
            break;
        }

        case BusQueryHardwareIDs:
        case BusQueryCompatibleIDs:
        {
            /* Optional, do nothing */
            break;
        }

        case BusQueryInstanceID:
        {
            UNICODE_STRING String;
            DPRINT("IRP_MJ_PNP / IRP_MN_QUERY_ID / BusQueryInstanceID\n");

            Status = RtlDuplicateUnicodeString(
                RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE,
                &DeviceExtension->DeviceInfo->InstanceID,
                &String);
            Irp->IoStatus.Information = (ULONG_PTR)String.Buffer;
            break;
        }

        default:
        {
            DPRINT1("IRP_MJ_PNP / IRP_MN_QUERY_ID / unknown query id type 0x%lx\n", IdType);
        }
    }

    return Status;
}

static NTSTATUS
PdoQueryBusInformation(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp)
{
    PPNP_BUS_INFORMATION BusInfo;
    NTSTATUS Status;

    BusInfo = (PPNP_BUS_INFORMATION)ExAllocatePoolWithTag(PagedPool, sizeof(PNP_BUS_INFORMATION), TAG_PNP_ROOT);
    if (!BusInfo)
        Status = STATUS_NO_MEMORY;
    else
    {
        RtlCopyMemory(
            &BusInfo->BusTypeGuid,
            &GUID_BUS_TYPE_INTERNAL,
            sizeof(BusInfo->BusTypeGuid));
        BusInfo->LegacyBusType = PNPBus;
        /* We're the only root bus enumerator on the computer */
        BusInfo->BusNumber = 0;
        Irp->IoStatus.Information = (ULONG_PTR)BusInfo;
        Status = STATUS_SUCCESS;
    }

    return Status;
}

/*
 * FUNCTION: Handle Plug and Play IRPs for the child device
 * ARGUMENTS:
 *     DeviceObject = Pointer to physical device object of the child device
 *     Irp          = Pointer to IRP that should be handled
 * RETURNS:
 *     Status
 */
static NTSTATUS
PnpRootPdoPnpControl(
  IN PDEVICE_OBJECT DeviceObject,
  IN PIRP Irp)
{
  PPNPROOT_PDO_DEVICE_EXTENSION DeviceExtension;
  PPNPROOT_FDO_DEVICE_EXTENSION FdoDeviceExtension;
  PIO_STACK_LOCATION IrpSp;
  NTSTATUS Status;

  DeviceExtension = (PPNPROOT_PDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
  FdoDeviceExtension = (PPNPROOT_FDO_DEVICE_EXTENSION)PnpRootDeviceObject->DeviceExtension;
  Status = Irp->IoStatus.Status;
  IrpSp = IoGetCurrentIrpStackLocation(Irp);

  switch (IrpSp->MinorFunction)
  {
    case IRP_MN_START_DEVICE: /* 0x00 */
      DPRINT("IRP_MJ_PNP / IRP_MN_START_DEVICE\n");
      Status = STATUS_SUCCESS;//不需要做什么
      break;

    case IRP_MN_QUERY_DEVICE_RELATIONS: /* 0x07 */
      Status = PdoQueryDeviceRelations(DeviceObject, Irp, IrpSp);
      break;

    case IRP_MN_QUERY_CAPABILITIES: /* 0x09 */
      DPRINT("IRP_MJ_PNP / IRP_MN_QUERY_CAPABILITIES\n");
      Status = PdoQueryCapabilities(DeviceObject, Irp, IrpSp);
      break;

    case IRP_MN_QUERY_RESOURCES: /* 0x0a */
      DPRINT("IRP_MJ_PNP / IRP_MN_QUERY_RESOURCES\n");
      Status = PdoQueryResources(DeviceObject, Irp, IrpSp);
      break;

   case IRP_MN_QUERY_RESOURCE_REQUIREMENTS: /* 0x0b */
      DPRINT("IRP_MJ_PNP / IRP_MN_QUERY_RESOURCE_REQUIREMENTS\n");
      Status = PdoQueryResourceRequirements(DeviceObject, Irp, IrpSp);
      break;

    case IRP_MN_QUERY_DEVICE_TEXT: /* 0x0c */
      DPRINT("IRP_MJ_PNP / IRP_MN_QUERY_RESOURCE_REQUIREMENTS\n");
      Status = PdoQueryDeviceText(DeviceObject, Irp, IrpSp);
      break;

    case IRP_MN_FILTER_RESOURCE_REQUIREMENTS: /* 0x0d */
        DPRINT("IRP_MJ_PNP / IRP_MN_FILTER_RESOURCE_REQUIREMENTS\n");
        break;

    case IRP_MN_REMOVE_DEVICE:
        /* Remove the device from the device list and decrement the device count*/
        KeAcquireGuardedMutex(&FdoDeviceExtension->DeviceListLock);
        RemoveEntryList(&DeviceExtension->DeviceInfo->ListEntry);
        FdoDeviceExtension->DeviceListCount--;
        KeReleaseGuardedMutex(&FdoDeviceExtension->DeviceListLock);

        /* Free some strings we created */
        RtlFreeUnicodeString(&DeviceExtension->DeviceInfo->DeviceDescription);
        RtlFreeUnicodeString(&DeviceExtension->DeviceInfo->DeviceID);
        RtlFreeUnicodeString(&DeviceExtension->DeviceInfo->InstanceID);

        /* Free the resource requirements list */
        if (DeviceExtension->DeviceInfo->ResourceRequirementsList != NULL)
            ExFreePool(DeviceExtension->DeviceInfo->ResourceRequirementsList);

        /* Free the boot resources list */
        if (DeviceExtension->DeviceInfo->ResourceList != NULL)
            ExFreePool(DeviceExtension->DeviceInfo->ResourceList);

        /* Free the device info */
        ExFreePool(DeviceExtension->DeviceInfo);

        /* Finally, delete the device object */
        IoDeleteDevice(DeviceObject);

        /* Return success */
        Status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_ID: /* 0x13 */
      Status = PdoQueryId(DeviceObject, Irp, IrpSp);
      break;

        case IRP_MN_QUERY_BUS_INFORMATION: /* 0x15 */
            DPRINT("IRP_MJ_PNP / IRP_MN_QUERY_BUS_INFORMATION\n");
            Status = PdoQueryBusInformation(DeviceObject, Irp, IrpSp);
            break;

        default:
            DPRINT1("IRP_MJ_PNP / Unknown minor function 0x%lx\n", IrpSp->MinorFunction);
            break;
    }

    if (Status != STATUS_PENDING)
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    return Status;
}

/*
 * FUNCTION: Handle Plug and Play IRPs
 * ARGUMENTS:
 *     DeviceObject = Pointer to PDO or FDO
 *     Irp          = Pointer to IRP that should be handled
 * RETURNS:
 *     Status
 */
static NTSTATUS NTAPI
PnpRootPnpControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PPNPROOT_COMMON_DEVICE_EXTENSION DeviceExtension;
    NTSTATUS Status;

    DeviceExtension = (PPNPROOT_COMMON_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (DeviceExtension->IsFDO)
        Status = PnpRootFdoPnpControl(DeviceObject, Irp);
    else
        Status = PnpRootPdoPnpControl(DeviceObject, Irp);

    return Status;
}
 
//创建全局唯一静态变量(FDO)：PnpRootDeviceObject
NTSTATUS
NTAPI
PnpRootAddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject/*必须存在，否则报错*/)
{
    PPNPROOT_FDO_DEVICE_EXTENSION DeviceExtension;
    NTSTATUS Status;

    DPRINT("PnpRootAddDevice(DriverObject %p, Pdo %p)\n", DriverObject, PhysicalDeviceObject);

    if (!PhysicalDeviceObject)
    {
        DPRINT("PhysicalDeviceObject 0x%p\n", PhysicalDeviceObject);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        KeBugCheckEx(PHASE1_INITIALIZATION_FAILED, Status, 0, 0, 0);
    }

    Status = IoCreateDevice(
        DriverObject,
        sizeof(PNPROOT_FDO_DEVICE_EXTENSION),//注意是PNPROOT_FDO_DEVICE_EXTENSION结构，就是我们正在创建FDO
        NULL,
        FILE_DEVICE_BUS_EXTENDER,
        FILE_DEVICE_SECURE_OPEN,
        TRUE,
        &PnpRootDeviceObject);//全局唯一静态指针：static PDEVICE_OBJECT PnpRootDeviceObject = NULL;
...
    DPRINT("Created FDO %p\n", PnpRootDeviceObject);

    DeviceExtension = (PPNPROOT_FDO_DEVICE_EXTENSION)PnpRootDeviceObject->DeviceExtension;
    RtlZeroMemory(DeviceExtension, sizeof(PNPROOT_FDO_DEVICE_EXTENSION));

    DeviceExtension->Common.IsFDO = TRUE;
    DeviceExtension->State = dsStopped;
    InitializeListHead(&DeviceExtension->DeviceListHead);
    DeviceExtension->DeviceListCount = 0;
    KeInitializeGuardedMutex(&DeviceExtension->DeviceListLock);

    Status = IoAttachDeviceToDeviceStackSafe(
        PnpRootDeviceObject,//上面刚刚创建的
        PhysicalDeviceObject,//必须存在
        &DeviceExtension->Ldo);//问题：DeviceExtension->Ldo难道不是PnpRootDeviceObject？
...

    PnpRootDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DPRINT("Done AddDevice()\n");

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
PnpRootDriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath)
{
    DPRINT("PnpRootDriverEntry(%p %wZ)\n", DriverObject, RegistryPath);

    IopRootDriverObject = DriverObject;

    DriverObject->DriverExtension->AddDevice = PnpRootAddDevice;

    DriverObject->MajorFunction[IRP_MJ_PNP] = PnpRootPnpControl;
    //DriverObject->MajorFunction[IRP_MJ_POWER] = PnpRootPowerControl;

    return STATUS_SUCCESS;
}
