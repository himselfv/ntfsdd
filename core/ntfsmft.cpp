#pragma once
#include <algorithm>
#include "ntfsmft.h"
#include "mftutil.h"


NonResidentData::NonResidentData(Volume* vol)
	: vol(vol)
{
	this->dataHeader.RecordLength = 0; //As an indication of it not yet being available
}

//Returns LCN associated with a given VCN, or -1 if the map does not cover this VCN.
LCN NonResidentData::getLcn(VCN vcn) {
	for (auto& span : this->m_vcnMap)
		if (vcn < span.vcnStart)
			return (LCN)(-1);
		else if (vcn < span.vcnStart + span.len)
			return (span.lcnStart + vcn - span.vcnStart);
	return (LCN)(-1);
}

//Some files can be sparse but sometimes we'd like to check for no holes.
VCN NonResidentData::getFirstMissingVcn() {
	VCN vcn = 0;
	for (auto& span : this->m_vcnMap)
		if (span.vcnStart > vcn)
			return vcn;
		else
			vcn = span.vcnStart + span.len + 1;
	return (VCN)(-1);
}

void NonResidentData::addAttrChunk(ATTRIBUTE_RECORD_HEADER* attr) {
	if (attr->Form.Nonresident.LowestVcn == 0) {
		//Lowest run map attribute stores totals
		this->dataHeader = *attr;
	}
	VCN vcnpos = attr->Form.Nonresident.LowestVcn;
	for (auto& run : DataRunIterator(attr, DRI_SKIP_SPARSE)) {
		m_vcnMap.push_back(VcnMapEntry{ vcnpos, run.offset, run.length });
		vcnpos += run.length;
	}
	//WARNING: We list all allocated clusters, as described by run maps, but actual precise file size can be less!
	//Mind it when reading!
	std::sort(m_vcnMap.begin(), m_vcnMap.end(), [](const VcnMapEntry& a, const VcnMapEntry& b) { return a.vcnStart < b.vcnStart; });
}

void NonResidentData::readAll(void* buf)
{
	auto BytesPerCluster = vol->volumeData().BytesPerCluster;
	auto ptr = ((uint8_t*)buf);
	for (auto& run : this->m_vcnMap) {
		DWORD bytesRead = 0;
		LARGE_INTEGER vrbn;
		vrbn.QuadPart = run.lcnStart*BytesPerCluster;
		OSCHECKBOOL(vol->setFilePointer(vrbn));
		auto readSz = run.len*BytesPerCluster;
		auto remSz = this->dataHeader.Form.Nonresident.AllocatedLength - run.vcnStart*BytesPerCluster;
		if (remSz < readSz)
			readSz = remSz;
		OSCHECKBOOL(vol->read(ptr, (DWORD)readSz, &bytesRead, nullptr));
		assert(bytesRead == readSz);
		ptr += bytesRead;
	}
}



/*
Some resident attributes can become non-resident if they grow large, and then split into multiple chunks in different segments:
  Base:    $Data, VCN=0..1000
  Extra1:  $Data, VCN=1001..2000
  Extra2:  $Data, VCN=2001..3000
As we process these segments, we only have a partial VCN->LCN map from the chunks collected so far.

Sometimes we need to process as much as we can anyway! E.g. see $ATTRIBUTE_LIST: we may need to process the previous chunks
to know where the next chunk is.

This class handles that:
  proc->addAttrChunk(attr); //Add attribute chunks as you encounter them
  proc->advance(); //Immediately try to process more data
*/
AttributeCollectorProcessor::AttributeCollectorProcessor(Volume* vol)
	: NonResidentData(vol)
{
	//Reserve some space for typical processing but be ready to collect any length of data
	//Some processors want blocks of multiple clusters, others simply cannot process until they see some particular attribute.
	//It's the job of the caller to manage memory expectations. Don't collect endlessly on data that can get big.
	m_buf.reserve(vol->volumeData().BytesPerCluster * 2);
	m_pos = m_buf.data();
}

void AttributeCollectorProcessor::packBuffer()
{
	size_t rem = m_buf.size() - (m_pos - m_buf.data());
	memcpy(m_buf.data(), m_pos, rem);
	m_buf.resize(rem);
	m_pos = m_buf.data();
}

