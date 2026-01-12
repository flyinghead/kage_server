#
# dependencies: libasio-dev libdcserver
#
prefix = /usr/local
exec_prefix = $(prefix)
sbindir = $(exec_prefix)/sbin
sysconfdir = $(prefix)/etc
CFLAGS = -g -Wall -O3 -DNDEBUG # -fsanitize=address -static-libasan
CXXFLAGS = $(CFLAGS) -std=c++17
DEPS = blowfish.h model.h propa_rank.h discord.h log.h kage.h propa_auth.h outtrigger.h bomberman.h
USER = dcnet

all: kageserver ot_dissect

%.o: %.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

kageserver: kageserver.o blowfish.o model.o discord.o log.o outtrigger.o bomberman.o
	$(CXX) $(CXXFLAGS) -o $@ kageserver.o blowfish.o model.o discord.o log.o outtrigger.o bomberman.o -lpthread -ldcserver -Wl,-rpath,/usr/local/lib

ot_dissect: ot_dissect.o
	$(CXX) $(CXXFLAGS) -o $@ ot_dissect.o

clean:
	rm -f *.o kageserver ot_dissect kage.service

install: all
	mkdir -p $(DESTDIR)$(sbindir)
	install kageserver $(DESTDIR)$(sbindir)
	mkdir -p $(DESTDIR)$(sysconfdir)
	cp -n kage.cfg $(DESTDIR)$(sysconfdir)

kage.service: kage.service.in Makefile
	cp kage.service.in kage.service
	sed -e "s/INSTALL_USER/$(USER)/g" -e "s:SBINDIR:$(sbindir):g" -e "s:SYSCONFDIR:$(sysconfdir):g" < $< > $@

installservice: kage.service
	mkdir -p /usr/lib/systemd/system/
	cp $< /usr/lib/systemd/system/
	mkdir -p /var/log/kage
	chown $(USER):$(USER) /var/log/kage
	systemctl enable kage.service
