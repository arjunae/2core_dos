#!/bin/sh
rm -rf *.o *.a 
#/home/tho/dev/djgpp-install/bin/i586-pc-msdosdjgpp-as apboot.s -o apboot.o
nasm -f coff apboot.asm -o apboot.o
/home/tho/dev/djgpp-install/bin/i586-pc-msdosdjgpp-gcc -c 2core.cc -o 2core.o
#/home/tho/dev/djgpp-install/bin/i586-pc-msdosdjgpp-gcc -O2 -c 2core_c_api.cc -o 2core_c_api.o
/home/tho/dev/djgpp-install/bin/i586-pc-msdosdjgpp-ar -rcs lib2core.a 2core.o  apboot.o
/home/tho/dev/djgpp-install/bin/i586-pc-msdosdjgpp-ranlib lib2core.a
