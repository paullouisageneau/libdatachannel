# getopt-for-windows
getopt.h and getopt.c is very often used in linux, to make it easy for windows user, two files were extracted from glibc. In order to make it works properly in windows, some modification was done and you may compare the change using original source files. Enjoy it!

Source: https://github.com/Chunde/getopt-for-windows.  IMPORTANT: getopt.[ch] are likely not safe for Linux due to conflict with existing getopt.[ch].  They are thus NOT in CMakeFiles.txt and instead both files are #include only on Windows.