//Processes a complete resident instance of the attribute. Has to be the only instance of it.
//Do not call advance() after calling this. This circumvents step-by-step logic.
//Your tryReadEntry() override has to be ready for both cases.
void AttributeCollectorProcessor::processResidentAttr(ATTRIBUTE_RECORD_HEADER& attr)
{
	//Set this as a dataHeader
	this->dataHeader = attr;
	assert(attr.FormCode == RESIDENT_FORM);
	this->processData(attr.ResidentValuePtr(), attr.Form.Resident.ValueLength);
	this->m_vcnEof = true;
}

//Simplified version that reads one complete block of data.
void AttributeCollectorProcessor::processData(void* data, size_t len)
{
	while (auto readSz = tryReadEntry((byte*)data, len)) {
		data = (void*)((byte*)data + readSz);
		len -= readSz;
	}
	assert_eq(len, 0, "Unprocessed data in a standalone attribute data block");
}

/*
How to handle a genuine EOF?
VCN==0 entry contains all the sizes. As soon as we have that, we know the final size.
*/
bool AttributeCollectorProcessor::tryReadMore()
{
	if (this->haveDataHeader() && this->m_nextVcn > this->dataHeader.Form.Nonresident.HighestVcn) {
		this->m_vcnEof = true;
		return false;
	}

	auto lcn = this->getLcn(this->m_nextVcn);
	if (lcn < 0) return false;

	auto clusterSize = vol->volumeData().BytesPerCluster;

	OSCHECKBOOL(this->vol->setFilePointer(lcn*clusterSize));

	packBuffer();
	//To have m_pos at the beginning of the buffer.

	auto oldSize = m_buf.size();

	//Resize the buffer and update the pointers in case there was reallocation:
	m_buf.resize(m_buf.size() + clusterSize);
	m_pos = m_buf.data();

	DWORD bytesRead = 0;
	OSCHECKBOOL(this->vol->read(&m_buf[oldSize], clusterSize, &bytesRead, nullptr));
	assert(bytesRead == clusterSize);

	//If we have an incomplete final cluster, adjust the length so that the reader may rely on it blindly
	//By this point we have read *something* and so surely we have read VCN==0, so we have that chunk, so we have sizes
	int64_t remainingBytes = this->dataHeader.Form.Nonresident.FileSize - this->m_nextVcn*clusterSize;
	if (remainingBytes < clusterSize) {
		if (remainingBytes < 0) remainingBytes = 0; //Can happen if there's more than 1 cluster of slack
		this->m_buf.resize(this->m_buf.size() - (clusterSize - remainingBytes));
		//Skip reading the rest of the clusters
		this->m_nextVcn = dataHeader.Form.Nonresident.HighestVcn+1;
		//Do not set vcnEof here because we need to give another chance to the tryReadEntry().
		//We'll set it when we're called again with no more VCNs => tryReadEntry had its time with the buffered remainder.
	} else
		this->m_nextVcn++;
	return true;
}

int AttributeCollectorProcessor::advance()
{
	int steps = 0;
	while (!m_vcnEof) {
		size_t rem = this->remainingBytesInBuf();
		size_t sz = 0;
		while ((rem > 0) && (sz = tryReadEntry(m_pos, rem))) {
			m_pos += sz;
			rem -= sz;
			steps++;
		}
		if (!tryReadMore())
			return steps;
	}
	return steps;
}



