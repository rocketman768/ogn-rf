VERSION = 0.2.6

# USE_RPI_GPU_FFT = 1

CXX ?= g++
FLAGS = -Wall -O3 -ffast-math -Iinclude -DVERSION=$(VERSION)
LIBS  = -lpthread -lm -ljpeg -lconfig -lrt

ifneq ("$(wildcard /opt/vc/src/hello_pi/hello_fft)","")
USE_RPI_GPU_FFT = 1
FLAGS += -mcpu=arm1176jzf-s -mtune=arm1176jzf-s -march=armv6zk -mfpu=vfp
endif

#ifndef __MACH__ # _POSIX_TIMERS  # OSX has no clock_gettime() and thus not -lrt (but _POSIX_TIMERS seem not to be defined in make ?)
#LIBS += -lrt
#endif

ifdef USE_RPI_GPU_FFT
GPU_SRC = src/mailbox.c src/gpu_fft_base.c src/gpu_fft.c src/gpu_fft_twiddles.c src/gpu_fft_shaders.c
FLAGS += -DUSE_RPI_GPU_FFT
LIBS += -ldl
endif

all: gsm_scan ogn-rf r2fft_test

# Per-target source/lib ========================================================

# Source for ogn-rf
CSRC_ogn-rf = $(GPU_SRC)
CPPSRC_ogn-rf = src/ogn-rf.cc src/httpserver.cc src/rfacq.cc
LIB_ogn-rf = $(LIBS) -lrtlsdr -lfftw3 -lfftw3f
OBJ_ogn-rf = $(CSRC_ogn-rf:%.c=%.o) $(CPPSRC_ogn-rf:%.cc=%.opp)
DEP_ogn-rf = $(CSRC_ogn-rf:%.c=%.d) $(CPPSRC_ogn-rf:%.cc=%.dpp)

# Source for gsm-scan
CSRC_gsm-scan = $(GPU_SRC)
CPPSRC_gsm-scan = src/gsm_scan.cc
LIB_gsm-scan = $(LIBS) -lrtlsdr -lfftw3 -lfftw3f
OBJ_gsm-scan = $(CSRC_gsm-scan:%.c=%.o) $(CPPSRC_gsm-scan:%.cc=%.opp)
DEP_gsm-scan = $(CSRC_gsm-scan:%.c=%.d) $(CPPSRC_gsm-scan:%.cc=%.dpp)

# Source for r2fft_test
CSRC_r2fft-test =
CPPSRC_r2fft-test = src/r2fft_test.cc
LIB_r2fft-test = $(LIBS) -lfftw3 -lfftw3f
OBJ_r2fft-test = $(CSRC_r2fft-test:%.c=%.o) $(CPPSRC_r2fft-test:%.cc=%.opp)
DEP_r2fft-test = $(CSRC_r2fft-test:%.c=%.d) $(CPPSRC_r2fft-test:%.cc=%.dpp)


# Automatic targets ===========================================================

# Rule for generating obj files
%.o: %.c
	$(CXX) $(FLAGS) -o $@ -c $<
%.opp: %.cc
	$(CXX) $(FLAGS) -o $@ -c $<

# Rule for generating dep files
%.d: %.c
	@$(CXX) $(FLAGS) $< -MM -MT $(@:%.d=%.o) > $@
%.dpp: %.cc
	@$(CXX) $(FLAGS) $< -MM -MT $(@:%.dpp=%.o) > $@

# Include all dependency files
OBJ_ALL = $(sort $(OBJ_ogn-rf) $(OBJ_gsm-scan) $(OBJ_r2fft-test))
DEP_ALL = $(sort $(DEP_ogn-rf) $(DEP_gsm-scan) $(DEP_r2fft-test))
-include $(DEP_ALL)

# Real targets ================================================================

ogn-rf: Makefile $(OBJ_ogn-rf)
	$(CXX) $(FLAGS) -o $@ $(OBJ_ogn-rf) $(LIB_ogn-rf)
ifdef USE_RPI_GPU_FFT
	sudo chown root ogn-rf
	sudo chmod a+s  ogn-rf
endif

gsm_scan: Makefile $(OBJ_gsm-scan)
	$(CXX) $(FLAGS) -o $@ $(OBJ_gsm-scan) $(LIB_gsm-scan)
ifdef USE_RPI_GPU_FFT
	sudo chown root gsm_scan
	sudo chmod a+s gsm_scan
endif

r2fft_test:	Makefile $(OBJ_r2fft-test)
	$(CXX) $(FLAGS) -o $@ $(OBJ_r2fft-test) $(LIB_r2fft-test)

.PHONY: clean
clean:
	rm -f $(OBJ_ALL) $(DEP_ALL) ogn-rf gsm_scan r2fft_test

