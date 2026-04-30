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
#include "printers\segmentprint.h"



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


std::string indexHeaderFlagsToString(USHORT flags)
{
	std::string result{};
	if (flags & INDEX_NODE) result += " NODE";
	flags &= ~(INDEX_NODE);
	if (flags != 0)
		result += " FLAGS_" + flagsToStr(flags);
	return result;
}

std::string indexEntryFlagsToString(USHORT flags)
{
	std::string result{};
	if (flags & INDEX_ENTRY_NODE) result += " NODE";
	if (flags & INDEX_ENTRY_END) result += " END";
	flags &= ~(INDEX_ENTRY_NODE | INDEX_ENTRY_END);
	if (flags != 0)
		result += " FLAGS_" + flagsToStr(flags);
	return result;
}

void printIndexHeader(INDEX_HEADER& header)
{
	std::cout << "BytesAv: " << header.BytesAvailable;
	std::cout << " FirstFreeByte: " << header.FirstFreeByte;
	std::cout << " FirstIndexEntry: " << header.FirstIndexEntry;
	std::cout << " Flags: " << indexHeaderFlagsToString(header.Flags);
	std::cout << std::endl;
}

void printIndexRootHeader(INDEX_ROOT& header)
{
	std::cout << "AttrType: " << attrTypeToStr(header.IndexedAttributeType);
	std::cout << " PerIndexBuffer: Bytes=" << header.BytesPerIndexBuffer << ", Blocks=" << header.BlocksPerIndexBuffer;
	std::cout << " Collation: " << collationRuleToStr(header.CollationRule);
	printIndexHeader(header.IndexHeader);
}

void printIndexAllocationBufferHeader(INDEX_ALLOCATION_BUFFER& header)
{
	printMultiSectorHeader(header.MultiSectorHeader, header.Lsn);
	std::cout << ", ThisBlock: " << header.ThisBlock << std::endl;
	printIndexHeader(header.IndexHeader);
}


class IndexPrinter : public IndexProcessor, public SegmentPrinter
{
public:
	IndexPrinter(Mft& mft)
		: IndexProcessor(mft.vol), SegmentPrinter(mft)
	{}
	virtual void processIndexRoot(void* data, size_t len) override;
	virtual void processIndexAllocationBuffer(INDEX_ALLOCATION_BUFFER* buffer) override;
	virtual size_t tryReadIndexEntry(byte* buf, size_t len) override;
};


void IndexPrinter::processIndexRoot(void* data, size_t len)
{
	//Print the header before the inherited implementation parses index entries and we print them
	//Do minimal asserts, only enough for our printing. The rest is in the inherited.
	assert(len >= sizeof(INDEX_ROOT));
	auto header = (INDEX_ROOT*)data;

	printIndexRootHeader(*(INDEX_ROOT*)data);

	IndexProcessor::processIndexRoot(data, len);
}

void IndexPrinter::processIndexAllocationBuffer(INDEX_ALLOCATION_BUFFER* buffer)
{
	printIndexAllocationBufferHeader(*buffer);
}

size_t IndexPrinter::tryReadIndexEntry(byte* buf, size_t len)
{
	if (len < sizeof(INDEX_ENTRY)) return 0;

	auto entry = (INDEX_ENTRY*)buf;
	if (len < entry->Length) return 0;

	//These are always available
	std::cout << "Ref: " << segmentRefToStr(entry->FileReference);
	std::cout << " Len: " << entry->Length;
	std::cout << " AttrLen: " << entry->AttributeLength;
	std::cout << " Flags: " << indexEntryFlagsToString(entry->Flags); //INDEX_ENTRY_END and INDEX_ENTRY_NODE

	//The remainder depends on the flags

	//If this is an intermediate node it should have VCN in addition to ATTRIBUTE_LENGTH.
	if (entry->Flags & INDEX_ENTRY_NODE) {
		if (entry->Length < sizeof(INDEX_ENTRY) + entry->AttributeLength + sizeof(VCN))
			std::cout << std::endl << "INDEX_ENTRY too small for its flags!";
		else {
			auto vcn = (VCN*)((byte*)entry + entry->Length - sizeof(VCN));
			std::cout << " VCN: " << vcn << std::endl;
		}
	}

	int64_t extraBytes = (int64_t)entry->Length - sizeof(INDEX_ENTRY);
	if (entry->Flags & INDEX_ENTRY_NODE)
		extraBytes -= sizeof(VCN);

	if (entry->Flags & INDEX_ENTRY_END) {
		//Do not subtract AttributeLength here as there's not supposed to be attribute value here.
		//If it's non-zero, let the check at the end catch the discrepancy. AttributeLength is visible anyway as it's printed.
	} else {
		extraBytes -= entry->AttributeLength;
		this->printAttrResidentValue(this->m_root.IndexedAttributeType, entry->attributeData(), entry->AttributeLength);
	}

	if (extraBytes > 0)
		std::cout << "Additional " << extraBytes << "bytes in an entry!";

	return entry->Length;
}

class SegmentIndexesPrinter : public SegmentIndexesLoader
{
protected:
	Mft& m_mft;
public:
	SegmentIndexesPrinter(Mft& mft)
		: SegmentIndexesLoader(mft), m_mft(mft)
	{}
	std::unordered_map<std::string, IndexPrinter> indexes;
	virtual IndexProcessor* getIndexProcessor(const std::string& indexName);
	virtual void advanceIndexes();
};

//Override to return IndexProcessors for the indexes you're interested in.
IndexProcessor* SegmentIndexesPrinter::getIndexProcessor(const std::string& indexName)
{
	auto it = this->indexes.find(indexName);
	if (it == this->indexes.end()) {
		it = this->indexes.emplace(indexName, IndexPrinter(this->m_mft)).first;
	}
	return &(it->second);
}

void SegmentIndexesPrinter::advanceIndexes()
{
	for (auto& pair : this->indexes) {
		pair.second.advance();
	}
}



class DirIndexPrinter
{
protected:
	DirectoryTreeLoader* dirTree = nullptr;
	//Protect against duplicate names in the same dir (very common) + against recursion.
	//Could have done the same on each dir's level (duplicates) + another set/stack for recursion, but whatever.
	std::unordered_set<SegmentNumber> visited{};


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


	std::unordered_set<SegmentNumber> getSegmentLcns{};
	app.add_option("--get-segment-lcn", getSegmentLcns, "Print LCN of the cluster where this segment is hosted.")
		->group("Processing options")
		->delimiter(',');


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



	std::unordered_set<SegmentNumber> printIndexes{};
	app.add_option("--print-indexes", printIndexes, "Print indexes for this MFT entry.")
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


	for (auto& segmentNo : getSegmentLcns)
		std::cout << "Segment #" << segmentNo << ": LCN=" << src.mft->getLcn(segmentNo * src.volumeData().BytesPerFileRecordSegment / src.volumeData().BytesPerCluster) << std::endl;


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


	SegmentPrinter segPrinter(*src.mft);

	buf.resize(src.mft->BytesPerFileSegment);
	for (auto& idx : printSegments) {
		auto segment = (FILE_RECORD_SEGMENT_HEADER*)buf.data();
		src.mft->readSegmentByIndex(idx, segment);
		std::cout << std::endl << "Segment #" << std::to_string(idx) << ":" << std::endl;
		segPrinter.printSegment(segment);
	}


	for (auto& segmentNo : printIndexes) {
		SegmentIndexesPrinter printer(*src.mft);
		printer.load(*src.mft, segmentNo);
		for (auto& pair : printer.indexes) {
			std::cout << "Index " << pair.first << ":" << std::endl;
			//TODO: print.
		}
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