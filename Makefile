# Example command to build: make all -j9 && make deploy

release : uvc.rel.out debarcode 
release-cppt : uvc.cppt.out
debug-mt : uvc.mt.out 
debug-st : uvc.st.out 
debug-no : uvc.no.out
all : release release-cppt debug-mt debug-st debug-no

HDR=CmdLineArgs.hpp common.hpp grouping.hpp logging.hpp main.hpp main_conversion.hpp version.h CLI11-1.7.1/CLI11.hpp
SRC=CmdLineArgs.cpp common.cpp grouping.cpp logging.cpp main.cpp                     version.cpp 
DEP=bcf_formats.step1.hpp instcode.hpp Makefile

HTSPATH=ext/htslib-1.9-lowdep/libhts.a
HTSFLAGS=$(HTSPATH) -I ext/htslib-1.9-lowdep/ -pthread -lm -lz -lbz2 -llzma # -lcurl -lcrypto # can be changed depending on the specific installed components of htslib (please refer to the INSTALL file in htslib)
CC=gcc  # can be changed to clang or other compilers as needed
CXX=g++ # can be changed to clang or other compilers as needed
CXXFLAGS=-std=c++14 -static-libstdc++ -Wall
COMMIT_VERSION=$(shell git rev-parse HEAD | head -c 7)
COMMIT_DIFF_SH=$(shell git diff HEAD --shortstat)
COMMIT_DIFF_FULL=$(shell echo "R\"ZXF_specQUOTE(\n $$(git diff HEAD | sed 's/ZXF_specQUOTE/ZXF_specquote/g') \n)ZXF_specQUOTE\"" > gitdiff.txt)
VERFLAGS=-DCOMMIT_VERSION="\"$(COMMIT_VERSION)\"" -DCOMMIT_DIFF_SH="\"$(COMMIT_DIFF_SH)\"" -DCOMMIT_DIFF_FULL="\"$(COMMIT_DIFF_FULL)\""

debarcode  : debarcode_main.c version.h Makefile
	$(CC) -O3 -o debarcode $(VERFLAGS) debarcode_main.c ${HTSFLAGS}
	
# the main executable, uses OpenMP for multi-threading
uvc.rel.out : $(HDR) $(SRC) $(DEP)
	$(CXX) -O3 -DNDEBUG -o uvc.rel.out  $(CXXFLAGS) $(VERFLAGS) $(SRC) $(HTSFLAGS) -fopenmp # -l htslib

# the main executable, use C++ standard template library thread for multi-threading, useful if OpenMP runtime is not available
uvc.cppt.out : $(HDR) $(SRC) $(DEP)
	$(CXX) -O3 -DNDEBUG -o uvc.cppt.out $(CXXFLAGS) $(VERFLAGS) $(SRC) $(HTSFLAGS) -DUSE_STDLIB_THREAD # -l htslib

# single-thread executable with runtime assertions and debug symbols, very useful for debugging
uvc.st.out : $(HDR) $(SRC) $(DEP)
	$(CXX) -O2 -g -p    -o uvc.st.out   $(CXXFLAGS) $(VERFLAGS) $(SRC) $(HTSFLAGS) -fsanitize=address

uvc.no.out : $(HDR) $(SRC) $(DEP)
	$(CXX) -O0 -g -p    -o uvc.no.out   $(CXXFLAGS) $(VERFLAGS) $(SRC) $(HTSFLAGS) -fsanitize=address -Wextra

# multi-thread executable with runtime assertions and debug symbols, useful for debugging
uvc.mt.out : $(HDR) $(SRC) $(DEP)
	$(CXX) -O2 -g -p    -o uvc.mt.out   $(CXXFLAGS) $(VERFLAGS) $(SRC) $(HTSFLAGS) -fopenmp -fsanitize=address 

# generator for bcf templates
bcf_formats_generator1.out : bcf_formats_generator1.cpp version.h 
	$(CXX) -o bcf_formats_generator1.out $(CXXFLAGS) bcf_formats_generator1.cpp

# step1.hpp is the template code auto-generated by the generator for bcf templates
bcf_formats.step1.hpp : bcf_formats_generator1.out
	./bcf_formats_generator1.out > bcf_formats.step1.hpp

.PHONY: clean deploy

clean:
	rm bcf_formats_generator1.out bcf_formats.step1.hpp *.o *.out *.gch debarcode || true

# uvc1 is used by uvcTN.sh
deploy:
	cp uvc.rel.out bin/uvc1
	cp debarcode bin/debarcode
	cp uvc.cppt.out uvc.st.out uvc.mt.out uvc.no.out bin/

