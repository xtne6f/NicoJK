CFLAGS := -O2 $(CFLAGS)
CXXFLAGS := -std=c++11 -Wall -Wextra -pedantic-errors -O2 $(CXXFLAGS)
LDFLAGS := -Wl,-s $(LDFLAGS)

all: jkrdlog
jkrdlog: jkrdlog.cpp ../LogReader.cpp ../TextFileReader.cpp zlib1__adler32.o zlib1__crc32.o zlib1__inffast.o zlib1__inflate.o zlib1__inftrees.o zlib1__ioapi.o zlib1__unzip.o zlib1__zutil.o
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH) -o $@ $^
zlib1__%.o: ../zlib1/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c -o $@ $<
clean:
	$(RM) jkrdlog zlib1__*.o