/*
So okay. $ATTRIBUTE_LIST weirdness.
https://stackoverflow.com/questions/42777907/understanding-the-attribute-list-in-ntfs

In short:
Some attributes must stay resident.
Some can be made non-resident once they become too large. The attribute itself then becomes a list of data runs. This list must stay resident.

Once everything is maximally packed and still doesn't fit, additional segments are allocated and some resident data moved there.
Attributes describing data runs can be split between segments:
  Base:    $Data, VCN=0..1000
  Extra1:  $Data, VCN=1001..2000
  Extra1:  $Data, VCN=2001..3000  //Two chunks in the same segment are not prohibited!
Same type and name => chunks of the same attribute. Must not intersect.

The base segment gets SOME of them + a RESIDENT $ATTRIBUTE_LIST attribute describing where ALL of them are (type, name, VCN, host segment).

Once this $ATTRIBUTE_LIST overflows (very soon), it is made non-resident and the segment now hosts its data runs:
$ATTRIBUTE_LIST: VCN=0..1000 -> LCN 15100-16100

!! Be careful when processing these! Entries may span data run boundaries!


At the very least, these things seem guaranteed:
- If there are ANY attribute lists in the segment system, there's SOME $ATTRIBUTE_LIST in the base segment. (How else would you know)
- The part of the $ATTRIBUTE_LIST that stays in the base segment always starts with VCN==0


Once THIS data run list overflows the segment, things get confusing.

Some people say TWO child $ATTRIBUTE_LISTs will be created in extra segments and the base one becomes resident and now only contains links to these two.

Others say $ATTRIBUTE_LIST remains one, but gets fragmented as any other data run list can:
  Base:   $ATTRIBUTE_LIST, VCN=0..1000
  Extra1: $ATTRIBUTE_LIST, VCN=1001..2000
  Extra2: $ATTRIBUTE_LIST, VCN=2001..3000
The creator now must ensure that there's enough info in VCNs 0..1000 to find Extra1, and enough in VCNs 0(sic)..2000 to find Extra2.

The easiest way to achieve this is to place ALL $ATTRIBUTE_LIST chunk descriptions in the first VCNs, so that:
- You encounter partial $ATTRIBUTE_LIST in the base.
- You read its first VCNs
- These describe where the other parts are.
In reality, the entries in $ATTRIBUTE_LIST are ordered (by attr type, then by name). But $ATTRIBUTE_LIST is relatively low, so this may work.

Perhaps this is indeed the rule, and people who talk of two other $ATTRIBUTE_LIST records have just misinterpreted the two chunks that they saw.
Or maybe not! IDK.
*/

/*
Anyway, my plan:
1. EVERY segment, base or extended, can be simply processed directly. The chunks that fall into it are simply normal attribute chunks.
2. So we don't have to use $ATTRIBUTE_LIST information. It's enough to parse each of the mentioned segments normally.

The algorithm:

Add base segment to the queue.
For each segment in the queue:
  Read the segment, process the attributes normally. For non-resident attribute chunks, add the mentioned runs to their maps.
  When encountering $ATTRIBUTE_LIST,
    If it's resident, process immediately (see below).
    If it's non-resident, append the runs to its map and initiate attempt_to_advance()

attempt_to_advance:
  Remember the current position in the $ATTRIBUTE_LIST data VCN. Start with VCN==0.
  While the position you're at + entry_size bytes after it are already mapped:
    Read more clusters to have a complete entry.
    Process it (see below).
    Advance
  Once you cannot read a complete entry, return and remember the position and unprocessed tail.

Entry processing:
  When processing $ATTRIBUTE_LIST entries, do the only thing: collect mentioned segments and add them to the processing queue.
*/

//0 if not enough data in the buffer for another entry
size_t AttributeListProcessor::tryReadEntry(byte* buf, size_t len)
{
	if (len < sizeof(ATTRIBUTE_LIST_ENTRY))
		return 0;

	auto entry = reinterpret_cast<const ATTRIBUTE_LIST_ENTRY*>(buf);

	//Some say this can be 0 and it means EOF for the list, can't find this in the docs,
	//let's just assert for now.
	assert_neq(entry->RecordLength, 0);
	assert_greq(entry->RecordLength, sizeof(ATTRIBUTE_LIST_ENTRY));

	if (len < entry->RecordLength)
		return 0;

	auto segmentNo = entry->SegmentReference.segmentNumber();
	bool found = false;
	for (auto& segment : this->segments)
		if (segment == segmentNo) {
			found = true;
			break;
		}
	if (!found)
		this->segments.push_back(segmentNo);

	return entry->RecordLength;
}



MultiSegmentFileLoader::MultiSegmentFileLoader(Volume* vol)
	: attrList(vol)
{
}

MultiSegmentFileLoader::~MultiSegmentFileLoader()
{
}

//Start with the base segment and use Mft to load all segments sequentially
void MultiSegmentFileLoader::load(Mft& mft, SegmentNumber baseSegmentNo)
{
	//Initialize $ATTRIBUTE_LIST processing
	//Even if we never encounter $ATTRIBUTE_LIST, add our own segment 0 as a starting point
	this->attrList.segments.push_back(baseSegmentNo);
	int segmentIdx = 0;

	while (segmentIdx < this->attrList.segments.size()) {
		this->loadSegment(mft, this->attrList.segments[segmentIdx]);
		//This adds all $Data chunks and all $ATTRIBUTE_LIST chunks mentioned therein.
		//$Data chunks are independent, while $ATTRIBUTE_LIST chunks can only be processed sequentially.
		segmentIdx++;
		//Process however much we can:
		this->attrList.advance();
	}

	this->attrList.assert_all_processed();
}


