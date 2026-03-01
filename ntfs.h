#pragma once
#include <Windows.h>

typedef LONGLONG VCN;
typedef VCN *PVCN;
typedef LONGLONG LCN;
typedef int64_t SegmentNumber;
typedef ULONGLONG LSN, *PLSN;


//http://ntfs.com/ntfs-partition-boot-sector.htm
//https://kcall.co.uk/ntfs/
typedef struct BIOS_PARAMETER_BLOCK2 {
	USHORT BytesPerSector;
	UCHAR  SectorsPerCluster;
	USHORT ReservedSectors;

	UCHAR  Fats;               // Number of FATs         : always 0
	USHORT RootEntries;        // Root directory entries : always 0
	USHORT Sectors;            // Total logical sectors  : always 0
	UCHAR  Media;              // Media descriptor
	USHORT SectorsPerFat;

	USHORT SectorsPerTrack;
	USHORT Heads;              // Number of heads
	ULONG  HiddenSectors;      // 

	ULONG  LargeSectors;       // Large total logical sectors
	ULONG  Unused2;
	UINT64 TotalSectors;
	UINT64 MftStartLcn;
	UINT64 Mft2StartLcn;

	UCHAR ClustersPerFileRecordSegment;	//Negative values mean powers of 2 in bytes.
	UCHAR Unused3[3];

	UCHAR ClustersPerIndexBuffer;
	UCHAR Unused4[3];

	UINT64 VolumeSerialNumber;
	ULONG Checksum;
} BIOS_PARAMETER_BLOCK2;




//https://learn.microsoft.com/en-us/windows/win32/devnotes/mft-segment-reference
typedef struct _MFT_SEGMENT_REFERENCE {
  ULONG  SegmentNumberLowPart;
  USHORT SegmentNumberHighPart;
  USHORT SequenceNumber;
} MFT_SEGMENT_REFERENCE, *PMFT_SEGMENT_REFERENCE;

//https://learn.microsoft.com/en-us/windows/win32/devnotes/mft-segment-reference
typedef MFT_SEGMENT_REFERENCE FILE_REFERENCE, *PFILE_REFERENCE;

//https://learn.microsoft.com/en-us/windows/win32/devnotes/multi-sector-header
typedef struct _MULTI_SECTOR_HEADER {
  UCHAR  Signature[4];
  USHORT UpdateSequenceArrayOffset;
  USHORT UpdateSequenceArraySize;
} MULTI_SECTOR_HEADER, *PMULTI_SECTOR_HEADER;

//https://learn.microsoft.com/en-us/windows/win32/devnotes/file-record-segment-header
//This structure definition is valid only for major version 3 and minor version 0 or 1, as reported by FSCTL_GET_NTFS_VOLUME_DATA.
typedef struct _FILE_RECORD_SEGMENT_HEADER {
  MULTI_SECTOR_HEADER   MultiSectorHeader;
  LSN					Lsn;						// Log File Sequence Number
  USHORT                SequenceNumber;
  USHORT				ReferenceCount;
  USHORT                FirstAttributeOffset;
  USHORT                Flags;						//  FILE_xxx flags.
  ULONG					FirstFreeByte;				// In this segment, for attribute storage.
  ULONG					BytesAvailable;				// -- // --
  FILE_REFERENCE        BaseFileRecordSegment;
  USHORT				NextAttributeInstance;
//  UPDATE_SEQUENCE_ARRAY UpdateSequenceArray;
} FILE_RECORD_SEGMENT_HEADER, *PFILE_RECORD_SEGMENT_HEADER;

//
//  FILE_xxx flags.
//

#define FILE_RECORD_SEGMENT_IN_USE       (0x0001)
#define FILE_FILE_NAME_INDEX_PRESENT     (0x0002)



//
//  System File Numbers.
//

