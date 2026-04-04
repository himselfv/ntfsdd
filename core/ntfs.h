#pragma once
#include <Windows.h>

typedef LONGLONG VCN;
typedef VCN *PVCN;
typedef LONGLONG LCN;
typedef LONGLONG SegmentNumber;
typedef ULONGLONG LSN, *PLSN;

typedef ULONG COLLATION_RULE;
typedef ULONG DISPLAY_RULE;


//http://ntfs.com/ntfs-partition-boot-sector.htm
//https://kcall.co.uk/ntfs/
//https://thestarman.pcministry.com/asm/mbr/NTFSBR.htm#BPB
#pragma pack(push, 1)
typedef struct BIOS_PARAMETER_BLOCK {
	UCHAR  JumpInstruction[3];
	UCHAR  OEMID[8]; //"NTFS    "

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
} BIOS_PARAMETER_BLOCK;
#pragma pack(pop)

static constexpr USHORT BIOS_PARAMETER_BLOCK_SIZE_FOR_CHECKSUM = 0x80;

//ULONG BpbChecksum(void* buf);



//https://learn.microsoft.com/en-us/windows/win32/devnotes/mft-segment-reference
typedef struct _MFT_SEGMENT_REFERENCE {
	union {
		struct CLASSIC_SEGMENT_REFERENCE {
			ULONG  SegmentNumberLowPart;
			USHORT SegmentNumberHighPart;
			USHORT SequenceNumber;
		} classic;
		ULONGLONG mergedValue;
	};
	inline ULONGLONG segmentNumber() const { return classic.SegmentNumberLowPart + (classic.SegmentNumberHighPart << sizeof(ULONG)); }
} MFT_SEGMENT_REFERENCE, *PMFT_SEGMENT_REFERENCE;

//https://learn.microsoft.com/en-us/windows/win32/devnotes/mft-segment-reference
typedef MFT_SEGMENT_REFERENCE FILE_REFERENCE, *PFILE_REFERENCE;


//https://learn.microsoft.com/en-us/windows/win32/devnotes/multi-sector-header
typedef struct _MULTI_SECTOR_HEADER {
  UCHAR  Signature[4];
  USHORT UpdateSequenceArrayOffset;
  USHORT UpdateSequenceArraySize;
} MULTI_SECTOR_HEADER, *PMULTI_SECTOR_HEADER;

static const UCHAR SIGNATURE_FILE[4] = { 'F', 'I', 'L', 'E' };
static const UCHAR SIGNATURE_INDX[4] = { 'I', 'N', 'D', 'X' };



typedef USHORT UPDATE_SEQUENCE_NUMBER, *PUPDATE_SEQUENCE_NUMBER;
typedef UPDATE_SEQUENCE_NUMBER UPDATE_SEQUENCE_ARRAY[1];


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
	inline char* ResidentValuePtr() {
		return (char*)this + this->Form.Resident.ValueOffset;
	};
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


//
//  Standard Information Attribute.  This attribute is present in
//  every base file record, and must be resident.
//
typedef struct _STANDARD_INFORMATION {

    //  File creation time.
    LONGLONG CreationTime;                                          //  offset = 0x000

    //  Last time the DATA attribute was modified.
    LONGLONG LastModificationTime;                                  //  offset = 0x008

    //  Last time any attribute was modified.
    LONGLONG LastChangeTime;                                        //  offset = 0x010

    //  Last time the file was accessed.  This field may not always
    //  be updated (write-protected media), and even when it is
    //  updated, it may only be updated if the time would change by
    //  a certain delta.  It is meant to tell someone approximately
    //  when the file was last accessed, for purposes of possible
    //  file migration.
    LONGLONG LastAccessTime;                                        //  offset = 0x018

    //  File attributes.  The first byte is the standard "Fat"
    //  flags for this file.
    ULONG FileAttributes;                                           //  offset = 0x020

    //  Maximum file versions allowed for this file.  If this field
    //  is 0, then versioning is not enabled for this file.  If
    //  there are multiple files with the same version, then the
    //  value of Maximum file versions in the file with the highest
    //  version is the correct one.
    ULONG MaximumVersions;                                          //  offset = 0x024

    //  Version number for this file.
    ULONG VersionNumber;                                            //  offset = 0x028

//#ifdef _CAIRO_

    //  Class Id from the bidirectional Class Id index
    ULONG ClassId;                                                  //  offset = 0x02c

    //  Id for file owner, from bidir security index
    ULONG OwnerId;                                                  //  offset = 0x030

    //  SecurityId for the file - translates via bidir index to
    //  granted access Acl.
    ULONG SecurityId;                                               //  offset = 0x034

    //  Current amount of quota that has been charged for all the
    //  streams of this file.  Changed in same transaction with the
    //  quota file itself.
    ULONGLONG QuotaCharged;                                         //  offset = 0x038

    //  Update sequence number for this file.
    ULONGLONG Usn;                                                  //  offset = 0x040

//#else _CAIRO_

//    ULONG Reserved;                                                 //  offset = 0x02c

//#endif _CAIRO_


} STANDARD_INFORMATION;                                             //  sizeof = 0x048
typedef STANDARD_INFORMATION *PSTANDARD_INFORMATION;