//Load this particular segment, either base or secondary. Add its relevant data to the attribute processors.
void MultiSegmentFileLoader::loadSegment(Mft& mft, SegmentNumber segmentNo)
{
	auto segment = mft.newSegmentBuf();
	mft.readSegmentByIndex(segmentNo, (FILE_RECORD_SEGMENT_HEADER*)segment.data());
	this->loadSegment((FILE_RECORD_SEGMENT_HEADER*)(segment.data()));
}

//Load this particular segment from data
void MultiSegmentFileLoader::loadSegment(FILE_RECORD_SEGMENT_HEADER* segment)
{
	//Read attributes
	for (auto& attr : AttributeIterator(segment)) {
		if (attr.TypeCode == $ATTRIBUTE_LIST) {
			//Process the attribute list chunk now.
			//We're only taking note of the segment numbers mentioned, not reading anything, so there's no reason
			//to delay this until we have read all possible $Data chunks.
			assert(attr.NameLength == 0); //Only unnamed $ATTRIBUTE_LIST is supported
			/*
			We make no attempt to limit the number of $ATTRIBUTE_LIST chunks to one,
			or to ensure that RESIDENT_FORM entries do not coexist with NONRESIDENT_FORM ones.
			Doing so could catch us our accidentally going to wrong segments or resolving to wrong clusters,
			but we will pay with the flexibility in accepting weird things that a real MFT might produce.
			Whatever. Process everything that it throws at us in this regard.
			*/
			if (attr.FormCode == RESIDENT_FORM)
				this->attrList.processData((char*)&attr + attr.Form.Resident.ValueOffset, attr.Form.Resident.ValueLength);
			else
				this->attrList.addAttrChunk(&attr);
		}
		this->processAttr(attr);
	}
}

void MultiSegmentFileLoader::processAttr(ATTRIBUTE_RECORD_HEADER& attr)
{
	//Override to process additional attributes.
}



Mft::Mft(Volume* volume)
	: NonResidentData(volume), MultiSegmentFileLoader(volume)
{
	vol = volume;
}

//Actual loading has to take place after the source Volume figures out its layout.
//Minimal loading allows us to read arbitrary segments in the first few clusters.
//Full loading does some additional integrity checks.
void Mft::loadMinimal()
{
	// Pre-calculate and verify some values
	assert(vol->volumeData().BytesPerFileRecordSegment % vol->volumeData().BytesPerSector == 0, "MFT segment size is not a multiple of logical sector size!");
	this->SectorsPerFileSegment = vol->volumeData().BytesPerFileRecordSegment / vol->volumeData().BytesPerSector;
	this->BytesPerFileSegment = vol->volumeData().BytesPerFileRecordSegment;
}

/*
$MFT consists of one or more cluster runs described in $Data.
Run 0 by definition starts at MftStartLcn. Its length and the rest of the runs have to be collected from $Data attributes
in its segment 0 ($MFT).

$MFT segment 0 can and will spawn child segments (via $ATTRIBUTE_LIST) if gets too large.
https://stackoverflow.com/questions/30424102/can-the-ntfs-mft-file-have-child-records
$Data attributes can in theory get pushed out into these additional segments. This poses a problem. How to resolve the physical
location of these additional segments if we don't yet know the map of logical to physical clusters.

Hopefully, enough of $Data runs will be described in segment 0 to resolve the first extra segment, and enough in 0+extra1
to resolve extra2, and so on.
But as a safety measure, initial expansions are placed in reserved segments 15-20+ which are more or less guaranteed to be
contiguous. So absent any runs, treat $MFT as a single flat space starting at segment 0.

The approach:
1. Until we have at least one run, treat all VCNs as flat space starting from segment 0.
2. Read all the attributes at 0, including any data runs.
3. If there's $ATTRIBUTE_LIST, extract all extra segment references and process them sequentially,
  reading data runs from each segment before taking the next one.

So we have basically dual task:
We have to collect the chunks for the $Data attribute and for the $ATTRIBUTE_LIST attribute.
$Data tells us how to map segments# to LCNs, and $ATTRIBUTE_LIST tells us in which segments# to look for more of the both.
*/
void Mft::load()
{
	this->loadMinimal();

	this->m_vcnMap.clear();

	//Load and process all related segments sequentially, collecting attrList and (via processAttr) vcnMap/data.
	MultiSegmentFileLoader::load(*this, 0);

	assert(this->m_vcnMap.size() > 0); //MFT should not be empty
	assert(this->m_vcnMap.front().lcnStart == this->vol->volumeData().MftStartLcn.QuadPart); //First cluster should match the one we started with
	assert(this->getFirstMissingVcn() == (uint64_t)(-1)); //Should be no spaces in the MFT
	assert(this->sizeInBytes() % this->BytesPerFileSegment == 0);
}

