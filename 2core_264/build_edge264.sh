./build_wk.sh
#-mno-avx -mno-avx2
i586-pc-msdosdjgpp-g++ -m32 -O3  -msse2  -ffast-math edge_prod.cc vesa.cc \
    -I../include -I. \
    -I/home/tho/dev/2CoreDOS/include \
    -L/home/tho/dev/2CoreDOS \
    -l2core \
    -o edge_pr.exe

