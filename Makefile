# nanoCDJ
#
#   make ipod        build and package the app -> port/nanoapps/build/nanoCDJ.nanoapp (+ .zip)
#   make test        run the host unit tests (no iPod needed)
#   make test-mp3 MP3="track.mp3"   check the streaming decoder against a real MP3
#   make tools       build tools/grid_offset (beat-grid alignment check, macOS + Rekordbox)
#
# The iPod build needs arm-none-eabi-gcc, Python 3 with Pillow and pyelftools, and
# the NanoApps submodule (git submodule update --init). Tests need only a C compiler.

CC     ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
B      := tests/build

ipod:
	./port/nanoapps/build.sh
	python3 port/nanoapps/package.py
.PHONY: ipod

test: test-deck test-app test-ring test-port
.PHONY: test

$(B):
	mkdir -p $@

src/font5x7.h: tools/mkfont.py
	python3 tools/mkfont.py > $@

test-deck: | $(B)
	$(CC) $(CFLAGS) -o $(B)/test_deck tests/test_deck.c src/deck.c src/mp3stream.c src/anlz.c src/util.c -lm && ./$(B)/test_deck

test-app: | $(B)
	$(CC) $(CFLAGS) -Wno-unused-function -o $(B)/test_app tests/test_app.c src/deck.c src/mp3stream.c src/gfx.c src/anlz.c src/library.c src/util.c -lm && ./$(B)/test_app

# the audio output against a simulated RetailOS sound voice, for three counter behaviours
test-ring: | $(B)
	$(CC) -O1 -g -Wall -Wno-unused-function -Itests -o $(B)/test_ring tests/test_ring.c && ./$(B)/test_ring 1 && ./$(B)/test_ring 2 && ./$(B)/test_ring 3
	$(CC) -O1 -g -Wall -Wno-unused-function -Itests -DHANDOVER_TEST -DRING_BLOCK_MS=1000 -DRING_SLOTS=4 -DRING_HANDOVER_JOINS=40 -DSIM_JOIN_LIMIT=100 -o $(B)/test_ring_ho tests/test_ring.c && ./$(B)/test_ring_ho
	$(CC) -O1 -g -Wall -Wno-unused-function -Itests -DHANDOVER_TEST -DHO_QUICKCHECK -DRING_BLOCK_MS=200 -DRING_SLOTS=4 -DRING_HANDOVER_JOINS=200 -DSIM_JOIN_LIMIT=500 -o $(B)/test_ring_ho200 tests/test_ring.c && ./$(B)/test_ring_ho200

# the freestanding libc/libm of the iPod build, renamed so they don't clash with the host's
PORT_RENAME := -Dmalloc=t_malloc -Dfree=t_free -Dcalloc=t_calloc -Drealloc=t_realloc -Dqsort=t_qsort \
  -Dsnprintf=t_snprintf -Dvsnprintf=t_vsnprintf -Dfloor=t_floor -Dround=t_round -Dlround=t_lround \
  -Dfmod=t_fmod -Dsqrt=t_sqrt -Dhypot=t_hypot -Dexp=t_exp -Dsin=t_sin -Dcos=t_cos -Datan2=t_atan2 -Dfabs=t_fabs
test-port: | $(B)
	$(CC) -O1 -w -fno-builtin -Itests/portstub -Iport/nanoapps $(PORT_RENAME) -c port/nanoapps/libc.c -o $(B)/port_libc.o
	$(CC) -O1 -w -fno-builtin -Iport/nanoapps $(PORT_RENAME) -c port/nanoapps/libm.c -o $(B)/port_libm.o
	$(CC) -O1 -w tests/test_port_libc.c $(B)/port_libc.o $(B)/port_libm.o -o $(B)/test_port_libc -lm && ./$(B)/test_port_libc

# streamed decode vs a whole-file decode, jumps, cue buffer, iPod-speed reads, deck output.
# Several files: build once, then run tests/build/test_mp3s a.mp3 b.mp3 ...
test-mp3: | $(B)
	$(CC) -O2 -g -Wall -Wno-unused-function -o $(B)/test_mp3s tests/test_mp3s.c src/mp3stream.c src/deck.c src/anlz.c src/util.c -lm
	@if [ -n "$(MP3)" ]; then ./$(B)/test_mp3s "$(MP3)"; else echo 'usage: make test-mp3 MP3="path/to/track.mp3"'; fi
.PHONY: test-deck test-app test-ring test-port test-mp3

tools:
	mkdir -p tools/.build
	$(CC) -O2 -o tools/.build/grid_offset tools/grid_offset.c src/mp3stream.c src/anlz.c src/util.c -lm
.PHONY: tools

clean:
	rm -rf $(B) tools/.build
	./port/nanoapps/build.sh clean 2>/dev/null || true
.PHONY: clean
