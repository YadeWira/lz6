LZ6 - Library Files
================================

The __lib__ directory contains several files, but you don't necessarily need them all.

To integrate fast LZ6 compression/decompression into your program, you basically just need "**lz6.c**" and "**lz6.h**".

For more compression at the cost of compression speed (while preserving decompression speed), use **lz6hc** on top of regular lz6. `lz6hc` only provides compression functions. It also needs `lz6` to compile properly.

If you want to produce files or data streams compatible with `lz6` command line utility, use **lz6frame**. This library encapsulates lz6-compressed blocks into the [official interoperable frame format]. In order to work properly, lz6frame needs lz6 and lz6hc, and also **xxhash**, which provides error detection algorithm.
(_Advanced stuff_ : It's possible to hide xxhash symbols into a local namespace. This is what `liblz6` does, to avoid symbol duplication in case a user program would link to several libraries containing xxhash symbols.)

A more complex "lz6frame_static.h" is also provided, although its usage is not recommended. It contains definitions which are not guaranteed to remain stable within future versions. Use for static linking ***only***.

The other files are not source code. There are :

 - LICENSE : contains the BSD license text
 - Makefile : script to compile or install lz6 library (static or dynamic)
 - liblz6.pc.in : for pkg-config (make install)

[official interoperable frame format]: ../lz6_Frame_format.md