//
//  Define the file attributes, starting with the Fat attributes.
//

#define FAT_DIRENT_ATTR_READ_ONLY        (0x01)
#define FAT_DIRENT_ATTR_HIDDEN           (0x02)
#define FAT_DIRENT_ATTR_SYSTEM           (0x04)
#define FAT_DIRENT_ATTR_VOLUME_ID        (0x08)
#define FAT_DIRENT_ATTR_ARCHIVE          (0x20)
#define FAT_DIRENT_ATTR_DEVICE           (0x40)



//  The Attributes List attribute is an ordered-list of quad-word
//  aligned ATTRIBUTE_LIST_ENTRY records.  It is ordered first by
//  Attribute Type Code, and then by Attribute Name (if present).
//  No two attributes may exist with the same type code, name and
//  LowestVcn.  This also means that at most one occurrence of a
//  given Attribute Type Code without a name may exist.
//
//  To binary search this attribute, it is first necessary to make a
//  quick pass through it and form a list of pointers, since the
//  optional name makes it variable-length.

// https://learn.microsoft.com/en-us/windows/win32/devnotes/attribute-list-entry

typedef struct _ATTRIBUTE_LIST_ENTRY {
    ATTRIBUTE_TYPE_CODE AttributeTypeCode;                          //  offset = 0x000

    //  Size of this record in bytes, including the optional name
    //  appended to this structure.
    USHORT RecordLength;                                            //  offset = 0x004

    //  Length of attribute name, if there is one.  If a name exists
    //  (AttributeNameLength != 0), then it is a Unicode string of
    //  the specified number of characters immediately following
    //  this record.
    UCHAR AttributeNameLength;                                      //  offset = 0x006

    //  Reserved to get to quad-word boundary
    UCHAR AttributeNameOffset;                                      //  offset = 0x007

    //  Lowest Vcn for this attribute.  This field is always zero
    //  unless the attribute requires multiple file record segments
    //  to describe all of its runs, and this is a reference to a
    //  segment other than the first one.  The field says what the
    //  lowest Vcn is that is described by the referenced segment.
    VCN LowestVcn;                                                  //  offset = 0x008

    //  Reference to the MFT segment in which the attribute resides.
    MFT_SEGMENT_REFERENCE SegmentReference;                         //  offset = 0x010

    //  The file-record-unique attribute instance number for this
    //  attribute.
    USHORT Instance;                                                //  offset = 0x018

    //  When creating an attribute list entry, start the name here.
    //  (When reading one, use the AttributeNameOffset field.)
    WCHAR AttributeName[1];                                         //  offset = 0x01A
} ATTRIBUTE_LIST_ENTRY;
typedef ATTRIBUTE_LIST_ENTRY *PATTRIBUTE_LIST_ENTRY;