#define MASTER_FILE_TABLE_NUMBER         (0)   //  $Mft
#define MASTER_FILE_TABLE2_NUMBER        (1)   //  $MftMirr
#define LOG_FILE_NUMBER                  (2)   //  $LogFile
#define VOLUME_DASD_NUMBER               (3)   //  $Volume
#define ATTRIBUTE_DEF_TABLE_NUMBER       (4)   //  $AttrDef
#define ROOT_FILE_NAME_INDEX_NUMBER      (5)   //  .
#define BIT_MAP_FILE_NUMBER              (6)   //  $BitMap
#define BOOT_FILE_NUMBER                 (7)   //  $Boot
#define BAD_CLUSTER_FILE_NUMBER          (8)   //  $BadClus
#define QUOTA_TABLE_NUMBER               (9)   //  $Quota
#define UPCASE_TABLE_NUMBER              (10)  //  $UpCase
#define CAIRO_NUMBER                     (11)  //  $Cairo
#define FIRST_USER_FILE_NUMBER           (16)


//
//  Attribute Type Codes.
//

typedef ULONG ATTRIBUTE_TYPE_CODE;
typedef ATTRIBUTE_TYPE_CODE *PATTRIBUTE_TYPE_CODE;

#define $UNUSED                          (0X0)

#define $STANDARD_INFORMATION            (0x10)
#define $ATTRIBUTE_LIST                  (0x20)
#define $FILE_NAME                       (0x30)
#define $OBJECT_ID                       (0x40)
#define $SECURITY_DESCRIPTOR             (0x50)
#define $VOLUME_NAME                     (0x60)
#define $VOLUME_INFORMATION              (0x70)
#define $DATA                            (0x80)
#define $INDEX_ROOT                      (0x90)
#define $INDEX_ALLOCATION                (0xA0)
#define $BITMAP                          (0xB0)
#define $SYMBOLIC_LINK                   (0xC0)
#define $EA_INFORMATION                  (0xD0)
#define $EA                              (0xE0)
#ifdef _CAIRO_
#define $PROPERTY_SET                    (0xF0)
#endif  //  _CAIRO_
#define $FIRST_USER_DEFINED_ATTRIBUTE    (0x100)
#define $END                             (0xFFFFFFFF)

//https://learn.microsoft.com/en-us/windows/win32/devnotes/attribute-record-header
typedef struct _ATTRIBUTE_RECORD_HEADER {
	ATTRIBUTE_TYPE_CODE TypeCode;
	ULONG               RecordLength;
	UCHAR               FormCode;
	UCHAR               NameLength;
	USHORT              NameOffset;
	USHORT              Flags;
	USHORT              Instance;
	union {
		struct {
			ULONG  ValueLength;
			USHORT ValueOffset;
			UCHAR  ResidentFlags;	//  RESIDENT_FORM_xxx Flags.
			UCHAR  Reserved1;
		} Resident;
		struct {
			VCN      LowestVcn;
			VCN      HighestVcn;
			USHORT   MappingPairsOffset;
			UCHAR    Reserved[6];
			LONGLONG AllocatedLength;
			LONGLONG FileSize;
			LONGLONG ValidDataLength;
			LONGLONG TotalAllocated;
		} Nonresident;
	} Form;
} ATTRIBUTE_RECORD_HEADER, *PATTRIBUTE_RECORD_HEADER;

//
//  Attribute Form Codes
//

#define RESIDENT_FORM                    (0x00)
#define NONRESIDENT_FORM                 (0x01)

//
//  Define Attribute Flags
//

//
//  The first range of flag bits is reserved for
//  storing the compression method.  This constant
//  defines the mask of the bits reserved for
//  compression method.  It is also the first
//  illegal value, since we increment it to calculate
//  the code to pass to the Rtl routines.  Thus it is
//  impossible for us to store COMPRESSION_FORMAT_DEFAULT.
//

#define ATTRIBUTE_FLAG_COMPRESSION_MASK  (0x00FF)
#define ATTRIBUTE_FLAG_SPARSE            (0x8000)
#define ATTRIBUTE_FLAG_ENCRYPTED		 (0x4000)

//
//  RESIDENT_FORM_xxx flags
//

