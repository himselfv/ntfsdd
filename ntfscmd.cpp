#include <windows.h>
#include <string>
#include <unordered_set>
#include <cinttypes>
#include "CLI11helper.h"
#include "ntfs.h"
#include "util.h"
#include "vssutil.h"
#include "ntfsvolume.h"
#include "ntfsmft.h"
#include "mftdir.h"
#include "mftutil.h"
#include "bitmap.h"
/*
--dump-segment
--print-segment
--dump-cluster
--list-dir
*/


//Volume + its MFT.
class Volume2 : public Volume {
public:
	Mft* mft = nullptr;
	using Volume::Volume;
	~Volume2() {
		if (mft)
			delete mft;
	}
	void initMft() {
		this->mft = new Mft(this);
	}
};

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
	const char* hex = "0123456789ABCDEF";
	char* ptr = (char*)data;
	auto& cout = std::cout;
	while (sz > 0) {
		for (auto i = 0; i < (sz >= lineSize ? lineSize : sz); i++) {
			cout << hex[(*ptr & 0xF0) >> 4] << hex[(*ptr) & 0x0F];
			ptr++;
		}
		cout << std::endl;
		sz -= (sz >= lineSize ? lineSize : sz);
	}
}

std::string segmentRefToStr(MFT_SEGMENT_REFERENCE& ref)
{
	if (ref.mergedValue == 0)
		return "none";
	else
		return std::to_string(ref.segmentNumber()) + " rev" + std::to_string(ref.classic.SequenceNumber);
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

std::string fileAttributesToString(ULONG attrs)
{
	std::string result {};
	if (attrs & FAT_DIRENT_ATTR_READ_ONLY) result += "READ_ONLY ";
	if (attrs & FAT_DIRENT_ATTR_HIDDEN) result += "HIDDEN ";
	if (attrs & FAT_DIRENT_ATTR_SYSTEM) result += "SYSTEM ";
	if (attrs & FAT_DIRENT_ATTR_VOLUME_ID) result += "VOLUME_ID ";
	if (attrs & FAT_DIRENT_ATTR_ARCHIVE) result += "ARCHIVE ";
	if (attrs & FAT_DIRENT_ATTR_DEVICE) result += "DEVICE ";
	return result;
}


class SegmentPrinter
{
protected:
	Volume2& vol;
public:
	SegmentPrinter(Volume2& vol)
		: vol(vol)
	{}

	void printFilenameAttr(ATTRIBUTE_RECORD_HEADER& attr)
	{
		AttrFilename fn{ &attr };
		std::cout << "  Filename: " << AttrFilename(&attr).name() << std::endl;
		std::cout << "  Parent dir: " << segmentRefToStr(fn.fn->ParentDirectory) << std::endl;
		std::cout << "  Flags: " << (USHORT)(fn.fn->Flags);
		if (fn.fn->Flags & FILE_NAME_NTFS) std::cout << " NTFS";
		if (fn.fn->Flags & FILE_NAME_DOS) std::cout << " DOS";
		std::cout << std::endl;
	}

	void printStandardInformationAttr(ATTRIBUTE_RECORD_HEADER& attr)
	{
		int64_t size = attr.Form.Resident.ValueLength;
		auto& sa = *((STANDARD_INFORMATION*)attr.ResidentValuePtr());

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

	void printAttributeListAttr(ATTRIBUTE_RECORD_HEADER& attr)
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

	void printAttr(ATTRIBUTE_RECORD_HEADER& attr)
	{
		std::cout << attrTypeToStr(attr.TypeCode) << " len=" << attr.RecordLength << " flags=" << attr.Flags;
		if (attr.Flags & ATTRIBUTE_FLAG_COMPRESSION_MASK) std::cout << " COMPRESSION_MASK";
		if (attr.Flags & ATTRIBUTE_FLAG_SPARSE) std::cout << " SPARSE";
		if (attr.Flags & ATTRIBUTE_FLAG_ENCRYPTED) std::cout << " ENCRYPTED";
		std::cout << std::endl;
		std::cout << "  Instance: " << attr.Instance << std::endl;
		std::cout << "  Name: " << attrNameStr(&attr) << std::endl;
		if (attr.FormCode == RESIDENT_FORM) {
			std::cout << "  Resident: Data=" << attr.Form.Resident.ValueOffset << "+" << attr.Form.Resident.ValueLength << ", Flags=" << ((USHORT)attr.Form.Resident.ResidentFlags) << std::endl;
			//Dump extended info on some attributes
			if (attr.TypeCode == $FILE_NAME)
				printFilenameAttr(attr);
			if (attr.TypeCode == $STANDARD_INFORMATION)
				printStandardInformationAttr(attr);
			if (attr.TypeCode == $ATTRIBUTE_LIST)
				printAttributeListAttr(attr);
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

	void printSegment(FILE_RECORD_SEGMENT_HEADER* segment)
	{
		std::cout << segment->MultiSectorHeader.Signature;
		std::cout << ", BaseSegment: " << segmentRefToStr(segment->BaseFileRecordSegment);
		std::cout << ", LSN: " << segment->Lsn << std::endl;

		std::cout << "SequenceNumber: " << segment->SequenceNumber << " ReferenceCount: " << segment->ReferenceCount << std::endl;

		std::cout << "Flags: " << std::to_string(segment->Flags);
		if (segment->Flags & FILE_RECORD_SEGMENT_IN_USE) std::cout << " IN_USE";
		if (segment->Flags & FILE_FILE_NAME_INDEX_PRESENT) std::cout << " FILE_NAME_INDEX_PRESENT";
		std::cout << std::endl;

		std::cout << "Update sequence: offset=" << segment->MultiSectorHeader.UpdateSequenceArrayOffset
			<< ", size=" << segment->MultiSectorHeader.UpdateSequenceArraySize << std::endl;
		std::cout << "Bytes: available=" << segment->BytesAvailable << ", firstFree=" << segment->FirstFreeByte << std::endl;
		std::cout << "Attrs: firstOffset=" << segment->FirstAttributeOffset << ", nextInst=" << segment->NextAttributeInstance << std::endl;

		for (ATTRIBUTE_RECORD_HEADER& attr : AttributeIterator(segment))
			printAttr(attr);
	}
};


class DirIndexPrinter
{
protected:
	DirectoryTreeLoader* dirTree = nullptr;
	//Protect against duplicate names in the same dir (very common) + against recursion.
	//Could have done the same on each dir's level (duplicates) + another set/stack for recursion, but whatever.
	std::unordered_set<SegmentNumber> visited {};


	void print(SegmentNumber segmentNo, const std::string& offset, bool recursive, bool force)
	{
		auto& dir = dirTree->get(segmentNo);
		if (!force && dir.children.empty())
			return; //A lot of them are files and not dirs so to avoid pointless repeats we cheat by skipping non-explicitly requested empty results

		std::cout << std::endl;
		std::cout << offset << "Segment #" << std::to_string(segmentNo);

		std::cout << " \"" << dir.name << "\": " << std::endl;
		for (auto& child : dir.children)
			std::cout << offset << "  " << "#" << child.segmentNo << ": " << child.filename << std::endl;

		if (!recursive) return;
		for (auto& child : dir.children) {
			if (!visited.insert(child.segmentNo).second)
				continue;
			print(child.segmentNo, offset + " ", recursive, false);
		}
	}

public:
	void setTree(DirectoryTreeLoader* dirTree)
	{
		this->dirTree = dirTree;
	}

	void print(std::string path, bool recursive)
	{
		auto dirNo = strtoimax(path.c_str(), nullptr, 10);
		if (dirNo == 0 && (path != "0")) {
			dirNo = dirTree->traverse(path);
			if (dirNo == -1) {
				qWarning() << "Cannot resolve path " << path << std::endl;
				return;
			}
		}

		this->visited.clear();
		this->print(dirNo, "", recursive, true);
	}
};



int main2(int argc, char* argv[]) {
	CLI::App app{};
	app.name("ntfscmd");
	app.description(R"(NTFS command line tools)");
	app.get_formatter()->column_width(10);
	app.get_formatter()->description_paragraph_width(100);

	std::string srcPath, destPath;
	app.add_option("source, --source", srcPath, "Source device/file")
		->required();


	//Create a temporary VSS shadow copy for the source.
	bool bVssCreateSourceShadow = false;
	app.add_flag("--shadow", bVssCreateSourceShadow,
		"Create a temporary VSS shadow copy for the source. See VSS docs for which paths can be shadowed. "
		"If you pass a manually-created shadow, do not set this flag."
		)
		->group("Access options")
		->capture_default_str()
		;
	bool bVssWritersParticipation = false;
	app.add_flag("--shadow-writers", bVssWritersParticipation,
		"Use VSS_CTX_BACKUP instead of VSS_CTX_FILE_SHARE_BACKUP. Requires --shadow. Read the docs. Better more stable backup, but slower and more flaky shadow creation process itself."
		)
		->group("Access options")
		->capture_default_str()
		;


	//We'll enforce FSCTL_LOCK_VOLUME where it seems reasonable and TRY it elsewhere.
	//These flags force us to insist on it even if we're not sure it should work.
	bool bForceLockSrc = false;
	app.add_flag("--force-lock-src", bForceLockSrc, "Force FSCTL_LOCK_VOLUME on the source even when it doesn't seem to be a volume.")
		->group("Access options")
		->capture_default_str()
		;


	std::unordered_set<SegmentNumber> dumpSegments{};
	app.add_option("--dump-segment", dumpSegments, "Dump MFT entries in hex.")
		->group("Processing options")
		->delimiter(',');

	std::unordered_set<SegmentNumber> printSegments{};
	app.add_option("--print-segment", printSegments, "Print MFT entries.")
		->group("Processing options")
		->delimiter(',');

	std::unordered_set<SegmentNumber> dumpClusters{};
	app.add_option("--dump-cluster", dumpClusters, "Dump clusters in hex.")
		->group("Processing options")
		->delimiter(',');

	bool bDumpRaw = false;
	app.add_flag("--raw", bDumpRaw, "Dump raw data instead of hex.")
		->group("Processing options")
		->delimiter(',');

	int lineSize = 32;
	app.add_option("--line-size", lineSize, "Dump hex in lines of this size (0=no line splitting).")
		->group("Processing options")
		->delimiter(',');


	std::vector<std::string> listDirs{};
	std::vector<std::string> listTrees{};
	app.add_option("--list-dir", listDirs, "List directory contents for this segment/path.")
		->group("Processing options")
		->delimiter(',');
	app.add_option("--list-tree", listTrees, "List directory contents for this segment/path and all subdirs.")
		->group("Processing options")
		->delimiter(',');




	bool quiet = false;
	app.add_flag("--quiet", quiet, "Only print warnings and above.")
		->group("Output options")
		->capture_default_str()
		;

	bool verbose = false;
	app.add_flag("--verbose", verbose, "Detailed logging.")
		->group("Output options")
		->capture_default_str()
		;

	bool debug = false;
	app.add_flag("--debug", debug, "Extra detailed logging.")
		->group("Output options")
		->capture_default_str()
		;


	CLI11_PARSE(app, argc, argv);

	if (debug) {
		LogPrinter::verbosity = Verbosity::Debug;
		verbose = true;
	}
	else if (verbose) {
		LogPrinter::verbosity = Verbosity::Verbose;
		quiet = false;
	}
	else if (!quiet)
		LogPrinter::verbosity = Verbosity::Info;
	else
		LogPrinter::verbosity = Verbosity::Warning;


	if (bVssWritersParticipation && !bVssCreateSourceShadow)
		qWarning() << "--shadow-writers without --shadow, ignored." << std::endl;

	if (lineSize <= 0)
		lineSize = MAXINT;


	// Before we open handles, auto-create the shadow
	std::unique_ptr<VssShadowCopy> srcShadow;
	if (bVssCreateSourceShadow) {
		qInfo() << "VSS: Creating shadow copy for " << srcPath << std::endl;
		//This initializes COM so only try to create when asked to.
		srcShadow.reset(new VssShadowCopy());
		srcShadow->setSnapshotMode(bVssWritersParticipation ? VssSnapshotMode::WriterBackup : VssSnapshotMode::NonWriterBackup);
		srcShadow->create(utf8ToWchar(srcPath));
		auto snapshotPath = wcharToUtf8(srcShadow->snapshotPath());
		qInfo() << "VSS: Shadow copy for " << srcPath << " created at: " << snapshotPath << std::endl;
		srcPath = snapshotPath;
	}


	qVerbose() << "Source: " << srcPath << std::endl;

	// Open Handles
	auto src = Volume2(srcPath, GENERIC_READ);
	src.readLayout();


	// Open and scan MFT
	qInfo() << "Loading MFT structures..." << std::endl;
	src.initMft();
	src.mft->load();

	qVerbose() << "Loading stored bitmap..." << std::endl;
	NtfsBitmapFile srcBitmap(&src, src.mft);

	BitmapBuf srcUsed;

	std::vector<byte> buf;
	buf.resize(src.mft->BytesPerFileSegment);
	for (auto& idx : dumpSegments) {
		src.mft->readSegmentByIndex(idx, (FILE_RECORD_SEGMENT_HEADER*)buf.data());
		std::cout << std::endl << "Segment #" << std::to_string(idx) << " dump:" << std::endl;
		if (bDumpRaw)
			dumpRaw(buf.data(), buf.size());
		else
			dumpHex(buf.data(), buf.size(), lineSize);
	}

	buf.resize(src.volumeData().BytesPerCluster);
	for (auto& idx : dumpClusters) {
		src.setFilePointer(idx*buf.size());
		DWORD bytesRead = 0;
		OSCHECKBOOL(src.read(buf.data(), (DWORD)buf.size(), &bytesRead, nullptr));
		assert(bytesRead == buf.size());
		std::cout << std::endl << "Cluster #" << std::to_string(idx) << " dump:" << std::endl;
		if (bDumpRaw)
			dumpRaw(buf.data(), buf.size());
		else
			dumpHex(buf.data(), buf.size(), lineSize);

	}


	SegmentPrinter segPrinter(src);

	buf.resize(src.mft->BytesPerFileSegment);
	for (auto& idx : printSegments) {
		auto segment = (FILE_RECORD_SEGMENT_HEADER*)buf.data();
		src.mft->readSegmentByIndex(idx, segment);
		std::cout << std::endl << "Segment #" << std::to_string(idx) << ":" << std::endl;
		segPrinter.printSegment(segment);
	}


	DirectoryTreeLoader dirTree(*src.mft);
	DirIndexPrinter dirPrinter;
	dirPrinter.setTree(&dirTree);
	for (auto& path : listDirs)
		dirPrinter.print(path, false);

	for (auto& path : listTrees)
		dirPrinter.print(path, true);

	return 0;
}

int main(int argc, char* argv[]) {
	try {
		return main2(argc, argv);
	}
	catch (const std::exception& e) {
		qError() << e.what() << std::endl;
		return -1;
	}
}