typedef struct _DUPLICATED_INFORMATION {

	//  File creation time.
	LONGLONG CreationTime;                                          //  offset = 0x000

	//  Last time the DATA attribute was modified.
	LONGLONG LastModificationTime;                                  //  offset = 0x008

	//  Last time any attribute was modified.
	LONGLONG LastChangeTime;                                        //  offset = 0x010

	//  Last time the file was accessed.  This field may not always
	//  be updated (write-protected media), and even when it is
	//  updated, it may only be updated if the time would change by
	//  a certain delta.  It is meant to tell someone approximately
	//  when the file was last accessed, for purposes of possible
	//  file migration.
	LONGLONG LastAccessTime;                                        //  offset = 0x018

	//  Allocated Length of the file in bytes.  This is obviously
	//  an even multiple of the cluster size.  (Not present if
	//  LowestVcn != 0.)
	LONGLONG AllocatedLength;                                       //  offset = 0x020

	//  File Size in bytes (highest byte which may be read + 1).
	//  (Not present if LowestVcn != 0.)
	LONGLONG FileSize;                                              //  offset = 0x028

	//  File attributes.  The first byte is the standard "Fat"
	//  flags for this file.
	ULONG FileAttributes;                                           //  offset = 0x030

	//  The size of buffer needed to pack these Ea's
	USHORT PackedEaSize;                                            //  offset = 0x034

	//  Reserved for quad word alignment
	USHORT Reserved;                                                //  offset = 0x036

} DUPLICATED_INFORMATION;                                           //  sizeof = 0x038
typedef DUPLICATED_INFORMATION *PDUPLICATED_INFORMATION;

