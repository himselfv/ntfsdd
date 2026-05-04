#pragma once
#include "segmentprint.h"
#include "mftutil.h"
#include "mftdir.h"


std::string binToHex(void* data, size_t sz, int lineSize)
{
	const char* hex = "0123456789ABCDEF";
	char* ptr = (char*)data;
	std::string result {};
	while (sz > 0) {
		for (auto i = 0; i < (sz >= lineSize ? lineSize : sz); i++) {
			result += hex[(*ptr & 0xF0) >> 4];
			result += hex[(*ptr) & 0x0F];
			ptr++;
		}
		result += "\n";
		sz -= (sz >= lineSize ? lineSize : sz);
	}
	return result;
}

void dumpRaw(void* data, size_t sz)
{
	char* ptr = (char*)data;
	auto& cout = std::cout;
	while (sz > 0) {
		cout << *ptr;
		ptr++;
		sz--;
	}
	cout << std::endl;
}

void dumpHex(void* data, size_t sz, int lineSize)
{
	std::cout << binToHex(data, sz, lineSize) << std::endl;
}


std::string segmentRefToStr(MFT_SEGMENT_REFERENCE& ref)
{
	if (ref.mergedValue == 0)
		return "none";
	else
		return std::to_string(ref.segmentNumber()) + " rev" + std::to_string(ref.classic.SequenceNumber);
}

inline std::string flagsToStr(ULONG flags)
{
	return std::to_string(flags);
}

std::string segmentFlagsToString(USHORT flags)
{
	std::string result{ flagsToStr(flags) };
	if (flags & FILE_RECORD_SEGMENT_IN_USE) result += " IN_USE";
	if (flags & FILE_FILE_NAME_INDEX_PRESENT) result += " FILE_NAME_INDEX_PRESENT";
	return result;
}


std::string attrTypeToStr(ATTRIBUTE_TYPE_CODE type)
{
	switch (type) {
	case $STANDARD_INFORMATION: return "$STANDARD_INFORMATION"; break;
	case $ATTRIBUTE_LIST: return "$ATTRIBUTE_LIST"; break;
	case $FILE_NAME: return "$FILE_NAME"; break;
	case $OBJECT_ID: return "$OBJECT_ID"; break;
	case $SECURITY_DESCRIPTOR: return "$SECURITY_DESCRIPTOR"; break;
	case $VOLUME_NAME: return "$VOLUME_NAME"; break;
	case $VOLUME_INFORMATION: return "$VOLUME_INFORMATION"; break;
	case $DATA: return "$DATA"; break;
	case $INDEX_ROOT: return "$INDEX_ROOT"; break;
	case $INDEX_ALLOCATION: return "$INDEX_ALLOCATION"; break;
	case $BITMAP: return "$BITMAP"; break;
	case $SYMBOLIC_LINK: return "$SYMBOLIC_LINK"; break;
	case $EA_INFORMATION: return "$EA_INFORMATION"; break;
	case $EA: return "$EA"; break;
	default:
		return std::string{ "ATTR$" }+std::to_string(type);
	}
}

std::string attrFlagsToString(USHORT flags)
{
	std::string result{ flagsToStr(flags) };
	if (flags & ATTRIBUTE_FLAG_COMPRESSION_MASK) result += " COMPRESSION_MASK";
	if (flags & ATTRIBUTE_FLAG_SPARSE) result += " SPARSE";
	if (flags & ATTRIBUTE_FLAG_ENCRYPTED) result += " ENCRYPTED";
	return result;
}

std::string fileNameFlagsToString(USHORT flags)
{
	std::string result{};
	if (flags & FILE_NAME_NTFS) result += " NTFS";
	if (flags & FILE_NAME_DOS) result += " DOS";
	flags &= ~(FILE_NAME_NTFS | FILE_NAME_DOS);
	if (flags != 0)
		result += " FLAGS_" + flagsToStr(flags);
	return result;
}

std::string fileAttributesToString(ULONG attrs)
{
	std::string result{};
	if (attrs & FAT_DIRENT_ATTR_READ_ONLY) result += " READ_ONLY";
	if (attrs & FAT_DIRENT_ATTR_HIDDEN) result += " HIDDEN";
	if (attrs & FAT_DIRENT_ATTR_SYSTEM) result += " SYSTEM";
	if (attrs & FAT_DIRENT_ATTR_VOLUME_ID) result += " VOLUME_ID";
	if (attrs & FAT_DIRENT_ATTR_ARCHIVE) result += " ARCHIVE";
	if (attrs & FAT_DIRENT_ATTR_DEVICE) result += " DEVICE";
	attrs &= ~(FAT_DIRENT_ATTR_READ_ONLY | FAT_DIRENT_ATTR_HIDDEN | FAT_DIRENT_ATTR_SYSTEM | FAT_DIRENT_ATTR_VOLUME_ID | FAT_DIRENT_ATTR_ARCHIVE | FAT_DIRENT_ATTR_DEVICE);
	if (attrs != 0)
		result += " ATTRS_" + flagsToStr(attrs);
	return result;
}

