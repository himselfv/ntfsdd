#pragma once
#include <string>
#include "ntfs.h"
#include "ntfsmft.h"

std::string binToHex(void* data, size_t sz, int lineSize);
void dumpRaw(void* data, size_t sz);
void dumpHex(void* data, size_t sz, int lineSize);

std::string segmentRefToStr(MFT_SEGMENT_REFERENCE& ref);
inline std::string flagsToStr(ULONG flags);
std::string segmentFlagsToString(USHORT flags);
std::string attrTypeToStr(ATTRIBUTE_TYPE_CODE type);
std::string attrFlagsToString(USHORT flags);
std::string fileNameFlagsToString(USHORT flags);
std::string fileAttributesToString(ULONG attrs);

void printMultiSectorHeader(MULTI_SECTOR_HEADER& header);
void printMultiSectorHeader(MULTI_SECTOR_HEADER& header, LSN& lsn);

std::string collationRuleToStr(COLLATION_RULE rule);


class DataPrinter
{
protected:
	Mft& mft;
	Volume& vol;
public:
	DataPrinter(Mft& mft);
	inline void printFilenameAttr(ATTRIBUTE_RECORD_HEADER& attr);
	void printFilenameAttr(FILE_NAME& data, int64_t size);
	void printDuplicatedInformation(const DUPLICATED_INFORMATION& info);
	inline void printStandardInformationAttr(ATTRIBUTE_RECORD_HEADER& attr);
	void printStandardInformationAttr(STANDARD_INFORMATION& sa, int64_t size);
	void printAttributeListAttr(ATTRIBUTE_RECORD_HEADER& attr);
	void printAttributeListAttr(byte* data, size_t len);
	void printAttr(ATTRIBUTE_RECORD_HEADER& attr);
	inline void printAttrResidentValue(ATTRIBUTE_RECORD_HEADER& attr);
	void printAttrResidentValue(ATTRIBUTE_TYPE_CODE attrType, byte* data, size_t size);
	void printUnknownAttr(byte* data, size_t size);
};

class SegmentPrinter : public DataPrinter
{
public:
	using DataPrinter::DataPrinter;

	void printSegment(FILE_RECORD_SEGMENT_HEADER* segment);
};

