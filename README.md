VDRfuse
--------

 Fuse filesystem to "autoedit" VDR recordings!

 (c) Teemu Suikki 2017
 Based on the fuse example "passthrough_fh.c"

 Latest version:
 https://github.com/Zuikkis/vdrfuse


Using:
---------

This filesystem can be used to mount the VDR recordings directory. 
All other file operations work normally, but every "00001.ts" file is
replaced with edited version, according to "marks" file in the same
directory! The translation is done "on the fly", reading the original
file at correct positions.

There are some limitations:

- Directory must have "index", "info" and "marks" file.
- "info" file must have F line (fps)
- "marks" must have correct order. VDR does not require this..
- Recording must be single file, just 00001.ts!

If any of these requirements is not met, the file will be served unedited.
So it's not "fatal", you just need to watch commercials. :)

"marks" is re-read every time file is opened, so you can edit marks in VDR
and the change is instant when you re-open the video.

VDRfuse works perfectly with Plex Media Server, especially if combined with
VDR recordings scanner from my github.

Also recommended is Comskip, http://www.kaashoek.com/comskip/


Testing:
----------

If you just want to test it, create an empty directory for mountpoint and:
 
   vdrfuse -o modules=subdir,subdir=/srv/vdr/video  mntpoint

You can use "-f" switch to view some debug output. Then it won't run in
the background.

The "subdir" parameter is the VDR recording directory.


Installation:
--------------

You can mount it permanently in /etc/fstab:

vdrfuse      /srv/vdr/edited    fuse    allow_other,modules=subdir,subdir=/srv/vdr/video    0 2