//
//  This attribute is indexed.
//

#define RESIDENT_FORM_INDEXED            (0x01)

//
//  The maximum attribute name length is 255 (in chars)
//

#define NTFS_MAX_ATTR_NAME_LEN           (255)


typedef struct _DUPLICATED_INFORMATION {

	//
	//  File creation time.
	//

	LONGLONG CreationTime;                                          //  offset = 0x000

																	//
																	//  Last time the DATA attribute was modified.
																	//

	LONGLONG LastModificationTime;                                  //  offset = 0x008

																	//
																	//  Last time any attribute was modified.
																	//

	LONGLONG LastChangeTime;                                        //  offset = 0x010

																	//
																	//  Last time the file was accessed.  This field may not always
																	//  be updated (write-protected media), and even when it is
																	//  updated, it may only be updated if the time would change by
																	//  a certain delta.  It is meant to tell someone approximately
																	//  when the file was last accessed, for purposes of possible
																	//  file migration.
																	//

	LONGLONG LastAccessTime;                                        //  offset = 0x018

																	//
																	//  Allocated Length of the file in bytes.  This is obviously
																	//  an even multiple of the cluster size.  (Not present if
																	//  LowestVcn != 0.)
																	//

	LONGLONG AllocatedLength;                                       //  offset = 0x020

																	//
																	//  File Size in bytes (highest byte which may be read + 1).
																	//  (Not present if LowestVcn != 0.)
																	//

	LONGLONG FileSize;                                              //  offset = 0x028

																	//
																	//  File attributes.  The first byte is the standard "Fat"
																	//  flags for this file.
																	//

	ULONG FileAttributes;                                           //  offset = 0x030

																	//
																	//  The size of buffer needed to pack these Ea's
																	//

	USHORT PackedEaSize;                                            //  offset = 0x034

																	//
																	//  Reserved for quad word alignment
																	//

	USHORT Reserved;                                                //  offset = 0x036

} DUPLICATED_INFORMATION;                                           //  sizeof = 0x038
typedef DUPLICATED_INFORMATION *PDUPLICATED_INFORMATION;

//
//  File Name attribute.  A file has one File Name attribute for
//  every directory it is entered into (hard links).
//

typedef struct _FILE_NAME {
	//
	//  This is a File Reference to the directory file which indexes
	//  to this name.
	//
	FILE_REFERENCE ParentDirectory;                                 //  offset = 0x000
	DUPLICATED_INFORMATION Info;                                    //  offset = 0x008

																	//
																	//  Length of the name to follow, in (Unicode) characters.
																	//

	UCHAR FileNameLength;                                           //  offset = 0x040

																	//
																	//  FILE_NAME_xxx flags
																	//

	UCHAR Flags;                                                    //  offset = 0x041

																	//
																	//  First character of Unicode File Name
																	//

	WCHAR FileName[1];                                              //  offset = 0x042

} FILE_NAME;
typedef FILE_NAME *PFILE_NAME;

//
//  File Name flags
//

#define FILE_NAME_NTFS                   (0x01)
#define FILE_NAME_DOS                    (0x02)

//
//  The maximum file name length is 255 (in chars)
//

#define NTFS_MAX_FILE_NAME_LENGTH       (255)

//
//  The maximum number of links on a file is 1024
//

#define NTFS_MAX_LINK_COUNT             (1024)

//
//  This flag is not part of the disk structure, but is defined here
//  to explain its use and avoid possible future collisions.  For
//  enumerations of "directories" this bit may be set to convey to
//  the collating routine that it should not match file names that
//  only have the FILE_NAME_DOS bit set.
//

#define FILE_NAME_IGNORE_DOS_ONLY        (0x80)

#define NtfsFileNameSizeFromLength(LEN) (                   \
    (sizeof( FILE_NAME) + LEN - 2)                          \
)

#define NtfsFileNameSize(PFN) (                             \
    (sizeof( FILE_NAME ) + ((PFN)->FileNameLength - 1) * 2) \
)