//Called for every attribute chunk found in every segment related to the MFT
void Mft::processAttr(ATTRIBUTE_RECORD_HEADER& attr)
{
	if (attr.TypeCode == $DATA) {
		if (attr.NameLength != 0)
			qWarning() << "MFT: Alternate $Data streams in $MFT segment! Highly unusual. Ignoring.";
		else {
			assert(attr.FormCode == NONRESIDENT_FORM, "MFT: $Data attribute in $MFT segment must be non-resident.");
			this->addAttrChunk(&attr);
		}
	}
}

//Allocate a segment buffer, properly sized and properly aligned for this MFT
AlignedBuffer Mft::newSegmentBuf()
{
	AlignedBuffer segment;
	segment.resize(BytesPerFileSegment);
	return segment;
}

void Mft::readSegmentByIndex(SegmentNumber segmentNo, FILE_RECORD_SEGMENT_HEADER* segment)
{
	/*
	MFT uses a special convention to safeguard resolving initial additional segments of the MFT itself:
	If we do not have a map yet, assume one continuous space starting at the MftStartLcn.
	This IS a chunk of the MFT, we just don't know its length yet.
	*/
	if (this->m_vcnMap.empty()) {
		readSegmentByIndexMinimal(segmentNo, segment);
		return;
	}

	auto BytesPerCluster = vol->volumeData().BytesPerCluster;

	VRBN vrbn;
	vrbn.QuadPart = segmentNo * BytesPerFileSegment;
	VCN vcn = vrbn.QuadPart / BytesPerCluster;
	vrbn.QuadPart %= BytesPerCluster;

	auto lcn = this->getLcn(vcn);
	assert(lcn >= 0);
	vrbn.QuadPart += lcn * BytesPerCluster;

	return this->readSegmentsVrbn(vrbn, segment, 1);
}

/*
Minimal version of the above which does not rely on a $Data map and instead assumes a flat MFT starting at a MftStartLcn.
Used during the initial loading as a fallback when looking for $MFT segment 0 extensions in case the segment 0
for some reason does not contain a $Data chunk describing enough to find those.
Initial $MFT segment extensions are often allocated in reserved segments 15-20+, so this is not that far-fetched.
*/
void Mft::readSegmentByIndexMinimal(SegmentNumber segmentNo, FILE_RECORD_SEGMENT_HEADER* segment)
{
	auto BytesPerCluster = vol->volumeData().BytesPerCluster;

	VRBN vrbn;
	vrbn.QuadPart = segmentNo * BytesPerFileSegment;
	VCN vcn = vrbn.QuadPart / BytesPerCluster;
	vrbn.QuadPart %= BytesPerCluster;

	auto lcn = this->vol->volumeData().MftStartLcn.QuadPart + vcn;
	vrbn.QuadPart += lcn * BytesPerCluster;

	return this->readSegmentsVrbn(vrbn, segment, 1);
}

void Mft::readSegmentsVrbn(VRBN vrbn, FILE_RECORD_SEGMENT_HEADER* segment, int count)
{
	OSCHECKBOOL(vol->setFilePointer(vrbn));
	return readSegmentsNoSeek(segment, count);
}

void Mft::readSegmentsNoSeek(FILE_RECORD_SEGMENT_HEADER* segment, int count, LPOVERLAPPED lpOverlapped)
{
	DWORD bytesRead = 0;
	OSCHECKBOOL(vol->read(segment, count*BytesPerFileSegment, &bytesRead, nullptr));
	assert(bytesRead == count*BytesPerFileSegment);
	this->processSegments(segment, count);
}


void Mft::processSegments(FILE_RECORD_SEGMENT_HEADER* segment, int count, int* validCount)
{
	while (count > 0) {
		//So apparently records can be uninitialized. These appear closer to the end of the MFT.
		bool isValidSegment = IsValidSegment(segment);

		//Apply fixups
		if (isValidSegment) {
			this->segmentApplyFixups(segment);
			if (validCount != nullptr)
				(*validCount)++;
		}

		segment = (FILE_RECORD_SEGMENT_HEADER*)((uint8_t*)segment + BytesPerFileSegment);
		count--;
	}
}


