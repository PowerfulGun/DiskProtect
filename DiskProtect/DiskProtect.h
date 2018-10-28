#pragma once
#ifndef _DISK_PROTECT_H
#define	_DISK_PROTECT_H

#define	BITS_ALL_CLEAR	0x1
#define	BITS_ALL_SET	0x2
#define	BITS_BLEND		0x3


#pragma	pack(1)
typedef	struct _BITMAP
{
	//这个卷的每个扇区有多少字节，也是bitmap中一个位所对应的字节数
	ULONG	PerSectorSize;

	//每个字节有多少位，一般8位
	ULONG	Bits_Of_Byte;

	//每个块占多少字节
	ULONG	Bytes_Of_Region;

	//这个bitmap总共有多少个块
	ULONG	RegionNumber;

	//每个快对应多少磁盘的字节
	//即PerSectorSize * Bits_Of_Byte * Bytes_Of_Region
	ULONGLONG	PerRegionSizeMapped;

	//这个Bitmap对应的实际的磁盘字节数
	//即PerSectorSize * Bits_Of_Byte * Bytes_Of_Region * RegionNumber
	ULONGLONG	BitmapSizeMapped;

	//用于存取bitmap的锁
	PVOID	BitmapLock;

	//指向bitmap存储空间的指针，是一个指针数组，数组有RegionNumber个元素，每一个元素都是一个地址
	//每个地址指向一个Bytes_Of_Region大小的块，块中的每个字节都有8位，每一位代表磁盘上的一个扇区
	UCHAR**	bitmap;

}BITMAP , *PBITMAP;
#pragma	pack()


typedef	struct _Device_Extension
{
	//卷的名字，例如c:，d:字母部分
	WCHAR	VolumeLetter;
	//目标卷设备是否在保护状态
	BOOL	bProtect;
	//目标卷的总大小，以字节为单位
	LARGE_INTEGER	liVolumeTotalSize;
	//目标卷上文件系统的每簇大小，以字节为单位
	DWORD	dwPerClusterSize;
	//目标卷的每个扇区的大小,以字节为单位
	DWORD	dwPerSectorSize;
	//目标卷设备对应的过滤设备对象
	PDEVICE_OBJECT	pFilterDeviceObject;
	//目标卷设备对应的过滤设备的下层设备对象
	PDEVICE_OBJECT	pLowerDeviceObject;
	//目标卷设备对应的物理设备对象
	PDEVICE_OBJECT	pPhysicalDeviceObject;
	//这个数据结构是否已经初始化完毕了
	BOOL	bIsInitialized;
	//为保护目标卷使用的位图结构的指针
	PBITMAP	pBitmap;
	//用来转储的临时文件句柄
	HANDLE	hTempFile;
	//为处理目标卷IRP使用的请求队列
	LIST_ENTRY	RequestList;
	//为同步处理请求使用的自旋锁
	KSPIN_LOCK	RequestSpinLock;
	//用来通知处理线程请求队列中有请求要处理而使用的事件
	KEVENT	RequestEvent;
	//处理线程的句柄
	PVOID	hThreadHandle;
	//用来通知处理线程退出的标记
	BOOLEAN	ThreadExitFlag;
	//目标卷关机分页电源请求的计数事件
	KEVENT	PagingPathCountEvent;
	//目标卷关机分页电源请求的计数
	ULONG	PagingPathCount;
} DEVICE_EXTENSION , *PDEVICE_EXTENSION;


typedef struct _VOLUME_ONLINE_CONTEXT_
{
	//在volume_online的DeviceIoControl中传给完成函数的设备扩展
	PDEVICE_EXTENSION	pDevExt;
	//在volume_online的DeviceIoControl中传给完成函数的同步事件
	PKEVENT				pWaitEvent;
}VOLUME_ONLINE_CONTEXT , *PVOLUME_ONLINE_CONTEXT;



#pragma pack(1)
typedef struct _FAT16_BOOT_SECTOR
{
	UCHAR		JMPInstruction[3];
	UCHAR		OEM[8];
	USHORT		BytesPerSector;
	UCHAR		SectorsPerCluster;
	USHORT		ReservedSectors;
	UCHAR		NumberOfFATs;
	USHORT		RootEntries;
	USHORT		Sectors;
	UCHAR		MediaDescriptor;
	USHORT		SectorsPerFAT;
	USHORT		SectorsPerTrack;
	USHORT		Heads;
	DWORD		HiddenSectors;
	DWORD		LargeSectors;
	UCHAR		PhysicalDriveNumber;
	UCHAR		CurrentHead;
} FAT16_BOOT_SECTOR , *PFAT16_BOOT_SECTOR;

