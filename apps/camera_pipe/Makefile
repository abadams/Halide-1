include ../support/Makefile.inc

all: $(BIN)/process

TIMING_ITERATIONS ?= 5

$(BIN)/camera_pipe.generator: camera_pipe_generator.cpp $(GENERATOR_DEPS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -g -fno-rtti $(filter-out %.h,$^) -o $@ $(LDFLAGS) $(HALIDE_SYSTEM_LDFLAGS)

$(BIN)/camera_pipe.a: $(BIN)/camera_pipe.generator
	@mkdir -p $(@D)
	$^ -g camera_pipe -o $(BIN) -f camera_pipe target=$(HL_TARGET) auto_schedule=false

$(BIN)/camera_pipe_auto_schedule.a: $(BIN)/camera_pipe.generator
	@mkdir -p $(@D)
	$^ -g camera_pipe -o $(BIN) -f camera_pipe_auto_schedule target=$(HL_TARGET)-no_runtime auto_schedule=true

$(BIN)/viz/camera_pipe.a: $(BIN)/camera_pipe.generator
	@mkdir -p $(@D)
	$^ -g camera_pipe -o $(BIN)/viz target=$(HL_TARGET)-trace_loads-trace_stores-trace_realizations

$(BIN)/process: process.cpp $(BIN)/camera_pipe.a $(BIN)/camera_pipe_auto_schedule.a
	$(CXX) $(CXXFLAGS) -Wall -O3 -I$(BIN) $^ -o $@ $(IMAGE_IO_FLAGS) $(LDFLAGS)

$(BIN)/viz/process: process.cpp $(BIN)/viz/camera_pipe.a
	$(CXX) $(CXXFLAGS) -DNO_AUTO_SCHEDULE -Wall -O3 -I$(BIN)/viz $^ -o $@ $(IMAGE_IO_FLAGS) $(LDFLAGS)

$(BIN)/out.png: $(BIN)/process
	$(BIN)/process $(IMAGES)/bayer_raw.png 3700 2.0 50 $(TIMING_ITERATIONS) $@ $(BIN)/h_auto.png

../../bin/HalideTraceViz:
	$(MAKE) -C ../../ bin/HalideTraceViz

$(BIN)/camera_pipe.mp4: $(BIN)/viz/process viz.sh $(HALIDE_TRACE_VIZ) ../../bin/HalideTraceViz
	bash viz.sh $(BIN)

clean:
	rm -rf $(BIN)

test: $(BIN)/out.png