void sectorsApplyFixups(MULTI_SECTOR_HEADER* data, int sectors, DWORD bytesPerSector)
{
	auto fixupCnt = data->UpdateSequenceArraySize - 1; //1 additional cell is for the UpdateValueNumber
	assert(fixupCnt == sectors);

	auto fixup = (uint16_t*)((char*)data + data->UpdateSequenceArrayOffset);
	auto pos = (char*)data + bytesPerSector - 2;
	auto magic = *(fixup++);
	while (fixupCnt > 0) {
		assert(*((uint16_t*)pos) == magic, "Invalid fixup in a segment.");
		*((uint16_t*)pos) = *fixup;
		pos += bytesPerSector;
		fixup++;
		fixupCnt--;
	}
}

void Mft::segmentApplyFixups(FILE_RECORD_SEGMENT_HEADER* segment)
{
	sectorsApplyFixups(&segment->MultiSectorHeader, this->SectorsPerFileSegment, vol->volumeData().BytesPerSector);
}


NtfsBitmapFile::NtfsBitmapFile(Volume* vol, Mft* mft)
	: NonResidentData(vol)
{
	std::vector<uint8_t> buffer;
	buffer.resize(vol->volumeData().BytesPerFileRecordSegment);
	auto segment = (FILE_RECORD_SEGMENT_HEADER*)(buffer.data());
	mft->readSegmentByIndex(BIT_MAP_FILE_NUMBER, segment);
	auto attr = AttributeIterator::findFirstAttr(segment, $DATA);
	assert(attr != nullptr);
	this->addAttrChunk(attr);

	assert(attr->FormCode == NONRESIDENT_FORM);
	auto allocSize = sizeof(VOLUME_BITMAP_BUFFER) + attr->Form.Nonresident.AllocatedLength;
	this->buf = (VOLUME_BITMAP_BUFFER*)malloc(allocSize); //Rounded up to cluster for ease of reading
	this->readAll(&(buf->Buffer[0]));

	//Fill data in our VOLUME_BITMAP_BUFFER for some compatibility
	this->buf->StartingLcn.QuadPart = 0;
	this->buf->BitmapSize.QuadPart = vol->volumeData().TotalClusters.QuadPart; //This can be slightly less

	//Either the cluster count matches or it's rounded up.
	assert(this->sizeInBytes() == (this->buf->BitmapSize.QuadPart + 7) / 8);
}

NtfsBitmapFile::~NtfsBitmapFile()
{
	if (this->buf != nullptr) {
		free(this->buf);
		this->buf = nullptr;
	}
}


SegmentIteratorBase::SegmentIteratorBase(Mft* mft, Flags flags)
	: mft(mft), flags(flags)
{
	if (mft == nullptr) return; //null-initialized end() iterator
}

int64_t SegmentIteratorBase::selectMaxChunkSize()
{
	if (mft == nullptr) return 4096;
	//Our reading chunk
	//- Certainly a segment multiple
	//- Best if a cluster multiple to simplify reading
	//- Best to mind the physical sector size but SSDs often misreport it as 512 when it's 4096.
	auto batchSize = mft->vol->volumeData().BytesPerFileRecordSegment;
	//Segments can be bigger than a cluster! Though rare in practice.
	if (mft->vol->volumeData().BytesPerCluster > batchSize)
		batchSize = mft->vol->volumeData().BytesPerCluster;
	if (mft->vol->extendedVolumeData().BytesPerPhysicalSector > batchSize)
		batchSize = mft->vol->extendedVolumeData().BytesPerPhysicalSector;
	//In any case align to a segment.
	auto remainder = batchSize % mft->vol->volumeData().BytesPerFileRecordSegment;
	if (remainder != 0)
		batchSize = (batchSize + mft->vol->volumeData().BytesPerFileRecordSegment) - remainder;
	//Multiply!
	batchSize *= SEGMENTITERATOR_BATCHSIZE;
	return batchSize;
}

//Advances 1 times until eof() or reads current segment
void SegmentIteratorBase::advance()
{
	this->segment = nullptr; //Cannot advance
}

