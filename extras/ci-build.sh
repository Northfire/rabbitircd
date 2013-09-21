#!/bin/sh

PWD=`pwd`

bash autogen.sh || exit 1
./configure \
	--with-showlistmodes \
	--enable-ssl \
	--enable-inet6 \
	--with-dpath=$PWD \
	--with-spath=$PWD/src/ircd \
	--with-nick-history=2000 \
	--with-sendq=3000000 \
	--with-permissions=0600 \
	--with-fd-setsize=1024 \
	--with-system-tre \
	--with-system-cares \
	--enable-dynamic-linking || exit 1
make || exit 1

exit 0

