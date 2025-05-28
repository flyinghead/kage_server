CFLAGS = -g -Wall -O3 -DNDEBUG # -fsanitize=address -static-libasan
CXXFLAGS = $(CFLAGS) -std=c++17
DEPS = asio.h blowfish.h model.h propa_rank.h discord.h log.h kage.h propa_auth.h outtrigger.h

all: kageserver ot_dissect

%.o: %.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

kageserver: kageserver.o blowfish.o model.o discord.o log.o outtrigger.o
	$(CXX) $(CXXFLAGS) -o $@ kageserver.o blowfish.o model.o discord.o log.o outtrigger.o -lpthread -lcurl

ot_dissect: ot_dissect.o
	$(CXX) $(CXXFLAGS) -o $@ ot_dissect.o

clean:
	rm -f *.o kageserver ot_dissect
