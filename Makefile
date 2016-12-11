CXXFLAGS = -std=gnu++11 -g -Os
LIBS = -lcrypto

all: geet-make-commit-pack

%: %.cc
	$(CXX) $(LDFLAGS) $(CXXFLAGS) -o $@ $< $(LIBS)