std::string collationRuleToStr(COLLATION_RULE rule)
{
	return std::to_string(rule);
}


DataPrinter::DataPrinter(Mft& mft)
	: mft(mft), vol(*mft.vol)
{}

void DataPrinter::printDuplicatedInformation(const DUPLICATED_INFORMATION& info)
{
	std::cout << "  File size: " << info.FileSize << ", allocated: " << info.AllocatedLength << std::endl;
	std::cout << "  Attributes: " << fileAttributesToString(info.FileAttributes) << std::endl;
	std::cout << "  Times: CRE: " << info.CreationTime << " MOD: " << info.LastModificationTime
		<< " CHG: " << info.LastChangeTime << " ACC: " << info.LastAccessTime << std::endl;
	std::cout << "  Packed EA: " << info.PackedEaSize << " Reserved: " << info.Reserved << std::endl;
}

inline void DataPrinter::printFilenameAttr(ATTRIBUTE_RECORD_HEADER& attr)
{
	return printFilenameAttr(*((FILE_NAME*)attr.ResidentValuePtr()), attr.Form.Resident.ValueLength);
}

void DataPrinter::printFilenameAttr(FILE_NAME& data, int64_t size)
{
	AttrFilename fn{ &data };
	std::cout << "  Filename: " << fn.name() << std::endl;
	std::cout << "  Parent dir: " << segmentRefToStr(fn.fn->ParentDirectory) << std::endl;
	std::cout << "  Flags: " << fileNameFlagsToString((USHORT)(fn.fn->Flags));
	std::cout << std::endl;
	std::cout << "  Duplicated information:" << std::endl;
	this->printDuplicatedInformation(fn.fn->Info);
}

inline void DataPrinter::printStandardInformationAttr(ATTRIBUTE_RECORD_HEADER& attr)
{
	return printStandardInformationAttr(*((STANDARD_INFORMATION*)attr.ResidentValuePtr()), attr.Form.Resident.ValueLength);
}

void DataPrinter::printStandardInformationAttr(STANDARD_INFORMATION& sa, int64_t size)
{
	std::cout << "  Creation: " << sa.CreationTime;
	std::cout << " LastMod: " << sa.LastModificationTime;
	std::cout << " LastChange: " << sa.LastChangeTime;
	std::cout << " LastAcc: " << sa.LastAccessTime;
	std::cout << std::endl;

	std::cout << "  FileAttrs: " << fileAttributesToString(sa.FileAttributes) << std::endl;
	std::cout << "  MaximumVersions: " << sa.MaximumVersions;
	std::cout << " VersionNumber: " << sa.VersionNumber << std::endl;

	if (size >= offsetof(STANDARD_INFORMATION, SecurityId) + sizeof(STANDARD_INFORMATION::SecurityId)) {
		std::cout << "  ClassId: " << sa.ClassId
			<< " OwnerId: " << sa.OwnerId
			<< " SecurityId: " << sa.SecurityId
			<< std::endl;
	}
	if (size >= offsetof(STANDARD_INFORMATION, QuotaCharged) + sizeof(STANDARD_INFORMATION::QuotaCharged)) {
		std::cout << "  QuotaCharged: " << sa.QuotaCharged << std::endl;
	}
	if (size >= offsetof(STANDARD_INFORMATION, Usn) + sizeof(STANDARD_INFORMATION::Usn)) {
		std::cout << "  Usn: " << sa.Usn << std::endl;
	}
}

void DataPrinter::printAttributeListAttr(ATTRIBUTE_RECORD_HEADER& attr)
{
	AttributeListProcessor proc(&vol);

	if (attr.FormCode == RESIDENT_FORM)
		proc.processResidentAttr(attr);
	else {
		if (attr.Form.Nonresident.LowestVcn != 0) {
			std::cout << "WARNING: $ATTRIBUTE_LIST chunk with LowestVcn!=0. This is very rare. In this simplified tool we cannot parse this." << std::endl;
			//We try to in the main ntfsdd though.
			return;
		}
		proc.addAttrChunk(&attr);
		proc.advance();
	}
	for (auto& entry : proc.segments)
		std::cout << "  Segment: " << entry << std::endl;
	if (!proc.eof())
		std::cout << "WARNING: Unprocessed data left in $ATTRIBUTE_LIST. Likely to be chunked $ATTRIBUTE_LIST. This is very rare. In this simplified tool we cannot parse this." << std::endl;
}

