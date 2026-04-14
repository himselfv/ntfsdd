# NTFS Disk Destroyer
Compares and updates NTFS volume clones in a dangerously efficient fashion.

## Description
Given a source NTFS volume and a target clone of it, uses NTFS $Bitmap or compares the MFTs to generate a list of potentially changed clusters.
Compares these clusters on the source and the destination and copies the changes to the destination.

AKA in some apps: Rapid Delta Clone. (But this one has read-compare-write!)

The goals:

* to minimize the number of clusters checked by using progressively more sophisticated ways of selecting them
* to minimize writes, preserving SSDs.

Can be used for:

* Cloning/imaging: ``ntfsdd copy --select all``
* Volume comparison: ``ntfsdd compare --select all``/``compare --select bitmap``
* Smart updates: ``ntfsdd copy --select mft`` (for HDDs)/``ntfsdd rcw --select mft`` (for SSDs).
* Accidental HDD wipes.

In general you have to run this as Administrator. But start without that and see if your goals are achievable.

This tool is very much in beta. Test it read-only before doing any writes. Stick to its safety measures. Trust it when it says it's confused.



## Quickstart:

Clone SOURCE (volume, file, vss) to DEST (volume, file):
```
ntfsdd copy --select all --source SOURCE --dest DEST --write # All clusters
ntfsdd copy --select bitmap --source SOURCE --dest DEST --write # Only used
```

Compare the MFTs and copy potentially changed clusters blindly (useful for HDDs):
```
ntfsdd copy --select mft --source SOURCE --dest DEST --write
```

Compare the MFTs, then compare all the potentially changed clusters and copy the changes (useful for SSDs):
```
ntfsdd rvw --select mft --source SOURCE --dest DEST --write
```

Print the cluster lists matching given selection criteria:
```
ntfsdd list --select all/bitmap/mft
```

Compares the clusters matching the selection criteria and set exit code + print clusters + print affected files.
```
ntfsdd compare --select all/bitmap/mft --exit-code --print-clusters --cluster-spans --print-files-to filelist.txt
```



## The basics

The tool works in two steps:

1. Select the clusters for processing.
2. Process them.


#### Selecting the clusters (``--select``):

**all**
: All clusters on the source volume.

**bitmap**
: Clusters marked as used in $Bitmap on the source volume.

**mft**
: Clusters referenced in the MFT segments (~= files/dirs) which are different between the source and the destination. \
  Basically, all clusters referenced by the files that could have changed.

**antimft**
: Clusters marked as used in $Bitmap but NOT belonging to files and dirs which seem to have changed.

MFT is the most narrow mode and it's recommended unless it's not working for you somehow. All/Bitmap are useful for initial cloning/occasional verification.


#### The action to perform (``--action``):

**list**
: List the selected clusters.

**compare**
: Compare the selected clusters on the source and the destination. List the clusters with differences.

**copy**
: Copy *all* the selected clusters from the source to the destination. This is typically how clone resync works on HDDs. This is more efficient for the HDDs.

**rcw**
: Compare the selected clusters between the source and the destination and copy the changed clusters to the destination. This is better for the SSDs because it avoids writes unless neccessary.


#### Additional flags:

**--exit-code**
: Set non-zero exit code on non-empty results (differences, clusters copied).

**--print-clusters**
: Print resulting cluster addresses (selected/differing/copied ones). ``--cluster-spans``, ``--cluster-separator``, ``--print-clusters-to`` control specifics.

**--print-files**
: Print paths to files containing said clusters. ``--print-files-to`` to print to a file.

**--quiet --verbose --debug --progress**
: Verbosity levels + enables progress updates for long operations.

**--human-readable**
: Sizes in human-readable units instead of bytes.


#### Copy or RCW?
Most existing cloning tools and software/firmware mirror raids do the equivalent of **copy** (I checked), either for the whole drive (raids) or for the partition being cloned. This is more efficient for HDDs. On HDDs reads and writes have similar time costs so it's faster to read+write instead of read+read+compare+write. Writes on HDDs are more or less free and may even help by reinforcing the stored data.

SSDs can only sustain a limited number of writes through their lifetime. That number is not that high. You'll thrash the SSD fast by rewriting it often. Reads on SSDs are more or less free and usually faster than writes, so RCW makes much more sense for SSDs.




## Unmounting
As a safety measure the tool requires your destination volume to not have any mount points. This is to prevent accidental wiping of unintended volumes. It is advised to NOT dismount your destination automatically via script, as that negates the safety. Instead,

1. For a one-shot copy, dismount the volume manually. Make sure you have dismounted the correct drive letter.
2. If you're doing regular scripted updates, keep the target free of mount points permanently.