typedef struct _FAT32_BOOT_SECTOR
{
	UCHAR		JMPInstruction[3];
	UCHAR		OEM[8];
	USHORT		BytesPerSector;
	UCHAR		SectorsPerCluster;
	USHORT		ReservedSectors;
	UCHAR		NumberOfFATs;
	USHORT		RootEntries;
	USHORT		Sectors;
	UCHAR		MediaDescriptor;
	USHORT		SectorsPerFAT;
	USHORT		SectorsPerTrack;
	USHORT		Heads;
	DWORD		HiddenSectors;
	DWORD		LargeSectors;
	DWORD		LargeSectorsPerFAT;
	UCHAR		Data[24];
	UCHAR		PhysicalDriveNumber;
	UCHAR		CurrentHead;
} FAT32_BOOT_SECTOR , *PFAT32_BOOT_SECTOR;

typedef struct _NTFS_BOOT_SECTOR
{
	UCHAR		Jump[3];					//0
	UCHAR		FSID[8];					//3
	USHORT		BytesPerSector;				//11
	UCHAR		SectorsPerCluster;			//13
	USHORT		ReservedSectors;			//14
	UCHAR		Mbz1;						//16		
	USHORT		Mbz2;						//17
	USHORT		Reserved1;					//19
	UCHAR		MediaDesc;					//21
	USHORT		Mbz3;						//22
	USHORT		SectorsPerTrack;			//24
	USHORT		Heads;						//26
	ULONG		HiddenSectors;				//28
	ULONG		Reserved2[2];				//32
	ULONGLONG	TotalSectors;				//40
	ULONGLONG	MftStartLcn;				//48
	ULONGLONG	Mft2StartLcn;				//56
}NTFS_BOOT_SECTOR , *PNTFS_BOOT_SECTOR;
#pragma pack()



static	UCHAR	BitMask[8] =
{
	0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
};


NTSTATUS	_AddDevice(
	IN	PDRIVER_OBJECT	_pDriverObject ,
	IN	PDEVICE_OBJECT	_pPhysicalDeviceObject
);

VOID	_BitmapFree(
	IN	PBITMAP	_pBitmap
);

NTSTATUS	_BitmapGet(
	IN	PBITMAP	_pBitmap ,
	IN	LARGE_INTEGER	_liOffset ,
	IN	ULONG	_Length ,
	IN OUT	PVOID	_pInOutBuffer ,
	IN	PVOID	_pInBuffer
);

NTSTATUS	_BitmapInit(
	OUT	PBITMAP*	_ppBitmap ,
	IN	ULONG	_PerSectorSize ,
	IN	ULONG	_Bits_Of_Byte ,
	IN	ULONG	_Bytes_Of_Region ,
	IN	ULONG	_RegionNumber
);

NTSTATUS	_BitmapSet(
	IN OUT	PBITMAP	_pBitmap ,
	IN	LARGE_INTEGER	_liOffset ,
	IN	ULONG	_Length
);

LONG	_BitmapTest(
	PBITMAP	_pBitmap ,
	LARGE_INTEGER	_liOffset ,
	ULONG	_Length
);

VOID	_BootReinitializationRoutine(
	IN	PDRIVER_OBJECT	_pDriverObject ,
	IN	PVOID	_Context ,
	IN	ULONG	Count
);

NTSTATUS	_DefaultDispatch(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
);

NTSTATUS	_DeviceControlDispatch(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
);

NTSTATUS	_ForwardIrpSync(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
);

NTSTATUS	_PnpDispatch(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
);

NTSTATUS	_PnpIrpCompletionRoutine(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp ,
	IN	PVOID	_Context
);

NTSTATUS	_PowerDispatch(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
);

NTSTATUS	_QueryVolumeInformation(
	PDEVICE_OBJECT	_pDeviceObject ,
	PLARGE_INTEGER	_liTotalSize ,
	PDWORD	_pdwPerClusterSize ,
	PDWORD	_pdwPerSectorSize
);

NTSTATUS	_QueryVolumeInformationCompletionRoutine(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp ,
	IN	PVOID	_Context
);

NTSTATUS	_ReadWriteDispatch(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp
);

VOID	_ReadWriteThreadEntry(
	IN	PVOID	_Context
);

VOID	_UnloadDispatch(
	IN	PDRIVER_OBJECT	_pDriverObject
);

NTSTATUS	_VolumeOnlineCompletionRoutine(
	IN	PDEVICE_OBJECT	_pDeviceObject ,
	IN	PIRP	_pIrp ,
	IN	PVOLUME_ONLINE_CONTEXT	_pVolumeContext
);

NTSTATUS	DriverEntry(
	IN	PDRIVER_OBJECT	_pDriverObject ,
	IN	PUNICODE_STRING	_pRegistryPath
);


#endif // !_DISK_PROTECT_H