void DataPrinter::printAttributeListAttr(byte* data, size_t len)
{
	AttributeListProcessor proc(&vol);

	proc.processResidentAttr(data, len);
	for (auto& entry : proc.segments)
		std::cout << "  Segment: " << entry << std::endl;
	if (!proc.eof())
		std::cout << "WARNING: Unprocessed data left in $ATTRIBUTE_LIST." << std::endl;
}

void DataPrinter::printAttr(ATTRIBUTE_RECORD_HEADER& attr)
{
	std::cout << attrTypeToStr(attr.TypeCode) << " len=" << attr.RecordLength << " flags=" << attrFlagsToString(attr.Flags);
	std::cout << std::endl;
	std::cout << "  Instance: " << attr.Instance << std::endl;
	std::cout << "  Name: " << attrNameStr(&attr) << std::endl;
	if (attr.FormCode == RESIDENT_FORM) {
		std::cout << "  Resident: Data=" << attr.Form.Resident.ValueOffset << "+" << attr.Form.Resident.ValueLength << ", Flags=" << ((USHORT)attr.Form.Resident.ResidentFlags) << std::endl;
		printAttrResidentValue(attr);
	}
	else if (attr.FormCode == NONRESIDENT_FORM) {
		std::cout << "  Non-resident: VCN=" << attr.Form.Nonresident.LowestVcn << "-" << attr.Form.Nonresident.HighestVcn << std::endl;
		std::cout << "  Size=" << attr.Form.Nonresident.FileSize << ", Valid=" << attr.Form.Nonresident.ValidDataLength
			<< ", Alloc=" << attr.Form.Nonresident.AllocatedLength << ", Total=" << attr.Form.Nonresident.TotalAllocated
			<< std::endl
			;
		for (auto& run : DataRunIterator(&attr))
			std::cout << "    Run: " << run.offset << "+" << run.length << std::endl;
	}
	else {
		std::cout << "  UNKNOWN FORM: " << attr.FormCode << std::endl;
	}
}

inline void DataPrinter::printAttrResidentValue(ATTRIBUTE_RECORD_HEADER& attr)
{
	printAttrResidentValue(attr.TypeCode, (byte*)(attr.ResidentValuePtr()), attr.Form.Resident.ValueLength);
}

void DataPrinter::printAttrResidentValue(ATTRIBUTE_TYPE_CODE attrType, byte* data, size_t size)
{
	//Dump extended info on some attributes
	switch (attrType) {
	case $FILE_NAME: printFilenameAttr(*(FILE_NAME*)data, size); break;
	case $STANDARD_INFORMATION: printStandardInformationAttr(*(STANDARD_INFORMATION*)data, size); break;
	case $ATTRIBUTE_LIST: printAttributeListAttr(data, size); break;
	default:
		printUnknownAttr(data, size);
	}
}

void DataPrinter::printUnknownAttr(byte* data, size_t size)
{
	std::cout << binToHex(data, size, MAXINT) << std::endl;
}

void printMultiSectorHeader(MULTI_SECTOR_HEADER& header)
{
	std::cout << header.Signature;
}

void printMultiSectorHeader(MULTI_SECTOR_HEADER& header, LSN& lsn)
{
	printMultiSectorHeader(header);
	std::cout << ", LSN: " << lsn;
}

void SegmentPrinter::printSegment(FILE_RECORD_SEGMENT_HEADER* segment)
{
	printMultiSectorHeader(segment->MultiSectorHeader, segment->Lsn);
	std::cout << ", BaseSegment: " << segmentRefToStr(segment->BaseFileRecordSegment) << std::endl;
	std::cout << "SequenceNumber: " << segment->SequenceNumber << " ReferenceCount: " << segment->ReferenceCount << std::endl;

	std::cout << "Flags: " << segmentFlagsToString(segment->Flags);
	std::cout << std::endl;

	std::cout << "Update sequence: offset=" << segment->MultiSectorHeader.UpdateSequenceArrayOffset
		<< ", size=" << segment->MultiSectorHeader.UpdateSequenceArraySize << std::endl;
	std::cout << "Bytes: available=" << segment->BytesAvailable << ", firstFree=" << segment->FirstFreeByte << std::endl;
	std::cout << "Attrs: firstOffset=" << segment->FirstAttributeOffset << ", nextInst=" << segment->NextAttributeInstance << std::endl;

	for (ATTRIBUTE_RECORD_HEADER& attr : AttributeIterator(segment))
		printAttr(attr);
}
