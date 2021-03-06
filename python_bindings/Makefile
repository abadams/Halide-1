UNAME = $(shell uname)

THIS_MAKEFILE = $(realpath $(filter %Makefile, $(MAKEFILE_LIST)))
ROOT_DIR = $(strip $(shell dirname $(THIS_MAKEFILE)))
HALIDE_DIR ?= ..

ifeq ($(UNAME), Linux)
# Disable some warnings that are pervasive in Boost
CCFLAGS=$(shell python3-config --cflags) -I $(HALIDE_DIR)/include -I $(ROOT_DIR) -std=c++11 -fPIC -Wno-unused-local-typedef -Wno-shorten-64-to-32
PYTHON_VER=$(shell python3 --version | cut -d' ' -f2 | cut -b1,3)
LDFLAGS=$(shell python3-config --ldflags) -lboost_python-py$(PYTHON_VER) -lz
endif

ifeq ($(UNAME), Darwin)
# The /opt includes are in case this is a macports install of python and boost python
# Disable some warnings that are pervasive in Boost
CCFLAGS=$(shell python-config --cflags) -I $(HALIDE_DIR)/include -I /opt/local/include -I $(ROOT_DIR) -std=c++11 -Wno-unused-local-typedef -Wno-shorten-64-to-32
LDFLAGS=$(shell python-config --ldflags) -L /opt/local/lib -lboost_python3-mt -lz
endif

NUMPY_PATH=$(shell python3 -c "import numpy; print(numpy.__path__[0] + '/core/include')")

PY_SRCS=$(shell ls $(ROOT_DIR)/python/*.cpp)
PY_OBJS=$(PY_SRCS:$(ROOT_DIR)/python/%.cpp=build/py_%.o)
NUMPY_SRCS=$(shell ls $(ROOT_DIR)/halide_numpy/*.cpp)
NUMPY_OBJS=$(NUMPY_SRCS:$(ROOT_DIR)/halide_numpy/%.cpp=build/numpy_%.o)

build/py_%.o: $(ROOT_DIR)/python/%.cpp
	mkdir -p build
	$(CXX) $(CCFLAGS) -c $< -o $@

build/numpy_%.o: $(ROOT_DIR)/halide_numpy/%.cpp
	mkdir -p build
	$(CXX) $(CCFLAGS) -I $(NUMPY_PATH) -c $< -o $@

build/halide.so: $(PY_SRCS) $(PY_OBJS) $(NUMPY_SRCS) $(NUMPY_OBJS)
	mkdir -p build
	$(CXX) $(PY_OBJS) $(NUMPY_OBJS) $(LDFLAGS) $(HALIDE_DIR)/lib/libHalide.a -shared -o $@

clean:
	rm -rf build

test: build/halide.so
	$(ROOT_DIR)/run_apps.sh
	$(ROOT_DIR)/run_tutorial.sh
