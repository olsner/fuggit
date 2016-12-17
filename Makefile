CXXFLAGS = -std=gnu++11 -g -Os -Wall
LIBS = -lcrypto -lz

all: fgt-make-commit-pack

%: %.cc
	$(CXX) $(LDFLAGS) $(CXXFLAGS) -o $@ $< $(LIBS)