//Advances 0 or more times until eof() or the first segment matching the criteria
void SegmentIteratorBase::readCurrent()
{
	while (currentRun != nullptr) {
		while (true) {
			if (!segment)
				break; //Valid run but need a new segment
			if ((flags & SI_SKIP_INVALID) && (!mft->IsValidSegment(segment)))
				break;
			if ((flags & SI_SKIP_NOT_IN_USE) && ((segment->Flags & FILE_RECORD_SEGMENT_IN_USE) == 0))
				break;
			return;
		}
		this->advance();
	}
}


SegmentIteratorBuffered::SegmentIteratorBuffered(Mft* mft, Flags flags)
	: SegmentIteratorBase(mft, flags)
{
	if (mft == nullptr) return; //null-initialized end() iterator

	buffer.resize(this->selectMaxChunkSize());

	remainingRuns = (int)mft->vcnMap().size();
	if (remainingRuns > 0) {
		currentRun = mft->vcnMap().data();
		remainingRuns--;
		this->openRun();
		this->readInt();
		this->readCurrent();
	}
}

void SegmentIteratorBuffered::advanceRun()
{
	if (remainingRuns <= 0) {
		currentRun = nullptr;
		remainingSegmentsInRun = 0;
		return;
	}
	remainingRuns--;
	currentRun++;
	this->openRun();
}

//Open currently selected run. It must have at least one segment.
void SegmentIteratorBuffered::openRun()
{
	auto bytesPerCluster = mft->vol->volumeData().BytesPerCluster;
	vrbn.QuadPart = bytesPerCluster * currentRun->lcnStart;
	if (remainingRuns <= 0) {
		remainingSegmentsInRun = mft->sizeInSegments();
		for (size_t i=0; i<mft->vcnMap().size()-1; i++)
			remainingSegmentsInRun -= ((bytesPerCluster * mft->vcnMap().at(i).len) / mft->BytesPerFileSegment);
		remainingSegmentsInRun--;
	} else
		remainingSegmentsInRun = ((bytesPerCluster * currentRun->len) / mft->BytesPerFileSegment) - 1;
	OSCHECKBOOL(mft->vol->setFilePointer(vrbn));

	assert(remainingBufferData == 0); //just in case
}

void SegmentIteratorBuffered::advance()
{
	if (remainingSegmentsInRun > 0) {
		remainingSegmentsInRun--;
		vrbn.QuadPart += mft->BytesPerFileSegment; //Track always, as this is needed for multiple things at once
	}
	else {
		this->advanceRun();
		if (!this->currentRun) {
			segment = nullptr;
			return;
		}
	}

	this->readInt();
}

//Assumes currentRun is valid and there's at least one segment record left in it.
//Advances segment pointer into the current buffer if there's enough buffered data left, or requests a new chunk of data and points segment into that.
//On exit we have a valid segment pointer.
void SegmentIteratorBuffered::readInt()
{
	if (remainingBufferData >= mft->BytesPerFileSegment) {
		segment = (FILE_RECORD_SEGMENT_HEADER*)((uint8_t*)segment + mft->BytesPerFileSegment);
		remainingBufferData -= mft->BytesPerFileSegment;
		//Not checking that remainingBufferData is a multiple of BytesPerFileSegment! This has to be true.
	}
	else {
		//Reading new segment in the limits of available in this run
		SegmentNumber segmentCount = (int64_t)(buffer.size()) / mft->BytesPerFileSegment;
		if (segmentCount > remainingSegmentsInRun + 1) //1 уже вычтен перед вызовом этого чтения
			segmentCount = remainingSegmentsInRun + 1;
		segment = (FILE_RECORD_SEGMENT_HEADER*)(buffer.data());
#ifdef SEGMENTITERATOR_ZEROMEM
		memset(buffer.data(), 0, buffer.size());
#endif
		mft->readSegmentsVrbn(vrbn, segment, (int)segmentCount);
		remainingBufferData = (segmentCount - 1)*(mft->BytesPerFileSegment);
	}

#ifdef SEGMENTITERATOR_TRACKPOS
	lbn += mft->BytesPerFileSegment;
	vcn++;
#endif
}


SegmentIteratorOverlapped::SegmentIteratorOverlapped(Mft* mft, Flags flags)
	: SegmentIteratorBase(mft, flags)
{
	if (mft == nullptr) return; //null-initialized end() iterator

	this->reader = new AsyncFileReader(mft->vol->h(), SEGMENTITERATOR_QUEUEDEPTH, this->selectMaxChunkSize());

	remainingRuns = (int)mft->vcnMap().size();
	if (remainingRuns > 0) {
		currentRun = mft->vcnMap().data();
		remainingRuns--;
		this->openRun();
		this->readCurrent();
	}
}

