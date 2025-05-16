CFLAGS = -g -Wall -O3 # -fsanitize=address -static-libasan
CXXFLAGS = $(CFLAGS) -std=c++17
DEPS = asio.h blowfish.h model.h propa_rank.h discord.h log.h

all: kageserver

%.o: %.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

kageserver: kageserver.o blowfish.o model.o discord.o log.o
	$(CXX) $(CXXFLAGS) -o $@ kageserver.o blowfish.o model.o discord.o log.o -lpthread -lcurl

clean:
	rm -f *.o kageserver