You're free to do otherwise of course. ``--unsafe-allow-mounted`` disables this check.


## Writing
As a safety measure, you have to pass ``--write`` to any command where you intend to make changes to the destination volume. Do not pass this on read-only commands.


## Raw cloning
The tool can be used for cloning. "``copy all``" will perform a forensic clone (all clusters) and "``copy bitmap``" will only copy the clusters used (recommended). In both cases, and especially after "``copy all``", ``defrag /Retrim`` is advised on a clone.

When doing a full clone as a safety measure you have to pass ``--overwrite``. No checks against the destination volume layout and MFT will then be made.


## Imaging and restoration
You can pass files both as source and as destination. This way you can image volumes, update and restore those images. The file has to exist:
```
type nul >filename.img
```

Note that "```copy all```" will produce a file equivalent to the source in size. "```copy bitmap```" might produce a smaller file as final empty unused sectors will not be copied. This may confuse some tools, idk. You'll also likely not be able to "```copy all```" from that file and will have to restore it with "```copy bitmap```". \
To prevent this, pre-fill the file with zeroes or do one initial ``copy all``.

#### HOW TO CREATE SMALL NTFS VOLUMES IN FILES W/VARIOUS PARAMS:
* Disk management MMC.
* Create virtual disk (VHD). VHDs are stored almost as is, only with additional footer. But we need partitions, not the entire disk.
* Create NTFS partitions w/different cluster sizes.
* ```fsutil volume list```
* ```dd if=\\.\{GUID} of=volumeName.img```
* Make sure it's ``\\.\`` and not ``\\?\``.


## VSS
It is recommended to use VSS snapshots as the source instead of the live volume:
```
--shadow
```
Auto-creates a temporary no-writer-cooperation shadow of the source. To enable writer cooperation (slower, flakier, but better consistency):
```
--shadow --shadow-writers
```

To create/delete a permanent shadow copy manually:
```
wmic shadowcopy call create Volume="C:\"
vssadmin list shadows
vssadmin delete shadows [...your shadow ids]
```



## Trim
This tool does NOT trim sectors that became free/had been free on full clone. The reasons for this are:

1. Trim is DANGEROUS and FAST. On modern SSDs trimmed data cannot be restored. One wrongly passed param and a second later your entire partition is irreversibly wiped. Do you want us messing with that? No, you don't.
2. Don't do what's already done well:
```
defrag /Retrim DriveLetter:
```
Trims all empty sectors on the target volume. Run after a full clone or from time to time after updates. I'd advise not running retrim after every update as: 1. The changes are likely limited and it's not that urgent. 2. This raises your chances to notice an update going awry and recover data before its trimmed. Just in case!

With regular and limited updates I think you can even retrim only occasionally, manually. If a non-trimmed "empty" sector is reused, for the SSD it simply looks like you're overwriting your own data. It's suboptimal (the SSD could rotate the backing sector with trim) but it's a normal work mode. You're doing this to files all the time when you change their contents.



## Ignoring/enforcing files
There's minimal ability to ignore files. Read carefully what it does! It's not meant as a full filename-based filtering. This is just to skip ```hiberfil.sys``` and ```pagefile.sys``` and such.

These options only work with MFT-based selection.

* ``--exclude-segment``, ``--exclude-file`` excludes this single file (all of its segments) OR DIR, ALONE.

* ``--exclude-subtree``, ``--exclude-path`` excludes this file (all of its segments) or dir, with all its contents recursively.

* ``--include-*`` forcefully marks files as dirty. Same zoo of versions as with ``--exclude``


What this ``--exclude`` does:

* MFT segments will be copied anyway. MFTs will always match perfectly. The cluster usage on the volumes must always match!

* The data clusters for these segments will not copied.

The file on the destination will become of its current size, referencing its current clusters, but those clusters will contain garbage left from their previous tenants.


WARNING: Security risk. Data from other files will leak into these. Might not matter if your destination is accessible only to the admin anyway, but think it through.

* Retrim regularly to lower the chance, but it's never zero.

* Manually ```del``` the file on the destination after cloning if you care.


WARNING: Havoc risk. When you skip a dir alone, its index is skipped. The files are still copied. Even if you skip the files, their MFT entries ARE still copied. These contain backreferences to the dirs including them. So now you have incorrect backreferences. PLUS incorrect obsolete forward-references. Or maybe even garbage. Perhaps you shouldn't skip dirs!

But if you mark all index allocations as dirty (``--all-index-dirty``) which is a useful thing to do sometimes, then dir indexes are effectively never skipped. Non-resident allocations because of this flag, and resident ones since they're in the MFT which is never skipped. So I think you can skip dirs in this case.