SegmentIteratorOverlapped::~SegmentIteratorOverlapped()
{
	if (this->reader) {
		delete this->reader;
		this->reader = nullptr;
	}
}

SegmentIteratorOverlapped::SegmentIteratorOverlapped(SegmentIteratorOverlapped&& other)
	: SegmentIteratorBase(nullptr, 0)
{
	(*this) = std::move(other);
}

SegmentIteratorOverlapped& SegmentIteratorOverlapped::operator=(SegmentIteratorOverlapped&& other)
{
	static_cast<SegmentIteratorBase&>(*this) = std::move(static_cast<SegmentIteratorBase&&>(other));
	
	this->reader = other.reader;
	other.reader = nullptr;

	this->remainingBufferData = other.remainingBufferData;
	this->currentClusterInRun = other.currentClusterInRun;
	return *this;
}

void SegmentIteratorOverlapped::advanceRun()
{
	if (remainingRuns <= 0) {
		currentRun = nullptr;
		currentClusterInRun = 0;
		return;
	}
	remainingRuns--;
	currentRun++;
	this->openRun();
}

//Open currently selected run. It must have at least one segment.
void SegmentIteratorOverlapped::openRun()
{
	auto bytesPerCluster = mft->vol->volumeData().BytesPerCluster;
/*	if (remainingRuns <= 0) {
		remainingClustersInRun = mft->sizeInBytes() / mft->vol->volumeData().BytesPerCluster;
		for (size_t i = 0; i<mft->vcnMap().size() - 1; i++)
			remainingClustersInRun -= mft->vcnMap().at(i).len;
	}
	else*/
		currentClusterInRun = 0;
}

void SegmentIteratorOverlapped::advance()
{
	auto BytesPerCluster = this->mft->vol->volumeData().BytesPerCluster;

	//Part 1. Push read requests to the queue
	while (currentRun != nullptr) {
		auto offset = currentRun->lcnStart + currentClusterInRun;
		auto len = currentRun->len - currentClusterInRun;
		if (len > SEGMENTITERATOR_BATCHSIZE)
			len = SEGMENTITERATOR_BATCHSIZE;
		if (len > 0) { //in case of zero-length runs
			if (!this->reader->try_push_back(offset*BytesPerCluster, (uint32_t)(len*BytesPerCluster)))
				break;
			currentClusterInRun += len;
		}
		if (currentClusterInRun >= currentRun->len)
			this->advanceRun();
#ifdef SEGMENTITERATOR_TRACKPOS
		readClustersPushed += len;
		readsPushed++;
#endif
	}

	//Part 2. Finish reading the current buffer.
	if (remainingBufferData >= mft->BytesPerFileSegment) {
		segment = (FILE_RECORD_SEGMENT_HEADER*)((uint8_t*)segment + mft->BytesPerFileSegment);
		remainingBufferData -= mft->BytesPerFileSegment;
#ifdef SEGMENTITERATOR_TRACKPOS
		segmentAdvances++;
#endif
		//Not checking that remainingBufferData is a multiple of BytesPerFileSegment! This has to be true.
		return;
	}
	assert(remainingBufferData == 0);

	//Part 3. Wait for and extract the next queued read.

	//If we hold the previous buffer ptr then we took it from the reader and must return it.
	if (segment != nullptr) {
#ifdef SEGMENTITERATOR_TRACKPOS
		readsPopped++;
#endif
		reader->pop_front();
		segment = nullptr;
	}

	remainingBufferData = 0; //Going to cast it to 32 bit so zero it
	segment = (FILE_RECORD_SEGMENT_HEADER*)reader->finalize_front((uint32_t*)&remainingBufferData);
	if (segment == nullptr)
		return;
	assert(remainingBufferData % mft->BytesPerFileSegment == 0);
#ifdef SEGMENTITERATOR_TRACKPOS
	readsPulled++;
	readClustersPulled += remainingBufferData / mft->vol->volumeData().BytesPerCluster;
	readSegmentsPulled += remainingBufferData / mft->BytesPerFileSegment;
	segmentAdvances++;
#endif
	mft->processSegments(segment, (int)(remainingBufferData / mft->BytesPerFileSegment));
	remainingBufferData -= mft->BytesPerFileSegment; //Read this right now
}
