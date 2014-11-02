
CPP  = g++
CC   = gcc
OBJ  = main.o
LINKOBJ  = $(OBJ)
LIBS =  -L"/usr/lib" -ljack
INCS =  -I"/usr/include"
CXXINCS =  $(INCS)
BIN  = jackiir
CXXFLAGS = $(CXXINCS) -O3 -ggdb -std=c++11 -msse -mfpmath=sse -ffast-math
CFLAGS = $(INCS) -O3 -ggdb
SOURCE = main.cpp
OBJECTS = $(SOURCES:.cpp=.o)
CONFIG = ./example.conf

.PHONY: all clean build run

build: $(BIN)

clean:
	rm -f $(OBJ) $(BIN)

$(BIN): $(OBJ)
	$(CPP) $(LINKOBJ) -o $(BIN) $(LIBS)

main.o: main.cpp $(SOURCE)
	$(CPP) -c main.cpp -o main.o $(CXXFLAGS)

run: $(BIN) $(CONFIG)
	./$(BIN) $(CONFIG)
