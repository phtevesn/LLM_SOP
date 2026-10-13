CXX := g++
CXXFLAGS := -std=c++14 -Wall -Wextra -O2

TARGET := validate_precedence
SOURCE := validate_precedence.cpp

.PHONY: all build clean

all: $(TARGET)

build: $(TARGET)

$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) $(SOURCE) -o $(TARGET)

clean:
	rm -f $(TARGET)
