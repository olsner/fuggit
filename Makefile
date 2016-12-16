CXXFLAGS = -std=gnu++11 -g -Os
LIBS = -lcrypto

all: fgt-make-commit-pack

%: %.cc
	$(CXX) $(LDFLAGS) $(CXXFLAGS) -o $@ $< $(LIBS)