//
//  File Name attribute.  A file has one File Name attribute for
//  every directory it is entered into (hard links).
//
typedef struct _FILE_NAME {
	//  This is a File Reference to the directory file which indexes
	//  to this name.
	FILE_REFERENCE ParentDirectory;                                 //  offset = 0x000
	DUPLICATED_INFORMATION Info;                                    //  offset = 0x008

	//  Length of the name to follow, in (Unicode) characters.
	UCHAR FileNameLength;                                           //  offset = 0x040

	//  FILE_NAME_xxx flags
	UCHAR Flags;                                                    //  offset = 0x041

	//  First character of Unicode File Name
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

#pragma pack(push, 1)
typedef struct _VOLUME_INFORMATION {
	LONGLONG Reserved;
	UCHAR MajorVersion;                                             //  offset = 0x000
	UCHAR MinorVersion;                                             //  offset = 0x001
	USHORT VolumeFlags;                                             //  offset = 0x002
} VOLUME_INFORMATION;                                               //  sizeof = 0x004
typedef VOLUME_INFORMATION *PVOLUME_INFORMATION;
#pragma pack(pop)

#define VOLUME_DIRTY                     (0x0001)
#define VOLUME_RESIZE_LOG_FILE           (0x0002)


//
//  Common Index Header for Index Root and Index Allocation Buffers.
//  This structure is used to locate the Index Entries and describe
//  the free space in either of the two structures above.
//
typedef struct _INDEX_HEADER {

    //  Offset from the start of this structure to the first Index
    //  Entry.
    ULONG FirstIndexEntry;                                          //  offset = 0x000

    //  Offset from the start of the first index entry to the first
    //  (quad-word aligned) free byte.
    ULONG FirstFreeByte;                                            //  offset = 0x004

    //  Total number of bytes available, from the start of the first
    //  index entry.  In the Index Root, this number must always be
    //  equal to FirstFreeByte, as the total attribute record will
    //  be grown and shrunk as required.
    ULONG BytesAvailable;                                           //  offset = 0x008

    //  INDEX_xxx flags.
    UCHAR Flags;                                                    //  offset = 0x00C

    //  Reserved to round up to quad word boundary.
    UCHAR Reserved[3];                                              //  offset = 0x00D

} INDEX_HEADER;                                                     //  sizeof = 0x010
typedef INDEX_HEADER *PINDEX_HEADER;


//
//  INDEX_xxx flags
//

//  This Index or Index Allocation buffer is an intermediate node,
//  as opposed to a leaf in the Btree.  All Index Entries will have
//  a block down pointer.
#define INDEX_NODE                       (0x01)


//
//  Index Root attribute.  The index attribute consists of an index
//  header record followed by one or more index entries.
//
typedef struct _INDEX_ROOT {

    //  Attribute Type Code of the attribute being indexed.
    ATTRIBUTE_TYPE_CODE IndexedAttributeType;                       //  offset = 0x000

    //  Collation rule for this index.
    COLLATION_RULE CollationRule;                                   //  offset = 0x004

    //  Size of Index Allocation Buffer in bytes.
    ULONG BytesPerIndexBuffer;                                      //  offset = 0x008

    //  Size of Index Allocation Buffers in units of blocks.
    //  Blocks will be clusters when index buffer is equal or
    //  larger than clusters and log blocks for large
    //  cluster systems.
    UCHAR BlocksPerIndexBuffer;                                     //  offset = 0x00C

    //  Reserved to round to quad word boundary.
    UCHAR Reserved[3];                                              //  offset = 0x00D

    //  Index Header to describe the Index Entries which follow
    INDEX_HEADER IndexHeader;                                       //  offset = 0x010

} INDEX_ROOT;                                                       //  sizeof = 0x020
typedef INDEX_ROOT *PINDEX_ROOT;


//
//  Index Allocation record is used for non-root clusters of the
//  b-tree.  Each non root cluster is contained in the data part of
//  the index allocation attribute.  Each cluster starts with an
//  index allocation list header and is followed by one or more
//  index entries.
//
typedef struct _INDEX_ALLOCATION_BUFFER {

    //  Multi-Sector Header as defined by the Cache Manager.  This
    //  structure will always contain the signature "INDX" and a
    //  description of the location and size of the Update Sequence
    //  Array.
    MULTI_SECTOR_HEADER MultiSectorHeader;                          //  offset = 0x000

    //  Log File Sequence Number of last logged update to this Index
    //  Allocation Buffer.
    LSN Lsn;                                                        //  offset = 0x008

    //  We store the index block of this Index Allocation buffer for
    //  convenience and possible consistency checking.
    VCN ThisBlock;                                                  //  offset = 0x010

    //  Index Header to describe the Index Entries which follow
    INDEX_HEADER IndexHeader;                                       //  offset = 0x018

    //  Update Sequence Array to protect multi-sector transfers of
    //  the Index Allocation Buffer.
    UPDATE_SEQUENCE_ARRAY UpdateSequenceArray;                      //  offset = 0x028

} INDEX_ALLOCATION_BUFFER;
typedef INDEX_ALLOCATION_BUFFER *PINDEX_ALLOCATION_BUFFER;

//
//  Default size of index buffer and index blocks.
//
#define DEFAULT_INDEX_BLOCK_SIZE        (0x200)
#define DEFAULT_INDEX_BLOCK_BYTE_SHIFT  (9)

//
//  Index Entry.  This structure is common to both the resident
//  index list attribute and the Index Allocation records
//
typedef struct _INDEX_ENTRY {

    //  Define a union to distinguish directory indices from view indices
    union {

        //  Reference to file containing the attribute with this
        //  attribute value.
        FILE_REFERENCE FileReference;                               //  offset = 0x000

        //  For views, describe the Data Offset and Length in bytes
        struct {
            USHORT DataOffset;                                      //  offset = 0x000
            USHORT DataLength;                                      //  offset = 0x001
            ULONG ReservedForZero;                                  //  offset = 0x002
        };
    };

    //  Length of this index entry, in bytes.
    USHORT Length;                                                  //  offset = 0x008

    //  Length of attribute value, in bytes.  The attribute value
    //  immediately follows this record.
    USHORT AttributeLength;                                         //  offset = 0x00A

    //  INDEX_ENTRY_xxx Flags.
    USHORT Flags;                                                   //  offset = 0x00C

    //  Reserved to round to quad-word boundary.
    USHORT Reserved;                                                //  offset = 0x00E

    //  If this Index Entry is an intermediate node in the tree, as
    //  determined by the INDEX_xxx flags, then a VCN  is stored at
    //  the end of this entry at Length - sizeof(VCN).
	FILE_NAME FileName;

} INDEX_ENTRY;                                                      //  sizeof = 0x010
typedef INDEX_ENTRY *PINDEX_ENTRY;

//
//  INDEX_ENTRY_xxx flags
//

//  This entry is currently in the intermediate node form, i.e., it
//  has a Vcn at the end.
#define INDEX_ENTRY_NODE                 (0x0001)

//  This entry is the special END record for the Index or Index
//  Allocation buffer.
#define INDEX_ENTRY_END                  (0x0002)
