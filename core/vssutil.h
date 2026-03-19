#pragma once
#include <Windows.h>
#include <string>
#pragma warning(push)
#pragma warning(disable: 4091)
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>
#pragma warning(pop)


enum VssSnapshotMode {
	WriterBackup,		//Writer participation: safer.
	NonWriterBackup,	//No writer participation: faster and the copy creation itself is more resilient.
};

class VssShadowCopy {
protected:
	VssSnapshotMode m_snapshotMode = NonWriterBackup;
	IVssBackupComponents* m_pBackup = nullptr;
	VSS_ID m_snapshotId;
	bool m_haveSnapshotId = false;
	VSS_SNAPSHOT_PROP m_prop;
	bool m_haveSnapshotProp = false;
public:
	VssShadowCopy();
	~VssShadowCopy();
	void setSnapshotMode(VssSnapshotMode mode) { this->m_snapshotMode = mode; };
	void create(const std::wstring& volumePath);
	void release();
	std::wstring snapshotPath() { return this->m_prop.m_pwszSnapshotDeviceObject; };
	//There's also m_pwszExposedPath but it's for when you request to mount it, and nullptr if you don't.
};
