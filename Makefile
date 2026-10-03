CXX = g++
CXXFLAGS = -std=c++17 -g -O2 -Wall -Wextra -pthread

all: server

server: server.cpp

clean:
	rm -rf *.o *~ *.dSYM server
