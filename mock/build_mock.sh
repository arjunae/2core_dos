./build_wk.sh

i586-pc-msdosdjgpp-g++ -m32 -O3 -msse2  -ffast-math mock_producer.cc \
    -I./include -I. \
    -L. -l2core \
    -o mock_pr.exe