In the same, but safer, way you can force the file clusters to ALWAYS be selected with ``--include-``. Internally this is used to handle driver magic folders such as System Volume Information. This has absolutely NO side effects except for more data to rcw/copy each time.

* ``--standard-includes``: Assume ``$Extend``, ``System Volume Information`` and some other subtrees always dirty, as these sometimes can change without this being properly reflected in the MFT. On by default.

* ``--all-index-dirty``: Assume all non-resident $INDEX_ALLOCATIONs to be dirty. More clusters to check, but you will catch background updates to DUPLICATE_INFORMATION in directory indexes (which otherwise do not affect the dir's MFT). These are details cached for speed so *I think* you might be fine without that. Enable if you want to be safe.


**Q**: Can I pass file names and paths?\
**A**: Yeah, with limitations. Symlinks are not supported, pass real paths. A file referenced by ANY of its paths (hardlinks) will be skipped/dirtied. Doesn't matter if there are no rules for the other hardlinks.

**Q**: Why not delete ignored files?\
**A**: This requires editing the destination MFT, $Bitmap, the transaction log and getting involved in the NTFS internals much deeper than required for our simple cloning. Just delete the file with ```del``` later.

**Q**: Why don't you at least trim the clusters you're skipping, to prevent data leaks?\
**A**: See the Trim section. Best to stay away from trim.

**Q**: What files can I skip?\
**A**: ```hiberfil.sys``` and ```pagefile.sys```. When you boot from the clone, the system may not boot the first time around due to garbage in ``hiberfil.sys``. It should delete it on the second boot. If it fails to do so, help it.

**Q**: Why not skip ``$BadClus``?\
**A**: True: Copying the map of physical bad clusters elsewhere rarely makes sense. But keeping the old map also makes no sense! We're duplicating the source clusters precisely. What if those clusters are marked bad on the destination? Cluster-by-cluster copy just doesn't mesh with bad cluster maps. Thankfully, these days bad clusters are managed by the hardware and $BadClus in NTFS is unused. So we copy it like any other file.\
Honestly, we could skip it. Skip it at your peril. Anyway, I don't think it should even change, so why bother.

**Q**: Why is this only working with MFT selection modes?\
**A**: Not much sense in it in other modes. Bitmap already includes all there is to include. Bitmap+Exclusion might make some sense if you want "100% guaranteed coverage minus manually excluded paths" but it's a strange thing to do and currently not implemented.



## NTFS version support
I wrote this for my own uses, so I enabled NTFS versions on which I could check this. If you need other NTFS versions supported, send me something.



## NTFScmd
Companion app to dump and study NTFS internals.
```
ntfs --dump-segment --dump-cluster --print-segment --list-dir --help
```
Dumps binary (hex) and decoded representation of MFT segments, lists directory contents.



## Building
Compiles with MSVC2015/C++14.

All requirements are in Requirements.example.props, rename and provide local paths.

There are some tests, write more.

If you have ideas on how to setup testing on a large number of isolated real-life NTFS edge cases without keeping large volume images in the repo, suggest them.



## In-depth discussion

### Is the Bitmap selection system safe?
Bitmap selection is slow but should be 100% safe. We're comparing ALL clusters that the source file system says are in use. You really have to break NTFS fundamentally for this to be an incomplete clone.



### What the MFT comparison system does, precisely?
It reads the volume index (the $MFT) and compares the same file header records ("segments") on both volumes:

* The MFT itself is always "selected". This means ALL the segments ("file headers") will always be verified/copied.

* Segments not IN_USE on the source are skipped (see "Deletions" below)

* Identical segments are skipped

* If segments differ, all the external clusters referenced in the source segment are "selected".

* For multi-segment files, the decision is made for the entire set of file segments together.



### Is the MFT comparison system safe?
Will it really catch everything? Can't the data change without the MFT entry changing?
[See here](https://www.boku.ru/2026/04/02/ntfs-can-a-file-change-without-its-mft-record-also-changing/).

MFT comparison is a bit of a risk. For normal files and normal cases MFT checks are safe with very solid safety margins. There does not seem to be a mode where you can change anything substantial without the corresponding MFT entries very inevitably changing.

But there are exceptions. Driver magic: system files, cached DUPLICATED_INFORMATION in index entries. What these exceptions tell us is that there could be *more* exceptions. We handle all the exceptions we know about and it seems to cover everything, but there's no documented promise anywhere that says "No other files will receive driver magic".

Our hope is instead that:

1. We cover most of the major exceptions.
2. We leave the target volume in a consistent state.
3. Any files or clusters that slip through the cracks are mishandled predictably. Their contents will be garbage/old versions, but the volume will work.
4. Once the exceptions are noticed, they can also be handled. If they aren't noticed in time, the damage is limited.

To protect against this long term, do a ```compare --select antimft --exit-code``` scan from time to time to detect any slippages. Hopefully there should be none.

Another way to look at this system is: When you're running rclone, it compares every file by looking at its size and modification date to decide whether it needs copying. For normal files, we're doing much, much better than that. Of course, rclone does not cover system files at all. That's where the exceptions are.


### Eventual consistency
NTFS delays writing out some changes to the volume, from milliseconds for normal data to hours for LastAccessTime/LastModificationTime according to some reports. Is this not a problem? How consistent will be a clone made from a live system?

The clone made from a *raw live* disk will not be consistent at all! As you're copying the sectors, other sectors change and so your copy will be self-contradictory. Thankfully, the OS will usually not let you do that without locking.

The clone made from a locked disk, or any VSS snapshot, has *crash consistency*. It's what you get when you yank out the power cord from your PC:

* Anything not yet written to the disk will be lost.
* The OS will usually boot and function fine.
* Apps which were weren't designed with this in mind may sometimes have their data corrupted.

For VSS snapshots crash consistency is explicitly guaranteed. VSS has an added benefit that you CAN make a snapshot of an OS volume, while you cannot usually just lock it.

With crash consistency, $Bitmap will correctly identify used clusters, MFT segments will correctly account for all of them. However it only means that the volume itself remains healthy. VSS with writers tries to ensure that some critical services save their data properly before cloning. This is useful but covers only a handful cases, which are pretty resilient anyway.

**Q**: How do I get something better than a crash consistency? \
**A**: Unmount the source, remove it's drive letter and mount points. This ensures that very little to no apps use it. For the OS volume, if you want true consistency, shut the PC down and clone the volume from another OS.

**Q**: Little to no? Not "none"?
**A**: You can still access the volume by its ``\\?\`` name, and apps can still hold open handles from before. Dismounting is supposed to do something about that, but I bet there's an exception or two.

**Q**: How bad is crash consistency? \
**A**: It's pretty normal for live backups. Power outages happen, apps usually try to be at least somewhat resilient. Try to run updates at a time when there's minimal system activity to minimize the number of apps that can be affected.

VSS might not guarantee that some cached data, such as LastModificationTime, is written out. It's supposed to "prepare for backup" but there are no detailed guarantees about particulars of caching.

However, you're also not actually crashing. Those changes will get written out, just too late to be included into the backup this time. So what this tool cares about is *eventual consistency*. We will miss the changes in data these reflect, but we will catch them next time!

This is not a problem because the precise moment when the clone runs is arbitrary anyway: it could have happened a moment earlier when those changes had not yet been made, and we would have missed them anyway. So even though the changes *had* been made and we have not seen them *yet*, what matters is every change gets caught *eventually*.

The goal is to have no *permanently undetectable* changes.


### MFT comparison and deletions
The algorithm only compares segments which are IN_USE on the source. What about the segments that were IN_USE but are now not? Do we not need to free their segments?

No! We're selecting clusters to verify/copy. *Which* clusters are in use depends only on the $MFT itself and on $Bitmap, not the content of those clusters. We *always* sync the entire $MFT and $Bitmap, and so we always copy the entire actual picture of what clusters are now not in use. We do not need to copy those clusters.

Do we not need to update the segment on the destination? Yes we do, and we will, even without selecting its clusters: Again: We *always* sync the entire $MFT and $Bitmap.


### The destination immediately changing
If you do a ``compare`` immediately after ``rcw``/``clone``, you may notice there are changes - even if your source is the same VSS shadow which stayed constant. The OS does some minor bookkeeping once you unlock the destination volume. Normally it should be just a few $System files.
Filter drivers may do their own thing too, so if you're solving some weirdness, check out which filter drivers you do have installed.


### DUPLICATE_INFORMATION
Directories cache some information about the files they host, such as their sizes and last modification times. This does not always get updated in time. This is especially true because the file could be referenced from multiple dirs via hardlinks. This is fine because the source of the truth is the file entry itself. DUPLICATE_INFORMATION is used for directory size estimations and such things.

When the driver gets around to updating these fields, it can happen at any time, without any reasonable trigger, and the driver often does not change the directory's MFT. This is reasonable, as this update of the file's properties is not an update of the directory itself. Yet this change will be missed.

You can accept that (it's just cached info anyway, or at least we hope it's *just* cached info), or enable ``--all-index-dirty``. This will force all non-resident index allocations to be treated as dirty, catching these cases.
