#pragma once
#include <Windows.h>

typedef LONGLONG VCN;
typedef VCN *PVCN;
typedef LONGLONG LCN;
typedef int64_t SegmentNumber;
typedef ULONGLONG LSN, *PLSN;


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
  LSN					Lsn;
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
