# NTFS Disk Destroyer
Compares and updates NTFS volume clones in a dangerously efficient fashion.

## Description
Given a source NTFS volume and a target clone of it, uses NTFS $Bitmap or compares the MFTs to generate a list of potentially changed clusters.
Compares these clusters between the source and the destination and copies the changes to the destination.

AKA in some apps: Rapid Delta Clone. (Only with read-compare-write!)

The goals:

* to minimize the number of clusters to be checked by using progressively more sophisticated ways of selecting them
* to minimize writes, preserving SSDs.

Can be used for:

* Cloning/imaging: ntfsdd copy all
* Volume comparison: ntfsdd compare all/compare bitmap
* Smart updates: ntfsdd copy mft (for HDDs)/ntfsdd rcw mft (for SSDs).
* Accidental HDD wipes.

In general you have to run this as Administrator. But start without that and see if your goals are satisfied.


### The basics

The tool works in two steps:

1. Select the clusters for processing.
2. Process them.

Selecting the clusters (``--select``):

**all**
: All clusters on the source volume.

**bitmap**
: Clusters marked as used in $Bitmap on the source volume.

**mft**:
: Clusters referenced in any of the MFT segments (~= files/dirs) which are different between the source and the destination. \
  Basically, all clusters referenced by the files that could have changed.

MFT is the most narrow and it's recommended unless it's not working for you somehow. All/Bitmap are useful for initial cloning.

The action to perform (``--action``):

**list**
: List the selected clusters.

**compare**
: Compare the selected clusters between the source and the destination. List the clusters with differences.

**copy**
: Copy *all* the selected clusters from the source to the destination. This is typically how clone resync works on HDDs. This is more efficient for the HDDs.

**rcw**
: Compare the selected clusters between the source and the destination and copy the changed clusters to the destination. This is better for the SSDs because it avoids writes unless neccessary.


#### Copy or RCW?
Most existing cloning tools and software/firmware mirror raids do the equivalent of **copy** (I checked), either for the whole drive (raids) or for the partition being cloned. This is more efficient for HDDs. On HDDs reads and writes have similar time costs so it's faster to read+write instead of read+read+compare+write. Writes on HDDs are more or less free and may even help by reinforcing the stored data.

SSDs can only sustain a limited number of writes through their lifetime. That number is not that high. You'll thrash the SSD fast by rewriting it often. Reads on SSDs are more or less free and usually faster than writes, so RCW makes much more sense for SSDs.



### Examples:

```
ntfsdd copy all --source SOURCE --dest DEST
```
Blindly clones SOURCE (volume, file, vss) to DEST (volume, file).

```
ntfsdd copy bitmap --source SOURCE --dest DEST
```
Same but copies only the clusters used by the filesystem.

```
ntfsdd copy mft --source SOURCE --dest DEST
```
Compares the MFTs and copies potentially changed clusters blindly (useful for HDDs).

```
ntfsdd rvw mft --source SOURCE --dest DEST
```
Compares the MFTs, then compares all the potentially changed clusters and copies the changes (useful for SSDs).

```
ntfsdd list all/bitmap/mft
```
Prints the cluster numbers matching given selection criteria.

```
ntfsdd compare all/bitmap/mft
```
Compares the clusters matching the selection criteria and prints the cluster numbers for the changed clusters.



## Unmounting
As a safety measure the tool requires your destination volume to not have any mount points. This is to prevent accidental wiping of unintended volumes. It is advised to NOT dismount your destination automatically via script, as that negates the safety. Instead,

1. For a one-shot copy, dismount the volume manually. Make sure you have dismounted the correct DriveLetter.
2. If you're doing regular scripted updates, keep the target free of mount points permanently.

You're free to do otherwise of course. ``--unsafe-allow-mounted`` disables this check.


## Cloning
The tool can be used for cloning. "``copy all``" will perform a forensic clone (all clusters) and "``copy bitmap``" will only copy the clusters used (recommended). In both cases, and especially after "``copy all``", defrag /Retrim is advised after clone.

When doing a full clone as a safety measure you have to pass ``--overwrite``. No checks against the destination volume layout and MFT will then be made.


## Imaging and restoration
You can pass files both as source and as destination. This way you can image volumes, update and restore those images. The file has to exist:
```
type nul >filename.img
```

Note that "copy all" will produce a file equivalent to the source in size. "copy bitmap" might produce a smaller file as final empty unused sectors will not be copied. This may confuse some tools, IDK. You'll also likely will not be able to "copy all" from that file and will have to restore it with "copy bitmap".
To prevent this, pre-fill the file with zeroes.

#### HOW TO CREATE SMALL NTFS VOLUMES IN FILES W/VARIOUS PARAMS:
* Disk management MMC.
* Create virtual disk (VHD). VHDs are stored almost as is, only with additional footer. But we need partitions, not the entire disk.
* Create NTFS partitions w/different cluster sizes.
* ```fsutil volume list```
* ```dd if=\\.\{GUID} of=volumeName.img```
* Make sure it's \\.\ and not \\?\.


## VSS
It is highly recommended to use Volume Shadow copies as the source instead of the live volume.
```
wmic shadowcopy call create Volume="C:\"
vssadmin list shadows
```
See also: vshadow.exe, which allows you to create a temporary in-memory shadow which is auto-deleted after your script finishes.


## Trim
This tool does NOT trim sectors that became free/had been free on full clone. The reasons for this are:

1. Trim is DANGEROUS and FAST. On modern SSDs trimmed data cannot be restored. One wrongly passed param and a second later your entire partition is irreversibly wiped. Do you want us messing with that? No.
2. Don't do what's already done well:
```
defrag /Retrim DriveLetter:
```
Trims all empty sectors on the target volume. Run after a full clone or from time to time after updates. I'd advise not running retrim after every update as: 1. The changes are likely limited and it's not that urgent. 2. This raises your chances to notice an update going awry and recover data before its trimmed. Just in case!

With regular and limited updates I think you can even retrim only occasionally, manually. If a non-trimmed empty sector is reused, 





## NTFS version support
I wrote this for my own uses, so I enabled NTFS versions on which I could have checked this. If you need other NTFS versions supported, send me something. 


## Building
Compiles with MSVC2015/C++14.
All requirements are in Requirements.example.props, rename and provide local paths.

There are some tests, write more.
