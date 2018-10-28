#pragma once
#ifndef _DISK_PROTECT_H
#define	_DISK_PROTECT_H

#define	BITS_ALL_CLEAR	0x1
#define	BITS_ALL_SET	0x2
#define	BITS_BLEND		0x3


#pragma	pack(1)
typedef	struct _BITMAP
{
	//������ÿ�������ж����ֽڣ�Ҳ��bitmap��һ��λ����Ӧ���ֽ���
	ULONG	PerSectorSize;

	//ÿ���ֽ��ж���λ��һ��8λ
	ULONG	Bits_Of_Byte;

	//ÿ����ռ�����ֽ�
	ULONG	Bytes_Of_Region;

	//���bitmap�ܹ��ж��ٸ���
	ULONG	RegionNumber;

	//ÿ�����Ӧ���ٴ��̵��ֽ�
	//��PerSectorSize * Bits_Of_Byte * Bytes_Of_Region
	ULONGLONG	PerRegionSizeMapped;

	//���Bitmap��Ӧ��ʵ�ʵĴ����ֽ���
	//��PerSectorSize * Bits_Of_Byte * Bytes_Of_Region * RegionNumber
	ULONGLONG	BitmapSizeMapped;

	//���ڴ�ȡbitmap����
	PVOID	BitmapLock;

	//ָ��bitmap�洢�ռ��ָ�룬��һ��ָ�����飬������RegionNumber��Ԫ�أ�ÿһ��Ԫ�ض���һ����ַ
	//ÿ����ַָ��һ��Bytes_Of_Region��С�Ŀ飬���е�ÿ���ֽڶ���8λ��ÿһλ��������ϵ�һ������
	UCHAR**	bitmap;

}BITMAP , *PBITMAP;
#pragma	pack()


typedef	struct _Device_Extension
{
	//������֣�����c:��d:��ĸ����
	WCHAR	VolumeLetter;
	//Ŀ����豸�Ƿ��ڱ���״̬
	BOOL	bProtect;
	//Ŀ�����ܴ�С�����ֽ�Ϊ��λ
	LARGE_INTEGER	liVolumeTotalSize;
	//Ŀ������ļ�ϵͳ��ÿ�ش�С�����ֽ�Ϊ��λ
	DWORD	dwPerClusterSize;
	//Ŀ����ÿ�������Ĵ�С,���ֽ�Ϊ��λ
	DWORD	dwPerSectorSize;
	//Ŀ����豸��Ӧ�Ĺ����豸����
	PDEVICE_OBJECT	pFilterDeviceObject;
	//Ŀ����豸��Ӧ�Ĺ����豸���²��豸����
	PDEVICE_OBJECT	pLowerDeviceObject;
	//Ŀ����豸��Ӧ�������豸����
	PDEVICE_OBJECT	pPhysicalDeviceObject;
	//������ݽṹ�Ƿ��Ѿ���ʼ�������
	BOOL	bIsInitialized;
	//Ϊ����Ŀ���ʹ�õ�λͼ�ṹ��ָ��
	PBITMAP	pBitmap;
	//����ת������ʱ�ļ����
	HANDLE	hTempFile;
	//Ϊ����Ŀ���IRPʹ�õ��������
	LIST_ENTRY	RequestList;
	//Ϊͬ����������ʹ�õ�������
	KSPIN_LOCK	RequestSpinLock;
	//����֪ͨ�����߳����������������Ҫ�����ʹ�õ��¼�
	KEVENT	RequestEvent;
	//�����̵߳ľ��
	PVOID	hThreadHandle;
	//����֪ͨ�����߳��˳��ı��
	BOOLEAN	ThreadExitFlag;
	//Ŀ���ػ���ҳ��Դ����ļ����¼�
	KEVENT	PagingPathCountEvent;
	//Ŀ���ػ���ҳ��Դ����ļ���
	ULONG	PagingPathCount;
} DEVICE_EXTENSION , *PDEVICE_EXTENSION;


typedef struct _VOLUME_ONLINE_CONTEXT_
{
	//��volume_online��DeviceIoControl�д�����ɺ������豸��չ
	PDEVICE_EXTENSION	pDevExt;
	//��volume_online��DeviceIoControl�д�����ɺ�����ͬ���¼�
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
