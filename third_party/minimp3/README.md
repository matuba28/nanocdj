minimp3 by lieff — https://github.com/lieff/minimp3, CC0 (see LICENSE).
Commit afb604c06bc8beb145fecd42c0ceb5bda8795144, minimp3.h only.
Original sha256 5fb296a790734772b65a5514438cc06d14f24e539c13475ff2a7f737681c82c0.

One local change (line 1718): the decoder's scratch struct is `static` instead of
on the stack (~16 KB), because the iPod frame callback's stack size is unknown.
That makes mp3dec_decode_frame non-reentrant, which is fine: nanoCDJ decodes on
one thread.
